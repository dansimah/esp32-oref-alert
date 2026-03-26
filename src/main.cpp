#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <nvs_flash.h>

// ── API Configuration ────────────────────────────────────────────────────────
static const char* ALERT_URL = "https://www.oref.org.il/warningMessages/alert/Alerts.json";
static const char* ALERT_REFERER = "https://www.oref.org.il/";

static const int RT_MESSAGE_CATEGORY = 10;
static const char* PRE_ALERT_KEYWORD = "\xD7\x91\xD7\x93\xD7\xA7\xD7\x95\xD7\xAA"; // "בדקות" UTF-8
static const int ALERT_CATEGORIES[] = {1, 3, 4, 5, 6, 7, 13};
static const int ALERT_CATEGORIES_COUNT = 7;

// ── LED Colors ───────────────────────────────────────────────────────────────
static const uint32_t COLOR_OFF    = 0x000000;
static const uint32_t COLOR_BLUE   = 0x0000FF;
static const uint32_t COLOR_GREEN  = 0x00FF00;
static const uint32_t COLOR_ORANGE = 0xFF8C00;
static const uint32_t COLOR_RED    = 0xFF0000;

// ── Polling Interval ─────────────────────────────────────────────────────────
static const unsigned long POLL_INTERVAL_MS = 2000;

// ── Device State ─────────────────────────────────────────────────────────────
enum DeviceState {
    STATE_AP_MODE,
    STATE_CONNECTING,
    STATE_OK,
    STATE_PRE_ALERT,
    STATE_ALERT
};

// ── Globals ──────────────────────────────────────────────────────────────────
Preferences prefs;
Adafruit_NeoPixel* strip = nullptr;
WebServer server(80);
DNSServer dnsServer;

String zoneName;
int ledPin;
int ttsSeconds;
int brightness;

DeviceState currentState = STATE_AP_MODE;
unsigned long lastPollTime = 0;
bool testRunning = false;
unsigned long testStartTime = 0;
bool wifiSetupDone = false;
bool apMode = false;
unsigned long lastBlinkTime = 0;
bool blinkOn = false;

// ── Forward Declarations ─────────────────────────────────────────────────────
void loadConfig();
void saveConfig();
void initLed();
void setLedColor(uint32_t color);
void setLedOff();
String fixAreaSpelling(const String& area);
bool isAlertCategory(int cat);
void pollAlertApi();
void updateLedForState();
void setupWebServer();
void handleGetRoot();
void handleGetConfig();
void handlePostConfig();
void handleGetStatus();
void handlePostTest();
void handleGetAreas();
void handlePostWifi();
void handlePostReset();
void handleCaptivePortal();
bool connectSTA(const char* ssid, const char* pass);
void startAP();
void onWiFiConnected();

// ── NVS Configuration ────────────────────────────────────────────────────────
void loadConfig() {
    if (prefs.begin("oref", true)) {
        zoneName = prefs.getString("zone_name", "\xD7\xAA\xD7\x9C \xD7\x90\xD7\x91\xD7\x99\xD7\x91 - \xD7\x99\xD7\xA4\xD7\x95 1");
        ledPin = prefs.getInt("led_pin", 48);
        ttsSeconds = prefs.getInt("tts_seconds", 90);
        brightness = prefs.getInt("brightness", 12);
        prefs.end();
    } else {
        zoneName = "\xD7\xAA\xD7\x9C \xD7\x90\xD7\x91\xD7\x99\xD7\x91 - \xD7\x99\xD7\xA4\xD7\x95 1";
        ledPin = 48;
        ttsSeconds = 90;
        brightness = 12;
    }
}

void saveConfig() {
    prefs.begin("oref", false);
    prefs.putString("zone_name", zoneName);
    prefs.putInt("led_pin", ledPin);
    prefs.putInt("tts_seconds", ttsSeconds);
    prefs.putInt("brightness", brightness);
    prefs.end();
}

// ── LED Control ──────────────────────────────────────────────────────────────
void initLed() {
    if (strip) {
        strip->clear();
        strip->show();
        delete strip;
    }
    strip = new Adafruit_NeoPixel(1, ledPin, NEO_GRB + NEO_KHZ800);
    strip->begin();
    strip->setBrightness(map(brightness, 1, 100, 1, 255));
    strip->show();
}

void setLedColor(uint32_t color) {
    if (!strip) return;
    strip->setPixelColor(0, color);
    strip->show();
}

void setLedOff() {
    setLedColor(COLOR_OFF);
}

// ── Area Spelling Fix (from HACS oref_alert) ─────────────────────────────────
String fixAreaSpelling(const String& area) {
    if (area.length() > 0 && area.charAt(0) == '\'') {
        return area.substring(1) + "'";
    }
    return area;
}

bool isAlertCategory(int cat) {
    for (int i = 0; i < ALERT_CATEGORIES_COUNT; i++) {
        if (ALERT_CATEGORIES[i] == cat) return true;
    }
    return false;
}

// ── WiFi (Arduino library) ───────────────────────────────────────────────────
bool connectSTA(const char* ssid, const char* password) {
    Serial.printf("[wifi] Connecting to '%s'...\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        updateLedForState();
        delay(250);
    }

    return WiFi.status() == WL_CONNECTED;
}

void startAP() {
    Serial.println("[wifi] Starting AP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("OrefAlert-Setup");

    dnsServer.start(53, "*", WiFi.softAPIP());
    apMode = true;

    Serial.printf("[wifi] AP started: OrefAlert-Setup (%s)\n",
                  WiFi.softAPIP().toString().c_str());
}

// ── Alert API Polling ────────────────────────────────────────────────────────
void pollAlertApi() {
    if (WiFi.status() != WL_CONNECTED) return;

    unsigned long now = millis();
    if (now - lastPollTime < POLL_INTERVAL_MS) return;
    lastPollTime = now;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    if (!http.begin(client, ALERT_URL)) return;

    http.addHeader("Referer", ALERT_REFERER);
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        payload.trim();

        if (payload.length() >= 3 &&
            (uint8_t)payload[0] == 0xEF &&
            (uint8_t)payload[1] == 0xBB &&
            (uint8_t)payload[2] == 0xBF) {
            payload = payload.substring(3);
        }

        if (payload.length() > 5) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload);

            if (!err && doc["data"].is<JsonArray>()) {
                int cat = 0;
                if (doc["cat"].is<int>()) {
                    cat = doc["cat"].as<int>();
                } else if (doc["cat"].is<const char*>()) {
                    cat = atoi(doc["cat"].as<const char*>());
                }
                String title = doc["title"].as<String>();
                JsonArray data = doc["data"].as<JsonArray>();

                bool zoneFound = false;
                for (JsonVariant area : data) {
                    String areaStr = fixAreaSpelling(area.as<String>());
                    if (areaStr == zoneName) {
                        zoneFound = true;
                        break;
                    }
                }

                if (zoneFound) {
                    if (cat == RT_MESSAGE_CATEGORY && title.indexOf(PRE_ALERT_KEYWORD) >= 0) {
                        currentState = STATE_PRE_ALERT;
                    } else if (isAlertCategory(cat)) {
                        currentState = STATE_ALERT;
                    } else if (cat == RT_MESSAGE_CATEGORY) {
                        currentState = STATE_OK;
                    }
                } else {
                    if (currentState == STATE_ALERT || currentState == STATE_PRE_ALERT) {
                        currentState = STATE_OK;
                    }
                }
            } else {
                if (currentState == STATE_ALERT || currentState == STATE_PRE_ALERT) {
                    currentState = STATE_OK;
                }
            }
        } else {
            if (currentState == STATE_ALERT || currentState == STATE_PRE_ALERT) {
                currentState = STATE_OK;
            }
        }
    }

    http.end();
}

// ── LED State Update ─────────────────────────────────────────────────────────
void updateLedForState() {
    if (testRunning) {
        unsigned long elapsed = millis() - testStartTime;
        if (elapsed < 1500) {
            setLedColor(COLOR_GREEN);
        } else if (elapsed < 3000) {
            setLedColor(COLOR_ORANGE);
        } else if (elapsed < 4500) {
            setLedColor(COLOR_RED);
        } else {
            testRunning = false;
            setLedColor(COLOR_GREEN);
        }
        return;
    }

    switch (currentState) {
        case STATE_AP_MODE:
            setLedColor(COLOR_BLUE);
            break;
        case STATE_CONNECTING: {
            unsigned long now = millis();
            if (now - lastBlinkTime >= 500) {
                lastBlinkTime = now;
                blinkOn = !blinkOn;
            }
            setLedColor(blinkOn ? COLOR_BLUE : COLOR_OFF);
            break;
        }
        case STATE_OK:
            setLedColor(COLOR_GREEN);
            break;
        case STATE_PRE_ALERT:
            setLedColor(COLOR_ORANGE);
            break;
        case STATE_ALERT:
            setLedColor(COLOR_RED);
            break;
    }
}

// ── Web Server Handlers ─────────────────────────────────────────────────────
void handleGetRoot() {
    File file = LittleFS.open("/index.html", "r");
    if (!file) {
        server.send(500, "text/plain", "Config page not found in LittleFS");
        return;
    }
    server.streamFile(file, "text/html");
    file.close();
}

void handleGetConfig() {
    JsonDocument doc;
    doc["zone_name"] = zoneName;
    doc["led_pin"] = ledPin;
    doc["tts_seconds"] = ttsSeconds;
    doc["brightness"] = brightness;
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handlePostConfig() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"no body\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    bool needReinit = false;

    if (doc["zone_name"].is<const char*>()) {
        zoneName = doc["zone_name"].as<String>();
    }
    if (doc["led_pin"].is<int>()) {
        int newPin = doc["led_pin"].as<int>();
        if (newPin != ledPin) {
            ledPin = newPin;
            needReinit = true;
        }
    }
    if (doc["tts_seconds"].is<int>()) {
        ttsSeconds = doc["tts_seconds"].as<int>();
    }
    if (doc["brightness"].is<int>()) {
        int newBri = constrain(doc["brightness"].as<int>(), 1, 100);
        if (newBri != brightness) {
            brightness = newBri;
            needReinit = true;
        }
    }

    saveConfig();

    if (needReinit) {
        initLed();
        setLedColor(COLOR_GREEN);
    } else if (strip) {
        strip->setBrightness(map(brightness, 1, 100, 1, 255));
        strip->show();
    }

    server.send(200, "application/json", "{\"ok\":true}");
}

void handleGetStatus() {
    JsonDocument doc;

    const char* stateStr;
    switch (currentState) {
        case STATE_AP_MODE:    stateStr = "ap_mode"; break;
        case STATE_CONNECTING: stateStr = "connecting"; break;
        case STATE_OK:         stateStr = "ok"; break;
        case STATE_PRE_ALERT:  stateStr = "pre_alert"; break;
        case STATE_ALERT:      stateStr = "alert"; break;
        default:               stateStr = "unknown"; break;
    }
    doc["state"] = stateStr;
    doc["uptime_sec"] = millis() / 1000;
    doc["ap_mode"] = apMode;

    if (WiFi.status() == WL_CONNECTED) {
        doc["rssi"] = WiFi.RSSI();
        doc["ip"] = WiFi.localIP().toString();
    } else if (apMode) {
        doc["rssi"] = 0;
        doc["ip"] = "192.168.4.1";
    }

    doc["zone_name"] = zoneName;

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handlePostTest() {
    testRunning = true;
    testStartTime = millis();
    server.send(200, "application/json", "{\"ok\":true,\"duration_ms\":4500}");
}

void handleGetAreas() {
    File file = LittleFS.open("/areas.json", "r");
    if (!file) {
        server.send(500, "application/json", "[]");
        return;
    }
    server.streamFile(file, "application/json");
    file.close();
}

void handlePostWifi() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"no body\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    String ssid = doc["ssid"].as<String>();
    String password = doc["password"].as<String>();

    if (ssid.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"ssid required\"}");
        return;
    }

    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("password", password);
    prefs.end();

    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Saved. Restarting...\"}");
    delay(1000);
    esp_restart();
}

void handlePostReset() {
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"WiFi cleared. Restarting to AP mode...\"}");
    delay(1000);
    esp_restart();
}

void handleCaptivePortal() {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
}

void setupWebServer() {
    server.on("/", HTTP_GET, handleGetRoot);
    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/config", HTTP_POST, handlePostConfig);
    server.on("/api/status", HTTP_GET, handleGetStatus);
    server.on("/api/test", HTTP_POST, handlePostTest);
    server.on("/api/areas", HTTP_GET, handleGetAreas);
    server.on("/api/wifi", HTTP_POST, handlePostWifi);
    server.on("/api/reset", HTTP_POST, handlePostReset);
    server.on("/generate_204", HTTP_GET, handleCaptivePortal);
    server.on("/fwlink", HTTP_GET, handleCaptivePortal);
    server.on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
    server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
    server.on("/canonical.html", HTTP_GET, handleCaptivePortal);
    server.on("/success.txt", HTTP_GET, handleCaptivePortal);
    server.onNotFound([]() {
        if (apMode) {
            handleCaptivePortal();
        } else {
            server.send(404, "text/plain", "Not found");
        }
    });
    server.begin();
}

// ── WiFi Connected Handler ───────────────────────────────────────────────────
void onWiFiConnected() {
    currentState = STATE_OK;
    setLedColor(COLOR_GREEN);

    Serial.printf("[wifi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin("oref-alert")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[wifi] mDNS: http://oref-alert.local/");
    }

    wifiSetupDone = true;
    lastPollTime = 0;
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== ESP32 Oref Alert LED ===");
    Serial.printf("[boot] Chip: %s, Flash: %d MB, Free heap: %d\n",
                  ESP.getChipModel(), ESP.getFlashChipSize() / 1048576, ESP.getFreeHeap());
    Serial.printf("[boot] Reset reason: %d\n", esp_reset_reason());

    esp_err_t nvsErr = nvs_flash_init();
    if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("[boot] NVS corrupted, erasing and re-initializing...");
        nvs_flash_erase();
        nvs_flash_init();
    }
    Serial.printf("[boot] NVS init: %s\n", esp_err_to_name(nvsErr));

    Serial.print("[boot] LittleFS... ");
    if (!LittleFS.begin(true)) {
        Serial.println("FAILED");
    } else {
        Serial.println("OK");
    }

    loadConfig();
    Serial.printf("[boot] Zone: %s, LED pin: %d\n", zoneName.c_str(), ledPin);

    initLed();
    setLedColor(COLOR_BLUE);
    Serial.println("[boot] LED blue");

    String storedSsid = "";
    String storedPass = "";
    if (prefs.begin("wifi", true)) {
        storedSsid = prefs.getString("ssid", "");
        storedPass = prefs.getString("password", "");
        prefs.end();
    }

    bool connected = false;
    if (storedSsid.length() > 0) {
        currentState = STATE_CONNECTING;
        connected = connectSTA(storedSsid.c_str(), storedPass.c_str());
    }

    if (connected) {
        onWiFiConnected();
    } else {
        Serial.println("[boot] No WiFi — starting AP mode");
        startAP();
        currentState = STATE_AP_MODE;
        setLedColor(COLOR_BLUE);
    }

    setupWebServer();
    Serial.println("[boot] Ready");
}

// ── Main Loop ────────────────────────────────────────────────────────────────
void loop() {
    if (apMode) {
        dnsServer.processNextRequest();
    }

    server.handleClient();

    if (wifiSetupDone) {
        if (WiFi.status() != WL_CONNECTED) {
            currentState = STATE_CONNECTING;
            WiFi.reconnect();
            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
                updateLedForState();
                delay(250);
            }
            if (WiFi.status() == WL_CONNECTED) {
                currentState = STATE_OK;
            }
        }

        pollAlertApi();
    }

    updateLedForState();
    delay(10);
}
