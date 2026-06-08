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
#include <time.h>

// ── API Configuration ────────────────────────────────────────────────────────
static const char* ALERT_URL = "https://www.oref.org.il/warningMessages/alert/Alerts.json";
static const char* HISTORY_URL = "https://www.oref.org.il/warningMessages/alert/History/AlertsHistory.json";
static const char* ALERT_REFERER = "https://www.oref.org.il/";

// History categories (from HACS oref_alert categories.py)
static const int PRE_ALERT_HISTORY_CAT = 14;
static const int END_HISTORY_CAT = 13;
static const int FIRST_DRILL_CATEGORY = 15;
static const int RT_SKIP_CATEGORY = 10; // ambiguous real-time "info" category

static const int HISTORY_ALERT_CATEGORIES[] = {1, 2, 3, 4, 7, 8, 9, 10, 11, 12};
static const int HISTORY_ALERT_CATEGORIES_COUNT = 10;

struct RtToHistoryMap {
    int rt;
    int history;
};
static const RtToHistoryMap RT_TO_HISTORY[] = {
    {1, 1}, {3, 7}, {4, 9}, {5, 11}, {6, 2}, {7, 12}, {13, 10}
};
static const int RT_TO_HISTORY_COUNT = 7;

static const int HISTORY_MAX = 10;

// ── LED Colors ───────────────────────────────────────────────────────────────
static const uint32_t COLOR_OFF    = 0x000000;
static const uint32_t COLOR_BLUE   = 0x0000FF;
static const uint32_t COLOR_GREEN  = 0x00FF00;
static const uint32_t COLOR_ORANGE = 0xFF8C00;
static const uint32_t COLOR_RED    = 0xFF0000;

// ── Polling & Expiration (matching HACS oref_alert integration) ──────────────
static const unsigned long POLL_ACTIVE_MS       = 2000;
static const unsigned long POLL_IDLE_MS         = 20000;
static const unsigned long IDLE_THRESHOLD_MS    = 10UL * 60 * 1000;
static const unsigned long PRE_ALERT_EXPIRE_MS  = 20UL * 60 * 1000;
static const unsigned long ALERT_EXPIRE_MS      = 180UL * 60 * 1000;

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
unsigned long stateStartTime = 0;
unsigned long lastActiveTime = 0;
bool testRunning = false;
unsigned long testStartTime = 0;
bool wifiSetupDone = false;
bool apMode = false;
unsigned long lastBlinkTime = 0;
bool blinkOn = false;
unsigned long lastHistoryPollTime = 0;

struct HistoryEntry {
    String date;
    String title;
    int category;
};
HistoryEntry historyEntries[HISTORY_MAX];
int historyCount = 0;
int lastHistoryHttpCode = 0;
size_t lastHistoryBytesRead = 0;
bool lastHistoryPatternFound = false;
char lastHistoryError[80] = "";
bool historyPollEverRan = false;

enum HistoryPollState {
    HIST_IDLE,
    HIST_CONNECTING,
    HIST_SEND_REQUEST,
    HIST_READ_STATUS,
    HIST_READ_HEADERS,
    HIST_READ_BODY
};
static HistoryPollState histState = HIST_IDLE;
static WiFiClientSecure histClient;
static String histScanBuffer;
static String histZonePattern;
static bool histStateApplied = false;
static uint8_t histBomRemaining = 3;
static unsigned long histStateTime = 0;

// ── Forward Declarations ─────────────────────────────────────────────────────
void loadConfig();
void saveConfig();
void initLed();
void setLedColor(uint32_t color);
void setLedOff();
String fixAreaSpelling(const String& area);
bool isHistoryAlertCat(int cat);
int realtimeToHistoryCat(int rtCat);
const char* historyRecordType(int category);
bool isHistoryRecordActive(const String& alertDate, int category);
int parseJsonCategory(JsonVariant v);
void applyHistoryRecord(const String& alertDate, const String& title, int category, bool& stateApplied);
void scanHistoryBuffer(String& buffer, const String& zonePattern, bool& stateApplied);
void trimHistoryBuffer(String& buffer, const String& zonePattern);
void setAlertState(DeviceState newState);
void checkStateExpiration();
unsigned long currentPollInterval();
String fetchOrefPayload(const char* url);
void pollAlertApi();
void pollHistoryApi();
void updateHistoryPoll();
void syncTime();
void updateLedForState();
void setupWebServer();
void handleGetRoot();
void handleGetConfig();
void handlePostConfig();
void handleGetStatus();
void handleGetHistory();
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
        zoneName.trim();
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

bool isHistoryAlertCat(int cat) {
    if (cat <= 0 || cat >= FIRST_DRILL_CATEGORY) return false;
    for (int i = 0; i < HISTORY_ALERT_CATEGORIES_COUNT; i++) {
        if (HISTORY_ALERT_CATEGORIES[i] == cat) return true;
    }
    return false;
}

int realtimeToHistoryCat(int rtCat) {
    if (rtCat == RT_SKIP_CATEGORY) return -1;
    for (int i = 0; i < RT_TO_HISTORY_COUNT; i++) {
        if (RT_TO_HISTORY[i].rt == rtCat) return RT_TO_HISTORY[i].history;
    }
    return -1;
}

const char* historyRecordType(int category) {
    if (category == PRE_ALERT_HISTORY_CAT) return "pre_alert";
    if (category == END_HISTORY_CAT) return "end";
    if (isHistoryAlertCat(category)) return "alert";
    return "other";
}

time_t parseAlertDate(const String& alertDate) {
    if (alertDate.length() < 19) return 0;

    struct tm tm = {};
    tm.tm_year = alertDate.substring(0, 4).toInt() - 1900;
    tm.tm_mon = alertDate.substring(5, 7).toInt() - 1;
    tm.tm_mday = alertDate.substring(8, 10).toInt();
    tm.tm_hour = alertDate.substring(11, 13).toInt();
    tm.tm_min = alertDate.substring(14, 16).toInt();
    tm.tm_sec = alertDate.substring(17, 19).toInt();
    tm.tm_isdst = -1;
    return mktime(&tm);
}

bool isHistoryRecordActive(const String& alertDate, int category) {
    if (category == END_HISTORY_CAT) return false;

    time_t now = time(nullptr);
    if (now < 100000) return false;

    time_t recordTime = parseAlertDate(alertDate);
    if (recordTime <= 0) return false;

    unsigned long ageSec = (unsigned long)(now - recordTime);
    if (category == PRE_ALERT_HISTORY_CAT) {
        return ageSec <= (PRE_ALERT_EXPIRE_MS / 1000);
    }
    if (isHistoryAlertCat(category)) {
        return ageSec <= (ALERT_EXPIRE_MS / 1000);
    }
    return false;
}

void syncTime() {
    configTzTime("IST-2IDT,M3.4.4/26,M10.5.0", "pool.ntp.org", "time.google.com");
    struct tm timeinfo;
    for (int i = 0; i < 20; i++) {
        if (getLocalTime(&timeinfo)) {
            Serial.println("[time] NTP synced");
            return;
        }
        delay(250);
    }
    Serial.println("[time] NTP sync failed");
}

int parseJsonCategory(JsonVariant v) {
    if (v.is<int>()) return v.as<int>();
    if (v.is<const char*>()) return atoi(v.as<const char*>());
    return 0;
}

void applyHistoryRecord(const String& alertDate, const String& title, int category, bool& stateApplied) {
    if (historyCount < HISTORY_MAX) {
        historyEntries[historyCount].date = alertDate;
        historyEntries[historyCount].title = title;
        historyEntries[historyCount].category = category;
        historyCount++;
    }

    if (stateApplied) return;
    stateApplied = true;

    if (category == END_HISTORY_CAT) {
        currentState = STATE_OK;
    } else if (category == PRE_ALERT_HISTORY_CAT && isHistoryRecordActive(alertDate, category)) {
        setAlertState(STATE_PRE_ALERT);
    } else if (isHistoryAlertCat(category) && isHistoryRecordActive(alertDate, category)) {
        setAlertState(STATE_ALERT);
    }
}

void setAlertState(DeviceState newState) {
    if (newState == STATE_PRE_ALERT && currentState == STATE_ALERT) return;

    if (newState != currentState) {
        currentState = newState;
        stateStartTime = millis();
    } else if (newState == STATE_PRE_ALERT || newState == STATE_ALERT) {
        stateStartTime = millis();
    }

    if (newState == STATE_PRE_ALERT || newState == STATE_ALERT) {
        lastActiveTime = millis();
    }
}

void checkStateExpiration() {
    if (currentState != STATE_PRE_ALERT && currentState != STATE_ALERT) return;

    unsigned long elapsed = millis() - stateStartTime;
    unsigned long limit = (currentState == STATE_PRE_ALERT)
                          ? PRE_ALERT_EXPIRE_MS
                          : ALERT_EXPIRE_MS;

    if (elapsed >= limit) {
        Serial.printf("[alert] %s expired after %lu s\n",
                      currentState == STATE_PRE_ALERT ? "Pre-alert" : "Alert",
                      elapsed / 1000);
        currentState = STATE_OK;
    }
}

unsigned long currentPollInterval() {
    if (currentState == STATE_PRE_ALERT || currentState == STATE_ALERT) return POLL_ACTIVE_MS;
    if (millis() - lastActiveTime < IDLE_THRESHOLD_MS) return POLL_ACTIVE_MS;
    return POLL_IDLE_MS;
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
String fetchOrefPayload(const char* url) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    if (!http.begin(client, url)) return "";

    http.addHeader("Referer", ALERT_REFERER);
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);

    int httpCode = http.GET();
    String payload;

    if (httpCode == HTTP_CODE_OK) {
        payload = http.getString();
        payload.replace("\x00", "");
        payload.trim();

        if (payload.length() >= 3 &&
            (uint8_t)payload[0] == 0xEF &&
            (uint8_t)payload[1] == 0xBB &&
            (uint8_t)payload[2] == 0xBF) {
            payload = payload.substring(3);
        }
    } else {
        Serial.printf("[fetch] HTTP %d for %s\n", httpCode, url);
    }

    http.end();
    return payload;
}

void pollAlertApi() {
    if (WiFi.status() != WL_CONNECTED) return;

    unsigned long now = millis();
    if (now - lastPollTime < currentPollInterval()) return;
    lastPollTime = now;

    String payload = fetchOrefPayload(ALERT_URL);
    if (payload.length() <= 5) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err || !doc["data"].is<JsonArray>()) return;

    int cat = 0;
    if (doc["cat"].is<int>()) {
        cat = doc["cat"].as<int>();
    } else if (doc["cat"].is<const char*>()) {
        cat = atoi(doc["cat"].as<const char*>());
    }

    JsonArray data = doc["data"].as<JsonArray>();
    bool zoneFound = false;
    for (JsonVariant area : data) {
        String areaStr = fixAreaSpelling(area.as<String>());
        if (areaStr == zoneName) {
            zoneFound = true;
            break;
        }
    }

    if (!zoneFound) return;

    int historyCat = realtimeToHistoryCat(cat);
    if (historyCat >= 0 && isHistoryAlertCat(historyCat)) {
        setAlertState(STATE_ALERT);
    }
}

static const size_t HISTORY_SCAN_KEEP = 512;
static const size_t HISTORY_SCAN_CHUNK = 4096;

void scanHistoryBuffer(String& buffer, const String& zonePattern, bool& stateApplied) {
    int searchFrom = 0;

    while (historyCount < HISTORY_MAX) {
        int idx = buffer.indexOf(zonePattern, searchFrom);
        if (idx < 0) break;

        lastHistoryPatternFound = true;

        int objStart = buffer.lastIndexOf('{', idx);
        int objEnd = buffer.indexOf('}', idx);
        if (objStart < 0 || objEnd < 0 || objEnd <= objStart) {
            searchFrom = idx + 1;
            continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, buffer.substring(objStart, objEnd + 1)) == DeserializationError::Ok) {
            String area = fixAreaSpelling(doc["data"].as<String>());
            area.trim();
            if (area == zoneName) {
                applyHistoryRecord(
                    doc["alertDate"].as<String>(),
                    doc["title"].as<String>(),
                    parseJsonCategory(doc["category"]),
                    stateApplied);
            }
        }

        searchFrom = objEnd + 1;
    }
}

void trimHistoryBuffer(String& buffer, const String& zonePattern) {
    size_t keep = zonePattern.length() + HISTORY_SCAN_KEEP;
    if (buffer.length() > keep) {
        buffer = buffer.substring(buffer.length() - keep);
    }
}

void beginHistoryPoll() {
    historyCount = 0;
    lastHistoryBytesRead = 0;
    lastHistoryPatternFound = false;
    histStateApplied = false;
    histBomRemaining = 3;
    histScanBuffer = "";
    histZonePattern = "\"data\":\"" + zoneName + "\"";
    lastHistoryError[0] = '\0';
    lastHistoryHttpCode = 0;
    histStateTime = millis();
    histState = HIST_CONNECTING;
}

void finishHistoryPoll() {
    histClient.stop();
    histState = HIST_IDLE;
    historyPollEverRan = true;
    Serial.printf("[history] zone='%s' http=%d bytes=%u matches=%d pattern=%d err='%s'\n",
                  zoneName.c_str(), lastHistoryHttpCode, lastHistoryBytesRead,
                  historyCount, lastHistoryPatternFound, lastHistoryError);
}

void updateHistoryPoll() {
    switch (histState) {
        case HIST_IDLE:
            return;

        case HIST_CONNECTING:
            histClient.setInsecure();
            histClient.setTimeout(100);
            if (!histClient.connect("www.oref.org.il", 443, 15000)) {
                strncpy(lastHistoryError, "ssl connect failed", sizeof(lastHistoryError));
                finishHistoryPoll();
                return;
            }
            histState = HIST_SEND_REQUEST;
            break;

        case HIST_SEND_REQUEST:
            histClient.print(
                "GET /warningMessages/alert/History/AlertsHistory.json HTTP/1.1\r\n"
                "Host: www.oref.org.il\r\n"
                "Referer: https://www.oref.org.il/\r\n"
                "X-Requested-With: XMLHttpRequest\r\n"
                "Accept: application/json\r\n"
                "Connection: close\r\n\r\n");
            histState = HIST_READ_STATUS;
            histStateTime = millis();
            return;

        case HIST_READ_STATUS:
            if (!histClient.available()) {
                if (millis() - histStateTime > 20000) {
                    strncpy(lastHistoryError, "status timeout", sizeof(lastHistoryError));
                    finishHistoryPoll();
                }
                return;
            }
            {
                String line = histClient.readStringUntil('\n');
                int sp1 = line.indexOf(' ');
                int sp2 = line.indexOf(' ', sp1 + 1);
                if (sp1 >= 0 && sp2 > sp1) {
                    lastHistoryHttpCode = line.substring(sp1 + 1, sp2).toInt();
                }
                if (lastHistoryHttpCode != 200) {
                    snprintf(lastHistoryError, sizeof(lastHistoryError), "HTTP %d", lastHistoryHttpCode);
                    finishHistoryPoll();
                    return;
                }
                histState = HIST_READ_HEADERS;
                histStateTime = millis();
            }
            return;

        case HIST_READ_HEADERS:
            if (histClient.available()) {
                String line = histClient.readStringUntil('\n');
                line.trim();
                if (line.length() == 0) {
                    histState = HIST_READ_BODY;
                    histStateTime = millis();
                }
            } else if (millis() - histStateTime > 20000) {
                strncpy(lastHistoryError, "headers timeout", sizeof(lastHistoryError));
                finishHistoryPoll();
            }
            return;

        case HIST_READ_BODY: {
            int chunk = 0;
            while (histClient.available() && chunk < 1024) {
                int c = histClient.read();
                if (c < 0) continue;
                if (c == 0) continue;

                if (histBomRemaining > 0) {
                    if (histBomRemaining == 3 && c == 0xEF) { histBomRemaining = 2; continue; }
                    if (histBomRemaining == 2 && c == 0xBB) { histBomRemaining = 1; continue; }
                    if (histBomRemaining == 1 && c == 0xBF) { histBomRemaining = 0; continue; }
                    histBomRemaining = 0;
                }

                histScanBuffer += (char)c;
                lastHistoryBytesRead++;
                chunk++;

                if (histScanBuffer.length() >= HISTORY_SCAN_CHUNK) {
                    scanHistoryBuffer(histScanBuffer, histZonePattern, histStateApplied);
                    trimHistoryBuffer(histScanBuffer, histZonePattern);
                }

                if (historyCount >= HISTORY_MAX) {
                    finishHistoryPoll();
                    return;
                }
            }

            if (!histClient.connected() && !histClient.available()) {
                if (histScanBuffer.length() > 0) {
                    scanHistoryBuffer(histScanBuffer, histZonePattern, histStateApplied);
                }
                finishHistoryPoll();
                return;
            }

            if (millis() - histStateTime > 60000) {
                strncpy(lastHistoryError, "body timeout", sizeof(lastHistoryError));
                finishHistoryPoll();
            }
            return;
        }
    }
}

void pollHistoryApi() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (histState != HIST_IDLE) return;

    unsigned long now = millis();
    if (now - lastHistoryPollTime < POLL_IDLE_MS) return;
    lastHistoryPollTime = now;

    if (zoneName.length() == 0) return;
    beginHistoryPoll();
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
        zoneName.trim();
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

void handleGetHistory() {
    JsonDocument doc;
    JsonArray entries = doc["entries"].to<JsonArray>();

    for (int i = 0; i < historyCount; i++) {
        JsonObject item = entries.add<JsonObject>();
        item["date"] = historyEntries[i].date;
        item["title"] = historyEntries[i].title;
        item["category"] = historyEntries[i].category;
        item["type"] = historyRecordType(historyEntries[i].category);
    }

    JsonObject debug = doc["debug"].to<JsonObject>();
    debug["zone"] = zoneName;
    debug["http_code"] = lastHistoryHttpCode;
    debug["bytes_read"] = lastHistoryBytesRead;
    debug["matches"] = historyCount;
    debug["pattern_found"] = lastHistoryPatternFound;
    debug["polled"] = historyPollEverRan;
    if (lastHistoryError[0] != '\0') {
        debug["error"] = lastHistoryError;
    }

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
    server.on("/api/history", HTTP_GET, handleGetHistory);
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

    syncTime();
    wifiSetupDone = true;
    lastPollTime = 0;
    lastHistoryPollTime = millis() - POLL_IDLE_MS;
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

        checkStateExpiration();
        pollAlertApi();
        pollHistoryApi();
        updateHistoryPoll();
    }

    updateLedForState();
    delay(10);
}
