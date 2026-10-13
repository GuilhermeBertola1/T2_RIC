#include <Arduino.h>
#include <Wire.h>
#include <time.h>

#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <SSD1306.h>

#include <WiFi.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>

// ------------------------------ WiFi ---------------------------------
const char *WIFI_SSID = "ssid";
const char *WIFI_PASS = "babyyoda";

// ------------------------------ OTA ----------------------------------
#define OTA_HOSTNAME "esp-lora"
#define OTA_SENHA "AmandaBertola"
#define FW_VERSAO "1.0.0"

// ------------------------------ Pinos --------------------------------
#define LEDPIN 25
#define BUTTON 0

// BME280 (I2C secundário)
#define SEALEVELPRESSURE_HPA (1029.9)   // Sorocaba
#define I2C_SDA 21
#define I2C_SCL 22
TwoWire I2CBME = TwoWire(1);
Adafruit_BME280 bme;

// OLED (I2C primário)
#define OLED_I2C_ADDR 0x3C
#define OLED_RESET    16
#define OLED_SDA      4
#define OLED_SCL      15
SSD1306 display(OLED_I2C_ADDR, OLED_SDA, OLED_SCL);

// --------------------------- Temporizações ---------------------------
const unsigned long INTERVALO_LEITURA = 1000UL;
const unsigned long DEBOUNCE_MS       = 50UL;

// =====================================================================
//  OBJETOS GLOBAIS
// =====================================================================
struct DadosBME {
    float temperatura;
    float pressao;
    float altitude;
    float umidade;
};

struct Estatistica {
    uint32_t n     = 0;
    double   media = 0.0;
    double   m2    = 0.0;
    double   minimo = 0.0;
    double   maximo = 0.0;

    void adicionar(double x) {
        if (n == 0) {
            minimo = maximo = x;
        } else {
            if (x < minimo) minimo = x;
            if (x > maximo) maximo = x;
        }
        n++;
        double delta = x - media;
        media += delta / (double)n;
        m2    += delta * (x - media);
    }
    double desvioPadrao() const {
        return (n < 2) ? 0.0 : sqrt(m2 / (double)(n - 1));
    }
    void reiniciar() {
        n = 0; media = 0.0; m2 = 0.0; minimo = 0.0; maximo = 0.0;
    }
};

AsyncWebServer server(80);

Estatistica estatTemp, estatUmid, estatPres, estatAlt;
String horaInicioJanela = "";

DadosBME ultimaLeitura = {0, 0, 0, 0};
bool sensorOk  = false;
bool displayOk = false;

bool ledEstado = false;
bool botaoEstado = false;
int  botaoLeituraAnterior = HIGH;
unsigned long tempoUltimaMudanca = 0;
uint16_t botaoContagemPressoes = 0;

unsigned long tempoAnteriorLeitura = 0;

DadosBME readSensorBME();
void     atualizarBotao();
void     setLed(bool ligado);
void     acumularEstatisticas(const DadosBME &d);
void     atualizarDisplay();
float    pontoOrvalho(float t, float rh);
String   horaFormatada();
void     configurarRotas();

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nIniciando...");

    pinMode(LEDPIN, OUTPUT);
    digitalWrite(LEDPIN, LOW);

    pinMode(BUTTON, INPUT_PULLUP);
    botaoLeituraAnterior = digitalRead(BUTTON);

    // ------------------------------ OLED -----------------------------
    pinMode(OLED_RESET, OUTPUT);
    digitalWrite(OLED_RESET, LOW);
    delay(50);
    digitalWrite(OLED_RESET, HIGH);
    delay(50);

    displayOk = display.init();
    if (!displayOk) {
        Serial.println("Display indisponivel!");
    } else {
        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(0, 0, "Display OLED - OK");
        display.display();
        Serial.println("Display OLED - OK!");
    }

    // ----------------------------- BME280 ----------------------------
    I2CBME.begin(I2C_SDA, I2C_SCL, 100000);
    sensorOk = bme.begin(0x76, &I2CBME);
    if (!sensorOk) {
        Serial.println("Sensor BME280 - NAO OK (seguindo com valores 0)");
        if (displayOk) display.drawString(0, 12, "BME280 - Nao OK");
    } else {
        Serial.println("Sensor BME280 - OK");
        if (displayOk) display.drawString(0, 12, "Sensor BME280 - OK");
    }
    if (displayOk) display.display();

    // ------------------------------ WiFi -----------------------------
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Conectando");
    if (displayOk) { display.drawString(0, 24, "Conectando Wi-Fi..."); display.display(); }

    unsigned long inicio = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - inicio > 20000UL) {
            Serial.println("\nWi-Fi falhou, reiniciando...");
            ESP.restart();
        }
        Serial.print(".");
        delay(500);
    }
    Serial.println();
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // ------------------------------- NTP -----------------------------
    configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Sincronizando hora via NTP");
    inicio = millis();
    while (time(nullptr) < 1700000000 && millis() - inicio < 10000UL) {
        Serial.print(".");
        delay(300);
    }
    if (time(nullptr) >= 1700000000) Serial.printf(" OK -> %s\n", horaFormatada().c_str());
    else                             Serial.println(" sem NTP (seguindo sem hora)");

    // ------------------------- Servidor e OTA ------------------------
    configurarRotas();
    server.begin();

    ultimaLeitura = readSensorBME();
    atualizarDisplay();
    Serial.printf("Pronto! Dados em http://%s/dados\n", WiFi.localIP().toString().c_str());
}

// =====================================================================
//  LOOP
// =====================================================================
void loop() {
    ElegantOTA.loop();
    atualizarBotao();

    unsigned long agora = millis();
    if (agora - tempoAnteriorLeitura >= INTERVALO_LEITURA) {
        tempoAnteriorLeitura = agora;
        ultimaLeitura = readSensorBME();
        if (sensorOk) acumularEstatisticas(ultimaLeitura);
        atualizarDisplay();
    }
}

// ROTAS
void configurarRotas() {
    server.on("/dados", HTTP_GET, [](AsyncWebServerRequest *request) {
        DadosBME d = ultimaLeitura;
        JsonDocument doc;
        doc["sensor_ok"]     = sensorOk;
        doc["temperatura"]   = d.temperatura;
        doc["umidade"]       = d.umidade;
        doc["pressao"]       = d.pressao;
        doc["altitude"]      = d.altitude;
        doc["ponto_orvalho"] = pontoOrvalho(d.temperatura, d.umidade);
        doc["botao"]         = botaoEstado;
        doc["toques_botao"]  = botaoContagemPressoes;
        doc["led"]           = ledEstado;
        doc["rssi"]          = WiFi.RSSI();
        doc["uptime_s"]      = millis() / 1000;
        doc["heap_livre"]    = ESP.getFreeHeap();
        doc["ip"]            = WiFi.localIP().toString();
        doc["versao"]        = FW_VERSAO;

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    server.on("/stats", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["amostras"] = estatTemp.n;
        doc["inicio"]   = horaInicioJanela;
        doc["toques_botao"] = botaoContagemPressoes;
        const char *nomes[] = {"temperatura", "umidade", "pressao", "altitude"};
        const Estatistica *est[] = {&estatTemp, &estatUmid, &estatPres, &estatAlt};
        for (int i = 0; i < 4; i++) {
            JsonObject o = doc[nomes[i]].to<JsonObject>();
            o["media"] = est[i]->media;
            o["dp"]    = est[i]->desvioPadrao();
            o["min"]   = est[i]->minimo;
            o["max"]   = est[i]->maximo;
        }
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    server.on("/stats/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        estatTemp.reiniciar(); estatUmid.reiniciar();
        estatPres.reiniciar(); estatAlt.reiniciar();
        botaoContagemPressoes = 0;
        horaInicioJanela = "";
        request->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/led", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("estado")) {
            request->send(400, "text/plain", "Use /led?estado=0 ou /led?estado=1");
            return;
        }
        String e = request->getParam("estado")->value();
        setLed(e == "1" || e == "true");
        request->send(200, "application/json", ledEstado ? "{\"led\":true}" : "{\"led\":false}");
    });

    server.on("/reiniciar", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", "{\"reiniciando\":true}");
        request->onDisconnect([]() { ESP.restart(); });
    });

    ElegantOTA.begin(&server, OTA_HOSTNAME, OTA_SENHA);

    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Rota nao encontrada");
    });
}

// =====================================================================
//  SENSOR
// =====================================================================
DadosBME readSensorBME() {
    DadosBME leitura = {0, 0, 0, 0};
    if (!sensorOk) return leitura;

    leitura.temperatura = bme.readTemperature();
    leitura.pressao     = bme.readPressure() / 100.0F;
    leitura.altitude    = bme.readAltitude(SEALEVELPRESSURE_HPA);
    leitura.umidade     = bme.readHumidity();
    return leitura;
}

// Ponto de orvalho pela fórmula de Magnus
float pontoOrvalho(float t, float rh) {
    if (rh <= 0) return 0;
    const float a = 17.62, b = 243.12;
    float g = log(rh / 100.0) + a * t / (b + t);
    return b * g / (a - g);
}

// =====================================================================
//  DISPLAY
// =====================================================================
void atualizarDisplay() {
    if (!displayOk) return;
    const DadosBME &d = ultimaLeitura;
    display.clear();
    display.drawString(0, 0,  "IP: " + WiFi.localIP().toString());
    if (sensorOk) {
        display.drawString(0, 12, "T: " + String(d.temperatura, 1) + " C   U: " + String(d.umidade, 0) + " %");
        display.drawString(0, 24, "P: " + String(d.pressao, 1) + " hPa");
        display.drawString(0, 36, "Alt: " + String(d.altitude, 0) + " m");
    } else {
        display.drawString(0, 12, "BME280 nao encontrado");
    }
    display.drawString(0, 50, String("LED ") + (ledEstado ? "ON" : "OFF") + "   fw " FW_VERSAO);
    display.display();
}

// =====================================================================
//  BOTÃO E LED
// =====================================================================
void atualizarBotao() {
    int leitura = digitalRead(BUTTON);

    if (leitura != botaoLeituraAnterior) {
        tempoUltimaMudanca = millis();
        botaoLeituraAnterior = leitura;
    }

    if (millis() - tempoUltimaMudanca >= DEBOUNCE_MS) {
        bool novoEstado = (leitura == LOW);
        if (novoEstado != botaoEstado) {
            botaoEstado = novoEstado;
            if (botaoEstado) botaoContagemPressoes++;
        }
    }
}

void setLed(bool ligado) {
    ledEstado = ligado;
    digitalWrite(LEDPIN, ligado ? HIGH : LOW);
}

// =====================================================================
//  ESTATÍSTICA
// =====================================================================
void acumularEstatisticas(const DadosBME &d) {
    if (estatTemp.n == 0) horaInicioJanela = horaFormatada();

    estatTemp.adicionar(d.temperatura);
    estatUmid.adicionar(d.umidade);
    estatPres.adicionar(d.pressao);
    estatAlt.adicionar(d.altitude);
}

// =====================================================================
//  HORA
// =====================================================================
String horaFormatada() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    return String(buf);
}
