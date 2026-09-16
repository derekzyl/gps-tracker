/**
 * ============================================================
 *  GPS Speed Limit Alert & Monitoring System
 *  ESP32-WROOM Firmware  —  v1.2  (onboarding UI)
 * ============================================================
 *
 *  v1.2 — Professional Wi-Fi onboarding + LCD status carousel
 *  ---------------------------------------------------------
 *  • SoftAP provisioning with captive portal (SSID/password)
 *  • LCD shows hotspot name, password, and setup IP
 *  • Internet connectivity check after STA join
 *  • Auto-rotating LCD status (speed / WiFi / GPS / stats)
 *  • Buttons: MENU next screen, SCROLL prev; long holds for
 *    Wi-Fi re-setup and GSM re-init
 *
 *  PIN MAP
 *  -------
 *  GPIO 27  →  Red  LED + Active Buzzer (alert)
 *  GPIO 26  →  Yellow LED               (moderate warning)
 *  GPIO 25  →  Green LED                (speed compliant)
 *  GPIO 21  →  LCD I2C SDA
 *  GPIO 22  →  LCD I2C SCL
 *  GPIO 16  →  GPS  UART2 RX  (NEO-6M TX)
 *  GPIO 17  →  GPS  UART2 TX  (NEO-6M RX)
 *  GPIO 13  →  GSM  UART1 RX  (SIM800L TX)
 *  GPIO 14  →  GSM  UART1 TX  (SIM800L RX)
 *  GPIO 18  →  Button 1  (MENU: next screen / long = WiFi setup)
 *  GPIO 19  →  Button 2  (SCROLL: prev screen / long = GSM re-init)
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#include <ArduinoJson.h>

// ═══════════════════════════════════════════════════════════
//  PIN DEFINITIONS
// ═══════════════════════════════════════════════════════════
#define PIN_BUZZER       27
#define PIN_LED_RED      27
#define PIN_LED_YELLOW   26
#define PIN_LED_GREEN    25

#define GPS_RX_PIN       16
#define GPS_TX_PIN       17
#define GSM_RX_PIN       13
#define GSM_TX_PIN       14

#define BTN_MENU         18
#define BTN_SCROLL       19

#define LCD_SDA          21
#define LCD_SCL          22
#define LCD_ADDR         0x27
#define LCD_COLS         16
#define LCD_ROWS         2

// ═══════════════════════════════════════════════════════════
//  SPEED THRESHOLDS  (km/h over limit)
// ═══════════════════════════════════════════════════════════
#define THRESH_MINOR      1
#define THRESH_MODERATE  10
#define THRESH_SEVERE    20

// ═══════════════════════════════════════════════════════════
//  DEFAULT CONFIGURATION
// ═══════════════════════════════════════════════════════════
#define DEFAULT_SPEED_LIMIT   50
#define MAX_PHONE_NUMBERS     10
#define SMS_COOLDOWN_MS       30000
#define WIFI_TIMEOUT_MS       15000
#define GSM_TIMEOUT_MS        8000
#define SERVER_TIMEOUT_MS     5000
#define GPS_STALE_MS          3000
#define UI_ROTATE_MS          4000
#define UI_PAUSE_MS           12000
#define BTN_DEBOUNCE_MS       280
#define BTN_LONG_MS           3000
#define INTERNET_TEST_MS      4000
#define PROV_LCD_ROTATE_MS    3000

#define AP_SSID               "GPS-SpeedMonitor"
#define AP_PASS               "speed1234"
#define DNS_PORT              53

// ── UI / network modes ───────────────────────────────────────
enum UiScreen : uint8_t {
    UI_SPEED = 0,
    UI_WIFI,
    UI_GPS,
    UI_STATS,
    UI_COUNT
};

enum DeviceMode : uint8_t {
    MODE_PROVISIONING = 0,  // SoftAP — user must enter Wi-Fi
    MODE_CONNECTING,        // Trying STA / testing internet
    MODE_ONLINE,            // STA + internet OK
    MODE_LOCAL              // STA OK but no internet (or AP-only skip)
};

const char* DEFAULT_NUMBERS[] = {
    "+2347059011222",
    "+2348135993811",
    "+2348056322139"
};
// Auto-count: stays correct when you add/remove entries above
constexpr int N_DEFAULT_NUMBERS =
    sizeof(DEFAULT_NUMBERS) / sizeof(DEFAULT_NUMBERS[0]);

// ═══════════════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════════════
HardwareSerial gpsSerial(2);
HardwareSerial gsmSerial(1);

TinyGPSPlus        gps;
LiquidCrystal_I2C  lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
WebServer          webServer(80);
DNSServer          dnsServer;
Preferences        prefs;

// ── Runtime state ────────────────────────────────────────────
struct SystemState {
    float   currentSpeed   = 0.0f;
    float   speedLimit     = DEFAULT_SPEED_LIMIT;
    float   prevLat        = 0.0f;
    float   prevLon        = 0.0f;
    unsigned long prevFixMs = 0;

    bool    gpsValid       = false;
    bool    wifiConnected  = false;  // STA associated
    bool    apActive       = false;
    bool    internetOk     = false;
    bool    gsmReady       = false;
    bool    wifiConfigured = false;  // NVS has user Wi-Fi

    DeviceMode mode        = MODE_PROVISIONING;
    UiScreen   uiScreen    = UI_SPEED;
    bool       uiPaused    = false;
    unsigned long lastUiMs = 0;
    unsigned long uiPauseUntil = 0;
    uint8_t    provPage    = 0;

    int     violationTier  = 0;
    unsigned long lastSmsMs  = 0;
    unsigned long lastPostMs = 0;

    unsigned long totalViolations = 0;
    float   maxSpeedSeen   = 0.0f;
} state;

// ── NVS-backed config ────────────────────────────────────────
struct Config {
    // Empty SSID → first boot enters SoftAP onboarding
    char   wifiSSID[64]       = "";
    char   wifiPass[64]       = "";
    char   serverURL[128]     = "http://visiting-carmella-cybergenii-895c1fde.koyeb.app/api/violation";
    char   deviceID[32]       = "ESP32-SPEED-01";
    int    defaultSpeedLimit  = DEFAULT_SPEED_LIMIT;
    int    threshMinor        = THRESH_MINOR;
    int    threshModerate     = THRESH_MODERATE;
    int    threshSevere       = THRESH_SEVERE;
    int    numPhones          = N_DEFAULT_NUMBERS;
    char   phones[MAX_PHONE_NUMBERS][20];
} cfg;

// ── Violation ring buffer ────────────────────────────────────
struct ViolationRecord {
    float   speed;
    float   limit;
    float   lat;
    float   lon;
    int     tier;
    unsigned long timestamp;
    char    tier_str[10];
};
#define LOG_SIZE 20
ViolationRecord violationLog[LOG_SIZE];
int logHead  = 0;
int logCount = 0;

// ── Alert blink state ────────────────────────────────────────
unsigned long lastBlinkMs = 0;
bool blinkState = false;

// ═══════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ═══════════════════════════════════════════════════════════
void loadConfig();
void saveConfig();
void connectWiFi();
void startProvisioningAP();
bool tryConnectSTA();
bool testInternet();
void applyWiFiCredentials(const char* ssid, const char* pass);
void enterProvisioning(const char* reason);
bool initGSM();
void setupWebServer();
void handleGPS();
float haversineSpeed(float, float, float, float, float);
float getSpeedLimit(float, float);
void processViolation(float, float, float, float);
void triggerAlert(int);
void silenceAlert();
bool sendSMS(const char*);
bool postToServer(float, float, float, float, const char*);
void updateLCD(const char*, const char*);
void logViolation(float, float, float, float, int);
void truncate16(char* dest, const char* src);
void renderUi(bool force = false);
void renderProvisioningLcd();
void renderStatusScreen(UiScreen screen);
void handleButtons();
void sendDashboard();
void sendSettings();
void sendWifiSetup();
void sendStatusJSON();
void sendViolationsJSON();
void gsmSendAT(const char*, unsigned long timeout = 1000);
String gsmReadResponse(unsigned long timeout = 2000);
String gsmWaitFor(const char* token, unsigned long timeout);
IPAddress currentDeviceIP();
const char* modeLabel();

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial.println(F("\n=== GPS Speed Monitor v1.2 — System Starting ==="));

    pinMode(PIN_LED_RED,    OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_GREEN,  OUTPUT);
    pinMode(BTN_MENU,       INPUT_PULLUP);
    pinMode(BTN_SCROLL,     INPUT_PULLUP);
    digitalWrite(PIN_LED_RED,    LOW);
    digitalWrite(PIN_LED_YELLOW, LOW);
    digitalWrite(PIN_LED_GREEN,  LOW);

    Wire.begin(LCD_SDA, LCD_SCL);
    lcd.init();
    lcd.backlight();
    updateLCD("GPS Speed v1.2", "Starting...");

    loadConfig();

    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    gsmSerial.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    Serial.println(F("[GPS]  UART2 started on RX=16 TX=17"));
    Serial.println(F("[GSM]  UART1 started on RX=13 TX=14"));

    updateLCD("Network Setup", "Please wait...");
    connectWiFi();

    updateLCD("Init GSM...", "SIM800L");
    state.gsmReady = initGSM();
    Serial.printf("[GSM]  Ready: %s\n", state.gsmReady ? "YES" : "NO");

    setupWebServer();

    state.lastUiMs = millis();
    renderUi(true);
    digitalWrite(PIN_LED_GREEN, HIGH);

    Serial.printf("[MODE] %s\n", modeLabel());
    if (state.apActive)
        Serial.printf("[Web]  Setup: http://%s/wifi\n", WiFi.softAPIP().toString().c_str());
    else if (state.wifiConnected)
        Serial.printf("[Web]  http://%s/\n", WiFi.localIP().toString().c_str());

    Serial.println(F("[OK]   Setup complete — system running\n"));
}

// ═══════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════
void loop() {
    while (gpsSerial.available() > 0)
        gps.encode(gpsSerial.read());

    if (state.apActive)
        dnsServer.processNextRequest();

    webServer.handleClient();
    handleButtons();
    renderUi(false);

    if (gps.location.isUpdated() && gps.location.isValid())
        handleGPS();

    static unsigned long lastBlink = 0;
    if (!state.gpsValid && millis() - lastBlink > 1000) {
        lastBlink = millis();
        digitalWrite(PIN_LED_GREEN, !digitalRead(PIN_LED_GREEN));
    }

    static unsigned long lastHeap = 0;
    if (millis() - lastHeap > 5000) {
        lastHeap = millis();
        Serial.printf("[MEM]  Free heap: %u bytes\n", ESP.getFreeHeap());
    }

    delay(10);
}

// ═══════════════════════════════════════════════════════════
//  GPS PROCESSING
// ═══════════════════════════════════════════════════════════
void handleGPS() {
    if (gps.location.age() > GPS_STALE_MS) return;
    if (gps.hdop.isValid() && gps.hdop.value() > 500) return;

    float lat = gps.location.lat();
    float lon = gps.location.lng();
    unsigned long now = millis();

    state.gpsValid = true;
    state.speedLimit = getSpeedLimit(lat, lon);

    float speed = 0.0f;
    if (state.prevFixMs > 0) {
        float dt = (now - state.prevFixMs) / 1000.0f;
        if (dt > 0.1f && dt < 5.0f)
            speed = haversineSpeed(state.prevLat, state.prevLon, lat, lon, dt);
    }

    state.currentSpeed = speed;
    if (speed > state.maxSpeedSeen) state.maxSpeedSeen = speed;

    // LCD is owned by renderUi() — do not overwrite here
    processViolation(speed, state.speedLimit, lat, lon);

    state.prevLat   = lat;
    state.prevLon   = lon;
    state.prevFixMs = now;

    float excess = speed - state.speedLimit;
    Serial.printf("[GPS]  Speed=%.1f  Limit=%.0f  Excess=%.1f  HDOP=%.1f\n",
        speed, state.speedLimit, excess,
        gps.hdop.isValid() ? gps.hdop.hdop() : 0.0f);
}

// ═══════════════════════════════════════════════════════════
//  HAVERSINE SPEED
// ═══════════════════════════════════════════════════════════
float haversineSpeed(float lat1, float lon1, float lat2, float lon2, float dt) {
    const float R = 6371000.0f;
    float phi1 = lat1 * DEG_TO_RAD, phi2 = lat2 * DEG_TO_RAD;
    float dPhi = (lat2 - lat1) * DEG_TO_RAD;
    float dLam = (lon2 - lon1) * DEG_TO_RAD;
    float a = sinf(dPhi/2)*sinf(dPhi/2) + cosf(phi1)*cosf(phi2)*sinf(dLam/2)*sinf(dLam/2);
    float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
    if (dt <= 0.0f) return 0.0f;
    return (R * c / dt) * 3.6f;
}

// ═══════════════════════════════════════════════════════════
//  SPEED ZONE DATABASE
// ═══════════════════════════════════════════════════════════
struct SpeedZone { float latMin, latMax, lonMin, lonMax, limitKph; const char* label; };
const SpeedZone ZONES[] = {
    { 9.0700f, 9.0800f, 7.3900f, 7.4000f,  30.0f, "Residential" },
    { 9.0800f, 9.0950f, 7.3900f, 7.4200f,  50.0f, "Urban"       },
    { 9.0500f, 9.0700f, 7.3800f, 7.4300f,  80.0f, "Express"     },
    { 8.9000f, 9.0500f, 7.3000f, 7.5000f, 100.0f, "Highway"     },
};
const int N_ZONES = sizeof(ZONES) / sizeof(SpeedZone);

float getSpeedLimit(float lat, float lon) {
    for (int i = 0; i < N_ZONES; i++)
        if (lat >= ZONES[i].latMin && lat <= ZONES[i].latMax &&
            lon >= ZONES[i].lonMin && lon <= ZONES[i].lonMax)
            return ZONES[i].limitKph;
    return (float)cfg.defaultSpeedLimit;
}

// ═══════════════════════════════════════════════════════════
//  VIOLATION PROCESSING
// ═══════════════════════════════════════════════════════════
void processViolation(float speed, float limit, float lat, float lon) {
    if (speed <= limit) {
        silenceAlert();
        triggerAlert(0);
        state.violationTier = 0;
        return;
    }

    float excess = speed - limit;
    int tier = (excess >= (float)cfg.threshSevere)   ? 3 :
               (excess >= (float)cfg.threshModerate) ? 2 : 1;

    state.violationTier = tier;
    state.totalViolations++;
    triggerAlert(tier);

    const char* tierStr = (tier == 3) ? "SEVERE" : (tier == 2) ? "MODERATE" : "MINOR";
    Serial.printf("[ALERT] Tier=%d (%s)  Excess=+%.1f km/h\n", tier, tierStr, excess);

    logViolation(speed, limit, lat, lon, tier);

    unsigned long now = millis();

    if (tier == 3) {
        if (now - state.lastSmsMs > SMS_COOLDOWN_MS) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                "SPEED ALERT [%s]\nDevice: %s\nSpeed: %.0f km/h in %.0f km/h zone\n"
                "Excess: +%.0f km/h\nLat: %.6f  Lon: %.6f",
                tierStr, cfg.deviceID, speed, limit, excess, lat, lon);
            if (sendSMS(msg)) state.lastSmsMs = now;
        }
        if (now - state.lastPostMs > 5000)
            if (postToServer(speed, limit, lat, lon, tierStr)) state.lastPostMs = millis();
    } else if (tier == 2) {
        if (now - state.lastPostMs > 5000)
            if (postToServer(speed, limit, lat, lon, tierStr)) state.lastPostMs = millis();
    }
}

// ═══════════════════════════════════════════════════════════
//  ALERT OUTPUTS
// ═══════════════════════════════════════════════════════════
void triggerAlert(int tier) {
    unsigned long now = millis();
    switch (tier) {
        case 0:
            digitalWrite(PIN_LED_GREEN,  HIGH);
            digitalWrite(PIN_LED_YELLOW, LOW);
            digitalWrite(PIN_LED_RED,    LOW);
            break;
        case 1:
            digitalWrite(PIN_LED_GREEN, LOW);
            if (now - lastBlinkMs > 500) { blinkState = !blinkState; lastBlinkMs = now; }
            digitalWrite(PIN_LED_YELLOW, blinkState);
            digitalWrite(PIN_LED_RED,    LOW);
            break;
        case 2:
            digitalWrite(PIN_LED_GREEN,  LOW);
            digitalWrite(PIN_LED_YELLOW, HIGH);
            if (now - lastBlinkMs > 300) { blinkState = !blinkState; lastBlinkMs = now; }
            digitalWrite(PIN_LED_RED, blinkState);
            break;
        case 3:
            digitalWrite(PIN_LED_GREEN,  LOW);
            digitalWrite(PIN_LED_YELLOW, LOW);
            if (now - lastBlinkMs > 200) { blinkState = !blinkState; lastBlinkMs = now; }
            digitalWrite(PIN_LED_RED, blinkState);   // buzzer shares pin 27
            break;
    }
}

void silenceAlert() {
    digitalWrite(PIN_LED_RED,    LOW);
    digitalWrite(PIN_LED_YELLOW, LOW);
}

// ═══════════════════════════════════════════════════════════
//  GSM / AT HELPERS
// ═══════════════════════════════════════════════════════════
void gsmSendAT(const char* cmd, unsigned long timeout) {
    gsmSerial.println(cmd);
    unsigned long t = millis();
    while (millis() - t < timeout) { yield(); }
}

String gsmReadResponse(unsigned long timeout) {
    String resp;
    resp.reserve(128);
    unsigned long start = millis();
    while (millis() - start < timeout) {
        while (gsmSerial.available())
            resp += (char)gsmSerial.read();
        yield();
    }
    return resp;
}

// FIX 4: Wait specifically for a token (e.g. "+CMGS:" or ">")
String gsmWaitFor(const char* token, unsigned long timeout) {
    String resp;
    resp.reserve(256);
    unsigned long start = millis();
    while (millis() - start < timeout) {
        while (gsmSerial.available())
            resp += (char)gsmSerial.read();
        if (resp.indexOf(token) != -1) break;
        yield();
        yield();   // keep WDT happy during long waits
    }
    return resp;
}

bool initGSM() {
    Serial.println(F("[GSM]  Initialising..."));
    gsmSendAT("AT", 1000);
    String r = gsmReadResponse(1000);
    if (r.indexOf("OK") == -1) {
        gsmSendAT("AT", 1500);
        r = gsmReadResponse(1000);
    }
    if (r.indexOf("OK") == -1) {
        Serial.println(F("[GSM]  No response — check wiring"));
        return false;
    }
    gsmSendAT("ATE0",            500);
    gsmSendAT("AT+CMGF=1",       500);
    gsmSendAT("AT+CNMI=1,2,0,0,0", 500);
    Serial.println(F("[GSM]  Initialised OK"));
    return true;
}

// ═══════════════════════════════════════════════════════════
//  SMS — FIX 4: proper "+CMGS:" response check
//               FIX 3: WDT reset during blocking waits
// ═══════════════════════════════════════════════════════════
bool sendSMS(const char* message) {
    if (!state.gsmReady) {
        Serial.println(F("[SMS]  GSM not ready — skipping"));
        return false;
    }

    bool anySuccess = false;

    for (int i = 0; i < cfg.numPhones; i++) {
        if (strlen(cfg.phones[i]) < 7) continue;
        Serial.printf("[SMS]  Sending to %s ...\n", cfg.phones[i]);

        // Ensure text mode
        gsmSendAT("AT+CMGF=1", 300);
        gsmReadResponse(300);

        // Send recipient command
        String cmd = "AT+CMGS=\"";
        cmd += cfg.phones[i];
        cmd += "\"";
        gsmSerial.println(cmd);

        // Wait for ">" prompt — FIX 4
        String prompt = gsmWaitFor(">", 5000);
        if (prompt.indexOf(">") == -1) {
            Serial.printf("[SMS]  No prompt for %s — skipping\n", cfg.phones[i]);
            gsmSerial.write(27); // ESC to abort
            delay(500);
            continue;
        }

        // Send body + Ctrl-Z
        gsmSerial.print(message);
        gsmSerial.write(26);

        // Wait for "+CMGS:" — FIX 4 (not just "OK")
        String resp = gsmWaitFor("+CMGS:", 10000);

        if (resp.indexOf("+CMGS:") != -1) {
            Serial.printf("[SMS]  Sent OK to %s\n", cfg.phones[i]);
            anySuccess = true;
        } else {
            Serial.printf("[SMS]  FAILED to %s  resp: %s\n", cfg.phones[i], resp.c_str());
        }

        // Inter-SMS gap — safe yield loop
        unsigned long gap = millis();
        while (millis() - gap < 2000) { yield(); yield(); }
    }
    return anySuccess;
}

// ═══════════════════════════════════════════════════════════
//  HTTP POST — FIX 1: plain HTTP, no TLS heap cost
// ═══════════════════════════════════════════════════════════
bool postToServer(float speed, float limit, float lat, float lon, const char* tier) {
    if (!state.wifiConnected || strlen(cfg.serverURL) < 10) return false;

    HTTPClient http;
    http.begin(cfg.serverURL);          // HTTP only — no WiFiClientSecure needed
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(SERVER_TIMEOUT_MS);

    StaticJsonDocument<256> doc;
    doc["device"]    = cfg.deviceID;
    doc["speed"]     = speed;
    doc["limit"]     = limit;
    doc["excess"]    = speed - limit;
    doc["tier"]      = tier;
    doc["lat"]       = lat;
    doc["lon"]       = lon;
    doc["timestamp"] = millis();

    String payload;
    payload.reserve(200);
    serializeJson(doc, payload);

    int code = http.POST(payload);
    http.end();

    if (code == 200 || code == 201) {
        Serial.printf("[POST] Server accepted (HTTP %d)\n", code);
        return true;
    }
    Serial.printf("[POST] Server returned HTTP %d\n", code);
    return false;
}

// ═══════════════════════════════════════════════════════════
//  VIOLATION LOG
// ═══════════════════════════════════════════════════════════
void logViolation(float speed, float limit, float lat, float lon, int tier) {
    const char* ts = (tier == 3) ? "SEVERE" : (tier == 2) ? "MODERATE" : "MINOR";
    ViolationRecord& rec = violationLog[logHead];
    rec.speed = speed;  rec.limit = limit;
    rec.lat   = lat;    rec.lon   = lon;
    rec.tier  = tier;   rec.timestamp = millis();
    strncpy(rec.tier_str, ts, 9);
    rec.tier_str[9] = '\0';
    logHead = (logHead + 1) % LOG_SIZE;
    if (logCount < LOG_SIZE) logCount++;
}

// ═══════════════════════════════════════════════════════════
//  LCD + UI RENDER
// ═══════════════════════════════════════════════════════════
void updateLCD(const char* line1, const char* line2) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(line1);
    lcd.setCursor(0, 1); lcd.print(line2);
}

void truncate16(char* dest, const char* src) {
    strncpy(dest, src ? src : "", 16);
    dest[16] = '\0';
}

IPAddress currentDeviceIP() {
    if (state.wifiConnected) return WiFi.localIP();
    if (state.apActive)      return WiFi.softAPIP();
    return IPAddress(0, 0, 0, 0);
}

const char* modeLabel() {
    switch (state.mode) {
        case MODE_PROVISIONING: return "PROVISIONING";
        case MODE_CONNECTING:   return "CONNECTING";
        case MODE_ONLINE:       return "ONLINE";
        case MODE_LOCAL:        return "LOCAL";
        default:                return "?";
    }
}

void renderStatusScreen(UiScreen screen) {
    char l1[17], l2[17];

    switch (screen) {
        case UI_SPEED: {
            snprintf(l1, sizeof(l1), "Spd:%3.0f Lim:%3.0f",
                     state.currentSpeed, state.speedLimit);
            float excess = state.currentSpeed - state.speedLimit;
            if (!state.gpsValid)
                snprintf(l2, sizeof(l2), "Acquiring GPS..");
            else if (excess > 0)
                snprintf(l2, sizeof(l2), "OVER +%.0fkm/h!", excess);
            else
                snprintf(l2, sizeof(l2), "SAFE            ");
            break;
        }
        case UI_WIFI: {
            if (state.wifiConnected) {
                char ssid[17];
                truncate16(ssid, cfg.wifiSSID);
                snprintf(l1, sizeof(l1), "WiFi connected");
                if (state.internetOk)
                    snprintf(l2, sizeof(l2), "to:%.13s", ssid);
                else
                    snprintf(l2, sizeof(l2), "No internet");
            } else if (state.apActive) {
                snprintf(l1, sizeof(l1), "AP Setup Mode");
                snprintf(l2, sizeof(l2), "%s",
                         WiFi.softAPIP().toString().c_str());
            } else {
                snprintf(l1, sizeof(l1), "WiFi: Offline");
                snprintf(l2, sizeof(l2), "Hold MENU setup");
            }
            break;
        }
        case UI_GPS: {
            snprintf(l1, sizeof(l1), "Sats:%d HDOP:%.1f",
                gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f);
            snprintf(l2, sizeof(l2), "Fix:%s",
                     state.gpsValid ? "ACTIVE" : "SEARCHING");
            break;
        }
        case UI_STATS:
        default: {
            if (state.wifiConnected)
                snprintf(l1, sizeof(l1), "%s", WiFi.localIP().toString().c_str());
            else if (state.apActive)
                snprintf(l1, sizeof(l1), "%s", WiFi.softAPIP().toString().c_str());
            else
                snprintf(l1, sizeof(l1), "Viols:%lu", state.totalViolations);
            snprintf(l2, sizeof(l2), "Max:%.0f GSM:%s",
                     state.maxSpeedSeen, state.gsmReady ? "OK" : "--");
            break;
        }
    }
    updateLCD(l1, l2);
}

void renderProvisioningLcd() {
    char l1[17], l2[17];
    switch (state.provPage % 3) {
        case 0:
            snprintf(l1, sizeof(l1), "WiFi Setup Mode");
            snprintf(l2, sizeof(l2), "Join hotspot...");
            break;
        case 1:
            snprintf(l1, sizeof(l1), "Hotspot name:");
            snprintf(l2, sizeof(l2), "%.16s", AP_SSID);
            break;
        default:
            snprintf(l1, sizeof(l1), "Pass:%s", AP_PASS);
            snprintf(l2, sizeof(l2), "%s", WiFi.softAPIP().toString().c_str());
            break;
    }
    updateLCD(l1, l2);
}

void renderUi(bool force) {
    unsigned long now = millis();

    if (state.mode == MODE_PROVISIONING || state.mode == MODE_CONNECTING) {
        if (force || now - state.lastUiMs >= PROV_LCD_ROTATE_MS) {
            if (!force && state.mode == MODE_PROVISIONING)
                state.provPage++;
            state.lastUiMs = now;
            if (state.mode == MODE_CONNECTING)
                updateLCD("Connecting...",
                          strlen(cfg.wifiSSID) ? cfg.wifiSSID : "WiFi");
            else
                renderProvisioningLcd();
        }
        return;
    }

    if (state.uiPaused && now >= state.uiPauseUntil)
        state.uiPaused = false;

    if (!state.uiPaused && (force || now - state.lastUiMs >= UI_ROTATE_MS)) {
        if (!force)
            state.uiScreen = (UiScreen)((state.uiScreen + 1) % UI_COUNT);
        state.lastUiMs = now;
        renderStatusScreen(state.uiScreen);
    } else if (force) {
        state.lastUiMs = now;
        renderStatusScreen(state.uiScreen);
    }
}

// ═══════════════════════════════════════════════════════════
//  BUTTONS
//  MENU short  → next status screen (pauses auto-rotate)
//  MENU long   → enter Wi-Fi provisioning
//  SCROLL short→ previous status screen
//  SCROLL long → re-init GSM
// ═══════════════════════════════════════════════════════════
void handleButtons() {
    static unsigned long lastMenuEdge = 0, lastScrollEdge = 0;
    static bool menuWasDown = false, scrollWasDown = false;
    static unsigned long menuDownAt = 0, scrollDownAt = 0;
    static bool menuLongDone = false, scrollLongDone = false;

    bool menuDown   = digitalRead(BTN_MENU) == LOW;
    bool scrollDown = digitalRead(BTN_SCROLL) == LOW;
    unsigned long now = millis();

    // ── MENU ─────────────────────────────────────────────────
    if (menuDown && !menuWasDown && now - lastMenuEdge > BTN_DEBOUNCE_MS) {
        menuWasDown = true;
        menuDownAt = now;
        menuLongDone = false;
        lastMenuEdge = now;
    }
    if (menuDown && menuWasDown && !menuLongDone && now - menuDownAt >= BTN_LONG_MS) {
        menuLongDone = true;
        enterProvisioning("MENU hold");
        updateLCD("WiFi Setup...", "Starting AP");
        startProvisioningAP();
        state.provPage = 0;
        state.lastUiMs = 0;
        renderUi(true);
    }
    if (!menuDown && menuWasDown) {
        menuWasDown = false;
        if (!menuLongDone && now - menuDownAt < BTN_LONG_MS) {
            if (state.mode == MODE_PROVISIONING) {
                state.provPage++;
                state.lastUiMs = 0;
                renderUi(true);
            } else {
                state.uiScreen = (UiScreen)((state.uiScreen + 1) % UI_COUNT);
                state.uiPaused = true;
                state.uiPauseUntil = now + UI_PAUSE_MS;
                state.lastUiMs = now;
                renderStatusScreen(state.uiScreen);
            }
        }
    }

    // ── SCROLL ───────────────────────────────────────────────
    if (scrollDown && !scrollWasDown && now - lastScrollEdge > BTN_DEBOUNCE_MS) {
        scrollWasDown = true;
        scrollDownAt = now;
        scrollLongDone = false;
        lastScrollEdge = now;
    }
    if (scrollDown && scrollWasDown && !scrollLongDone && now - scrollDownAt >= BTN_LONG_MS) {
        scrollLongDone = true;
        updateLCD("Re-init GSM...", "");
        state.gsmReady = initGSM();
        updateLCD("GSM:", state.gsmReady ? "OK" : "FAILED");
        state.uiPaused = true;
        state.uiPauseUntil = now + UI_PAUSE_MS;
        state.lastUiMs = now;
    }
    if (!scrollDown && scrollWasDown) {
        scrollWasDown = false;
        if (!scrollLongDone && now - scrollDownAt < BTN_LONG_MS) {
            if (state.mode == MODE_PROVISIONING) {
                // Show password / IP page immediately
                state.provPage = 2;
                state.lastUiMs = 0;
                renderUi(true);
            } else {
                state.uiScreen = (UiScreen)((state.uiScreen + UI_COUNT - 1) % UI_COUNT);
                state.uiPaused = true;
                state.uiPauseUntil = now + UI_PAUSE_MS;
                state.lastUiMs = now;
                renderStatusScreen(state.uiScreen);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  WI-FI / ONBOARDING
// ═══════════════════════════════════════════════════════════
void enterProvisioning(const char* reason) {
    Serial.printf("[WiFi] Enter provisioning (%s)\n", reason ? reason : "");
    state.mode = MODE_PROVISIONING;
    state.wifiConnected = false;
    state.internetOk = false;
    state.provPage = 0;
    state.lastUiMs = 0;
}

void startProvisioningAP() {
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(AP_SSID, AP_PASS);
    state.apActive = ok;
    state.wifiConnected = false;
    state.internetOk = false;
    state.mode = MODE_PROVISIONING;

    IPAddress apIP = WiFi.softAPIP();
    dnsServer.stop();
    dnsServer.start(DNS_PORT, "*", apIP);

    Serial.printf("[WiFi] SoftAP '%s' / '%s' — %s (%s)\n",
                  AP_SSID, AP_PASS, apIP.toString().c_str(),
                  ok ? "OK" : "FAIL");
}

bool tryConnectSTA() {
    if (strlen(cfg.wifiSSID) == 0) return false;

    state.mode = MODE_CONNECTING;
    updateLCD("Connecting...", cfg.wifiSSID);

    WiFi.mode(WIFI_STA);
    state.apActive = false;
    dnsServer.stop();
    WiFi.begin(cfg.wifiSSID, cfg.wifiPass);

    unsigned long start = millis();
    Serial.printf("[WiFi] Connecting to %s", cfg.wifiSSID);
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
        delay(250);
        Serial.print(".");
        yield();
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        state.wifiConnected = false;
        return false;
    }

    state.wifiConnected = true;
    Serial.printf("[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

bool testInternet() {
    if (!state.wifiConnected) {
        state.internetOk = false;
        return false;
    }

    updateLCD("Testing net...", "Please wait");
    HTTPClient http;
    http.setTimeout(INTERNET_TEST_MS);
    // Lightweight reachability check (no TLS)
    bool ok = false;
    if (http.begin("http://clients3.google.com/generate_204")) {
        int code = http.GET();
        ok = (code == 204 || code == 200 || code == 301 || code == 302);
        Serial.printf("[Net]  Probe HTTP %d → %s\n", code, ok ? "OK" : "FAIL");
        http.end();
    }
    if (!ok && strlen(cfg.serverURL) > 0) {
        if (http.begin(cfg.serverURL)) {
            int code = http.sendRequest("HEAD");
            // Any response (even 404/405) means routing works
            ok = (code > 0);
            Serial.printf("[Net]  Server HEAD %d → %s\n", code, ok ? "OK" : "FAIL");
            http.end();
        }
    }
    state.internetOk = ok;
    return ok;
}

void connectWiFi() {
    if (!state.wifiConfigured || strlen(cfg.wifiSSID) == 0) {
        enterProvisioning("no credentials");
        startProvisioningAP();
        updateLCD("WiFi Setup Mode", WiFi.softAPIP().toString().c_str());
        return;
    }

    if (tryConnectSTA()) {
        if (testInternet()) {
            state.mode = MODE_ONLINE;
            updateLCD("WiFi connected", cfg.wifiSSID);
        } else {
            state.mode = MODE_LOCAL;
            updateLCD("WiFi: no net", cfg.wifiSSID);
        }
        delay(800);
        return;
    }

    Serial.println(F("[WiFi] STA failed — SoftAP onboarding"));
    enterProvisioning("STA failed");
    startProvisioningAP();
    updateLCD("WiFi Setup Mode", WiFi.softAPIP().toString().c_str());
}

void applyWiFiCredentials(const char* ssid, const char* pass) {
    strncpy(cfg.wifiSSID, ssid ? ssid : "", 63);
    cfg.wifiSSID[63] = '\0';
    strncpy(cfg.wifiPass, pass ? pass : "", 63);
    cfg.wifiPass[63] = '\0';
    state.wifiConfigured = strlen(cfg.wifiSSID) > 0;
    saveConfig();

    if (!state.wifiConfigured) {
        enterProvisioning("cleared");
        startProvisioningAP();
        return;
    }

    updateLCD("Saving WiFi...", cfg.wifiSSID);
    if (tryConnectSTA()) {
        if (testInternet())
            state.mode = MODE_ONLINE;
        else
            state.mode = MODE_LOCAL;
        state.uiScreen = UI_WIFI;
        state.uiPaused = true;
        state.uiPauseUntil = millis() + UI_PAUSE_MS;
        renderUi(true);
    } else {
        enterProvisioning("join failed");
        startProvisioningAP();
        updateLCD("Join failed", "Retry setup");
        delay(1200);
        renderUi(true);
    }
}

// ═══════════════════════════════════════════════════════════
//  NVS CONFIG
// ═══════════════════════════════════════════════════════════
void loadConfig() {
    prefs.begin("gps-monitor", true);

    if (prefs.isKey("wifiSSID")) {
        strncpy(cfg.wifiSSID, prefs.getString("wifiSSID").c_str(), 63);
        cfg.wifiSSID[63] = '\0';
    }
    if (prefs.isKey("wifiPass")) {
        strncpy(cfg.wifiPass, prefs.getString("wifiPass").c_str(), 63);
        cfg.wifiPass[63] = '\0';
    }
    state.wifiConfigured = prefs.isKey("wifiCfg")
        ? prefs.getBool("wifiCfg")
        : (strlen(cfg.wifiSSID) > 0);

    if (prefs.isKey("serverURL"))
        strncpy(cfg.serverURL, prefs.getString("serverURL").c_str(), 127);
    if (prefs.isKey("deviceID"))
        strncpy(cfg.deviceID,  prefs.getString("deviceID").c_str(),  31);

    if (prefs.isKey("defLimit"))    cfg.defaultSpeedLimit = prefs.getInt("defLimit");
    if (prefs.isKey("thrMinor"))    cfg.threshMinor       = prefs.getInt("thrMinor");
    if (prefs.isKey("thrModerate")) cfg.threshModerate    = prefs.getInt("thrModerate");
    if (prefs.isKey("thrSevere"))   cfg.threshSevere      = prefs.getInt("thrSevere");
    if (prefs.isKey("numPhones"))   cfg.numPhones         = prefs.getInt("numPhones");

    if (cfg.numPhones <= 0 || cfg.numPhones > MAX_PHONE_NUMBERS)
        cfg.numPhones = N_DEFAULT_NUMBERS;

    if (prefs.isKey("phonesStored") && prefs.getBool("phonesStored")) {
        for (int i = 0; i < cfg.numPhones; i++) {
            String key = "phone" + String(i);
            if (prefs.isKey(key.c_str()))
                strncpy(cfg.phones[i], prefs.getString(key.c_str()).c_str(), 19);
        }
    } else {
        for (int i = 0; i < N_DEFAULT_NUMBERS; i++)
            strncpy(cfg.phones[i], DEFAULT_NUMBERS[i], 19);
        cfg.numPhones = N_DEFAULT_NUMBERS;
    }

    prefs.end();
    Serial.printf("[NVS]  Config loaded — wifiCfg=%d ssid='%s' phones=%d\n",
                  state.wifiConfigured, cfg.wifiSSID, cfg.numPhones);
}

void saveConfig() {
    prefs.begin("gps-monitor", false);
    prefs.putString("wifiSSID",  cfg.wifiSSID);
    prefs.putString("wifiPass",  cfg.wifiPass);
    prefs.putBool("wifiCfg",     state.wifiConfigured);
    prefs.putString("serverURL", cfg.serverURL);
    prefs.putString("deviceID",  cfg.deviceID);
    prefs.putInt("defLimit",     cfg.defaultSpeedLimit);
    prefs.putInt("thrMinor",     cfg.threshMinor);
    prefs.putInt("thrModerate",  cfg.threshModerate);
    prefs.putInt("thrSevere",    cfg.threshSevere);
    prefs.putInt("numPhones",    cfg.numPhones);
    prefs.putBool("phonesStored", true);
    for (int i = 0; i < cfg.numPhones; i++) {
        String key = "phone" + String(i);
        prefs.putString(key.c_str(), cfg.phones[i]);
    }
    prefs.end();
    Serial.println(F("[NVS]  Config saved"));
}

// ═══════════════════════════════════════════════════════════
//  WEB SERVER
//  FIX 2: All pages sent as chunked streams using F() literals
//          — zero large String heap allocations
// ═══════════════════════════════════════════════════════════

// ── Shared CSS (sent once as a chunk) ────────────────────────
static const char PAGE_CSS[] PROGMEM = R"CSS(
<style>
:root{--ink:#0b1f33;--soft:#3d5166;--mut:#6b7c8f;--line:#d5dee8;--bg:#eef3f7;
--card:#fff;--teal:#0f9d8a;--td:#0a7a6b;--amber:#d97706;--rose:#be123c;--sky:#0284c7;
--sh:0 10px 32px rgba(11,31,51,.08);--r:14px}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,'Segoe UI',sans-serif;background:var(--bg);color:var(--ink);min-height:100vh}
.wrap{max-width:960px;margin:0 auto;padding:20px 16px 40px}
.top{display:flex;justify-content:space-between;align-items:flex-end;gap:12px;flex-wrap:wrap;margin-bottom:18px}
.brand{display:flex;align-items:center;gap:12px}
.logo{width:42px;height:42px;border-radius:11px;background:#0b1f33;color:#9ef0e2;display:grid;place-items:center;font:600 .85rem monospace}
.brand h1{font-size:1.45rem;letter-spacing:-.03em;line-height:1;margin:0}
.brand h1 b{color:var(--teal)}
.brand p{color:var(--mut);font-size:.8rem;margin:4px 0 0}
.nav{display:flex;gap:8px;flex-wrap:wrap}
.nav a{text-decoration:none;color:var(--soft);background:var(--card);border:1px solid var(--line);border-radius:999px;padding:8px 14px;font-size:.8rem;font-weight:600}
.nav a.on{color:var(--td);border-color:#b7e0d8}
.pill{display:inline-flex;align-items:center;gap:7px;background:var(--card);border:1px solid var(--line);border-radius:999px;padding:7px 12px;font-size:.75rem;color:var(--soft)}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block}
.dok{background:var(--teal)}.derr{background:var(--rose)}.dwrn{background:var(--amber)}
.hero{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:20px 22px;box-shadow:var(--sh);margin-bottom:16px}
.eyebrow{font:500 .68rem monospace;letter-spacing:.12em;text-transform:uppercase;color:var(--mut);margin-bottom:8px}
.hero h2{font-size:1.35rem;margin:0}.hero h2.ok{color:var(--td)}.hero h2.bad{color:var(--rose)}
.hero .sub{color:var(--soft);font-size:.9rem;margin-top:6px;line-height:1.45}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}
.chip{font:500 .7rem monospace;background:#eef5f3;color:var(--td);border:1px solid #cce8e2;border-radius:8px;padding:5px 9px}
.chip.warn{background:#fff4e5;color:#9a5b05;border-color:#f5d7a6}
.kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px;margin-bottom:16px}
.kpi{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:14px;box-shadow:var(--sh)}
.kpi .l{font-size:.68rem;color:var(--mut);text-transform:uppercase;letter-spacing:.06em}
.kpi .v{font:600 1.7rem monospace;margin-top:4px}.kpi .u{font-size:.7rem;color:var(--mut)}
.safe{color:var(--td)}.warn{color:var(--amber)}.danger{color:var(--rose)}
.panel{background:var(--card);border:1px solid var(--line);border-radius:var(--r);box-shadow:var(--sh);margin-bottom:14px;overflow:hidden}
.phd{padding:12px 16px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:center}
.phd h3{font-size:.92rem;margin:0}.pbd{padding:14px 16px}
.fs{margin:0 0 14px;padding:16px;background:var(--card);border:1px solid var(--line);border-radius:var(--r);box-shadow:var(--sh)}
.fs.hi{border:2px solid var(--teal);background:#f3fbf9}
.fs h2{font-size:1rem;margin:0 0 4px}.fs .lead{color:var(--mut);font-size:.8rem;margin:0 0 12px;line-height:1.4}
label{display:block;font-size:.8rem;color:var(--soft);margin:14px 0 6px;font-weight:600}
input,select{display:block;width:100%;min-height:48px;background:#fff;border:2px solid #9eb6c8;border-radius:10px;
padding:12px 14px;color:var(--ink);font:inherit;font-size:1rem;-webkit-appearance:none;appearance:none}
input:focus{outline:none;border-color:var(--teal);box-shadow:0 0 0 3px rgba(15,157,138,.2)}
.hint{font-size:.72rem;color:var(--mut);margin-top:6px;line-height:1.35}
.btn{display:inline-block;padding:12px 18px;border-radius:10px;border:none;font:600 .9rem system-ui,sans-serif;cursor:pointer;text-decoration:none;margin:10px 8px 0 0}
.bp{background:var(--teal);color:#fff}.bd{background:var(--rose);color:#fff}
.bg{background:var(--card);color:var(--ink);border:1px solid var(--line)}
.pi{display:flex;gap:8px;align-items:center;margin-bottom:8px}.pi input{flex:1}
.rm,.add{border:none;border-radius:8px;padding:9px 12px;cursor:pointer;font-size:.8rem;font-weight:600}
.rm{background:#fde8ec;color:var(--rose)}.add{background:#e6f4fc;color:#0369a1;margin-top:4px}
.mok{padding:11px 14px;border-radius:10px;margin-bottom:12px;font-size:.84rem;background:#e8f7f3;border:1px solid #9ad9ce;color:var(--td)}
.merr{padding:11px 14px;border-radius:10px;margin-bottom:12px;font-size:.84rem;background:#fde8ec;border:1px solid #f5c2cd;color:var(--rose)}
table{width:100%;border-collapse:collapse;font-size:.8rem}
th{text-align:left;padding:9px 12px;color:var(--mut);font-size:.68rem;text-transform:uppercase;border-bottom:1px solid var(--line);background:#f7fafc}
td{padding:10px 12px;border-bottom:1px solid #eef2f6}
.SEVERE{color:var(--rose);font-weight:700}.MODERATE{color:var(--amber);font-weight:600}.MINOR{color:var(--sky)}
.badge{display:inline-block;padding:3px 9px;border-radius:999px;font-size:.7rem;background:#eef5f3;border:1px solid #cce8e2;color:var(--td);margin:0 4px 4px 0}
.foot{margin-top:18px;color:var(--mut);font-size:.72rem;display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap}
.foot a{color:var(--td)}
.onb{max-width:480px;margin:12px auto}
.ipbox{font:600 1rem monospace;color:var(--td);background:#e8f7f3;border:1px solid #9ad9ce;border-radius:12px;padding:14px;text-align:center;margin:14px 0}
.steps{margin:12px 0;padding-left:18px;color:var(--soft);font-size:.84rem;line-height:1.55}
.mono{font-family:monospace}.empty{color:var(--mut);padding:18px;text-align:center}
</style>
)CSS";

static void htmlHead(const char* title, bool autoRefresh = false) {
    webServer.sendContent(F("<!DOCTYPE html><html lang=en><head>"
        "<meta charset=UTF-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"));
    if (autoRefresh) webServer.sendContent(F("<meta http-equiv=refresh content=4>"));
    webServer.sendContent(F("<title>"));
    webServer.sendContent(title);
    webServer.sendContent(F("</title>"));
    // FPSTR: send PROGMEM CSS correctly (plain sendContent can truncate/mangle)
    webServer.sendContent_P(PAGE_CSS);
    webServer.sendContent(F("</head><body><div class=wrap>"));
}

static void htmlTop(const char* active) {
    webServer.sendContent(F(
        "<div class=top><div class=brand><div class=logo>VX</div><div>"
        "<h1>Veloc<b>is</b></h1><p>Device console</p></div></div><nav class=nav>"));

    webServer.sendContent(F("<a href=/"));
    if (strcmp(active, "live") == 0) webServer.sendContent(F(" class=on"));
    webServer.sendContent(F(">Live</a>"));

    webServer.sendContent(F("<a href=/wifi"));
    if (strcmp(active, "wifi") == 0) webServer.sendContent(F(" class=on"));
    webServer.sendContent(F(">Wi-Fi Setup</a>"));

    webServer.sendContent(F("<a href=/settings"));
    if (strcmp(active, "settings") == 0) webServer.sendContent(F(" class=on"));
    webServer.sendContent(F(">Settings</a></nav></div>"));
}

static void htmlFoot() {
    webServer.sendContent(F(
        "<div class=foot><span>Velocis device firmware</span>"
        "<span><a href=/api/status>API status</a> · <a href=/wifi>Wi-Fi setup</a></span>"
        "</div></div></body></html>"));
}

// Escape so SSID never breaks value='...'
static void sendHtmlAttr(const char* s) {
    if (!s) return;
    for (; *s; ++s) {
        if (*s == '&')      webServer.sendContent(F("&amp;"));
        else if (*s == '\'') webServer.sendContent(F("&#39;"));
        else if (*s == '"')  webServer.sendContent(F("&quot;"));
        else if (*s == '<')  webServer.sendContent(F("&lt;"));
        else {
            char c[2] = { *s, 0 };
            webServer.sendContent(c);
        }
    }
}

// ── Wi-Fi onboarding (captive portal target) ─────────────────
void sendWifiSetup() {
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/html"), "");

    htmlHead("Wi-Fi Setup — Velocis");
    htmlTop("wifi");

    // FORM FIRST — SoftAP pages sometimes truncate; inputs must be early
    webServer.sendContent(F(
        "<div class=onb>"
        "<div class='fs hi'>"
        "<h2>Enter Wi-Fi name &amp; password</h2>"
        "<p class=lead>Type your home/office network below, then tap Save &amp; Connect.</p>"
        "<form method=POST action=/wifi>"
        "<label for=wifiSSID>Wi-Fi name (SSID)</label>"
        "<input id=wifiSSID name=wifiSSID type=text required maxlength=63 "
        "placeholder=\"Your network name\" value=\""));
    sendHtmlAttr(cfg.wifiSSID);
    webServer.sendContent(F("\" autocomplete=off autocapitalize=none spellcheck=false>"
        "<label for=wifiPass>Wi-Fi password</label>"
        "<input id=wifiPass name=wifiPass type=text maxlength=63 "
        "placeholder=\"Your network password\" value=\""));
    sendHtmlAttr(cfg.wifiPass);
    webServer.sendContent(F("\" autocomplete=off autocapitalize=none spellcheck=false>"
        "<p class=hint>Not the hotspot password. Leave blank only for open networks.</p>"
        "<button class=\"btn bp\" type=submit>Save &amp; Connect</button>"
        "</form></div>"));

    if (webServer.hasArg("err"))
        webServer.sendContent(F("<div class=merr>Could not join that network. Check name/password and try again.</div>"));
    if (webServer.hasArg("ok"))
        webServer.sendContent(F("<div class=mok>Saved. Connecting &amp; testing internet…</div>"));

    {
        char buf[240];
        IPAddress ip = state.apActive ? WiFi.softAPIP() : currentDeviceIP();
        snprintf(buf, sizeof(buf),
            "<div class=ipbox>http://%s/wifi</div>"
            "<ol class=steps>"
            "<li>Join phone to <b>%s</b> (pass <b>%s</b>)</li>"
            "<li>Fill the fields above with your router Wi-Fi</li>"
            "<li>Tap Save &amp; Connect</li>"
            "</ol>"
            "<p class=hint style=\"text-align:center\"><a href=/settings>All settings</a></p>"
            "</div>",
            ip.toString().c_str(), AP_SSID, AP_PASS);
        webServer.sendContent(buf);
    }

    htmlFoot();
    webServer.sendContent("");
}

// ── Dashboard ─────────────────────────────────────────────────
void sendDashboard() {
    if (state.mode == MODE_PROVISIONING) {
        sendWifiSetup();
        return;
    }
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/html"), "");

    htmlHead("Velocis — Live", true);
    htmlTop("live");

    // Status hero
    {
        const bool alert = state.violationTier > 0;
        const char* title = !state.gpsValid ? "Acquiring GPS fix"
                          : alert ? "Speed limit exceeded"
                          : "Speed compliant";
        webServer.sendContent(F("<div class='hero'><div class='eyebrow'>Live monitor</div><h2 class='"));
        webServer.sendContent(alert ? "bad" : "ok");
        webServer.sendContent(F("'>"));
        webServer.sendContent(title);
        webServer.sendContent(F("</h2><p class='sub'>"));
        webServer.sendContent(cfg.deviceID);
        webServer.sendContent(F(" · session desk</p><div class='chips'>"));

        char chip[120];
        snprintf(chip, sizeof(chip),
            "<span class='chip'>%s</span>"
            "<span class='chip%s'>%s</span>"
            "<span class='chip'>%s</span>",
            modeLabel(),
            state.internetOk ? "" : " warn",
            state.wifiConnected ? (state.internetOk ? "Internet OK" : "Wi-Fi · no net")
                                : (state.apActive ? "Setup AP" : "Offline"),
            currentDeviceIP().toString().c_str());
        webServer.sendContent(chip);
        webServer.sendContent(F("</div></div>"));
    }

    // KPI row
    webServer.sendContent(F("<div class='kpis'>"));
    {
        const char* cls = (state.violationTier == 0) ? "safe"
                        : (state.violationTier <= 2) ? "warn" : "danger";
        char buf[420];
        snprintf(buf, sizeof(buf),
            "<div class='kpi'><div class='l'>Speed</div><div class='v %s'>%d</div><div class='u'>km/h</div></div>"
            "<div class='kpi'><div class='l'>Limit</div><div class='v'>%d</div><div class='u'>km/h</div></div>"
            "<div class='kpi'><div class='l'>Alert</div><div class='v %s'>%s</div></div>"
            "<div class='kpi'><div class='l'>Violations</div><div class='v'>%lu</div><div class='u'>this session</div></div>"
            "<div class='kpi'><div class='l'>GPS</div><div class='v'>%s</div><div class='u'>%d sats</div></div>"
            "<div class='kpi'><div class='l'>Peak</div><div class='v warn'>%d</div><div class='u'>km/h</div></div>",
            cls, (int)state.currentSpeed,
            (int)state.speedLimit,
            cls,
            state.violationTier == 0 ? "SAFE" :
            state.violationTier == 1 ? "MINOR" :
            state.violationTier == 2 ? "MODERATE" : "SEVERE",
            state.totalViolations,
            state.gpsValid ? "LOCK" : "…",
            gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
            (int)state.maxSpeedSeen);
        webServer.sendContent(buf);
    }
    webServer.sendContent(F("</div>"));

    // System strip
    {
        char buf[280];
        snprintf(buf, sizeof(buf),
            "<div class='panel'><div class='phd'><h3>System</h3>"
            "<span class='pill'><span class='dot %s'></span>Wi-Fi</span></div>"
            "<div class='pbd' style='display:flex;flex-wrap:wrap;gap:10px;align-items:center'>"
            "<span class='pill'><span class='dot %s'></span>GSM</span>"
            "<span class='pill'><span class='dot %s'></span>GPS</span>"
            "<span class='pill mono'>IP %s</span>"
            "<span class='pill'>Heap %u B</span>"
            "<a class='btn bp' href='/wifi' style='margin:0'>Change Wi-Fi</a>"
            "</div></div>",
            state.wifiConnected ? "dok" : (state.apActive ? "dwrn" : "derr"),
            state.gsmReady ? "dok" : "derr",
            state.gpsValid ? "dok" : "dwrn",
            currentDeviceIP().toString().c_str(),
            ESP.getFreeHeap());
        webServer.sendContent(buf);
    }

    // SMS recipients
    webServer.sendContent(F("<div class='panel'><div class='phd'><h3>SMS recipients</h3></div><div class='pbd'>"));
    for (int i = 0; i < cfg.numPhones; i++) {
        webServer.sendContent(F("<span class='badge'>"));
        webServer.sendContent(cfg.phones[i]);
        webServer.sendContent(F("</span>"));
    }
    webServer.sendContent(F("</div></div>"));

    // Violations
    webServer.sendContent(F("<div class='panel'><div class='phd'><h3>Recent violations</h3></div>"));
    if (logCount == 0) {
        webServer.sendContent(F("<div class='empty'>No violations this session.</div>"));
    } else {
        webServer.sendContent(F("<table><thead><tr>"
            "<th>#</th><th>Tier</th><th>Speed</th><th>Limit</th><th>Excess</th><th>Location</th>"
            "</tr></thead><tbody>"));
        int start = (logHead - logCount + LOG_SIZE) % LOG_SIZE;
        for (int i = logCount - 1; i >= 0; i--) {
            int idx = (start + i) % LOG_SIZE;
            ViolationRecord& r = violationLog[idx];
            char row[220];
            snprintf(row, sizeof(row),
                "<tr><td class='mono'>%d</td><td class='%s'>%s</td>"
                "<td class='mono'>%d</td><td class='mono'>%d</td><td class='mono'>+%d</td>"
                "<td class='mono' style='font-size:.72rem'>%.4f, %.4f</td></tr>",
                logCount - i, r.tier_str, r.tier_str,
                (int)r.speed, (int)r.limit, (int)(r.speed - r.limit),
                r.lat, r.lon);
            webServer.sendContent(row);
        }
        webServer.sendContent(F("</tbody></table>"));
    }
    webServer.sendContent(F("</div>"));

    htmlFoot();
    webServer.sendContent("");
}

// ── Settings page ─────────────────────────────────────────────
void sendSettings() {
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/html"), "");

    htmlHead("Settings — Velocis");
    htmlTop("settings");

    if (webServer.hasArg("saved"))
        webServer.sendContent(F("<div class='mok'>Settings saved.</div>"));

    webServer.sendContent(F("<form method='POST' action='/settings'>"));

    // Wi-Fi — highlighted
    webServer.sendContent(F(
        "<div class='fs hi'>"
        "<h2>Wi-Fi name &amp; password</h2>"
        "<p class=lead>Enter your home/office network here. After save, the device reconnects and tests internet.</p>"
        "<label for=wifiSSID>Wi-Fi name (SSID)</label>"
        "<input id=wifiSSID name=wifiSSID type=text maxlength=63 placeholder=\"Network name\" value=\""));
    sendHtmlAttr(cfg.wifiSSID);
    webServer.sendContent(F("\" autocomplete=off autocapitalize=none>"
        "<label for=wifiPass>Wi-Fi password</label>"
        "<input id=wifiPass name=wifiPass type=text maxlength=63 placeholder=\"Network password\" value=\""));
    sendHtmlAttr(cfg.wifiPass);
    webServer.sendContent(F("\" autocomplete=off autocapitalize=none>"
        "<p class=hint>Clear the Wi-Fi name and save to reopen setup hotspot "
        "(GPS-SpeedMonitor / speed1234). Or hold MENU 3 seconds on the device. "
        "Or open <a href=/wifi>Wi-Fi Setup</a>.</p></div>"));

    // Server
    webServer.sendContent(F(
        "<div class='fs'><h2>Remote server</h2>"
        "<label>Server URL (HTTP POST)</label>"
        "<input name='serverURL' value='"));
    webServer.sendContent(cfg.serverURL);
    webServer.sendContent(F("'><p class='hint'>Use http:// — HTTPS needs too much RAM on ESP32</p>"
        "<label>Device ID</label><input name='deviceID' value='"));
    webServer.sendContent(cfg.deviceID);
    webServer.sendContent(F("'></div>"));

    // Thresholds
    {
        char buf[480];
        snprintf(buf, sizeof(buf),
            "<div class='fs'><h2>Speed thresholds</h2>"
            "<label>Default speed limit (km/h)</label>"
            "<input name='defLimit' type='number' value='%d' min='10' max='200'>"
            "<label>Minor (km/h over)</label>"
            "<input name='thrMinor' type='number' value='%d' min='1' max='50'>"
            "<label>Moderate (km/h over)</label>"
            "<input name='thrModerate' type='number' value='%d' min='1' max='50'>"
            "<label>Severe + SMS (km/h over)</label>"
            "<input name='thrSevere' type='number' value='%d' min='1' max='100'>"
            "</div>",
            cfg.defaultSpeedLimit, cfg.threshMinor, cfg.threshModerate, cfg.threshSevere);
        webServer.sendContent(buf);
    }

    // Phones
    webServer.sendContent(F("<div class='fs'><h2>SMS recipients</h2><div id='pl'>"));
    for (int i = 0; i < cfg.numPhones; i++) {
        webServer.sendContent(F("<div class='pi'><input name='phone' value='"));
        webServer.sendContent(cfg.phones[i]);
        webServer.sendContent(F("' placeholder='+234...'>"
            "<button type='button' class='rm' onclick='rm(this)'>Remove</button></div>"));
    }
    webServer.sendContent(F("</div>"
        "<button type='button' class='add' onclick='add()'>+ Add number</button>"
        "<p class='hint'>International format, e.g. +2347059011222 (max 10)</p></div>"));

    webServer.sendContent(F(
        "<button class='btn bp' type='submit'>Save settings</button>"
        "<button class='btn bd' type='button' "
        "onclick=\"if(confirm('Send test SMS?'))location='/test-sms'\">Test SMS</button>"
        "</form>"
        "<script>"
        "function add(){var l=document.getElementById('pl');"
        "if(l.children.length>=10){alert('Max 10');return;}"
        "var d=document.createElement('div');d.className='pi';"
        "d.innerHTML=\"<input name='phone' placeholder='+234...'>"
        "<button type='button' class='rm' onclick='rm(this)'>Remove</button>\";"
        "l.appendChild(d);}"
        "function rm(b){var l=document.getElementById('pl');"
        "if(l.children.length<=1){alert('Need at least one');return;}"
        "b.parentElement.remove();}"
        "</script>"));

    htmlFoot();
    webServer.sendContent("");
}

// ── Status JSON ───────────────────────────────────────────────
void sendStatusJSON() {
    StaticJsonDocument<512> doc;
    doc["device"]           = cfg.deviceID;
    doc["speed"]            = state.currentSpeed;
    doc["speed_limit"]      = state.speedLimit;
    doc["violation_tier"]   = state.violationTier;
    doc["gps_valid"]        = state.gpsValid;
    doc["wifi_connected"]   = state.wifiConnected;
    doc["ap_active"]        = state.apActive;
    doc["internet_ok"]      = state.internetOk;
    doc["wifi_configured"]  = state.wifiConfigured;
    doc["mode"]             = modeLabel();
    doc["ip"]               = currentDeviceIP().toString();
    doc["gsm_ready"]        = state.gsmReady;
    doc["total_violations"] = state.totalViolations;
    doc["max_speed"]        = state.maxSpeedSeen;
    doc["uptime_ms"]        = millis();
    doc["free_heap"]        = ESP.getFreeHeap();
    if (gps.satellites.isValid()) doc["satellites"] = gps.satellites.value();
    if (gps.hdop.isValid())       doc["hdop"]       = gps.hdop.hdop();
    String out;
    out.reserve(400);
    serializeJsonPretty(doc, out);
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", out);
}

// ── Violations JSON ───────────────────────────────────────────
void sendViolationsJSON() {
    // Stream the JSON manually to avoid DynamicJsonDocument heap spike
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", "");
    webServer.sendContent(F("{\"violations\":["));
    int startIdx = (logHead - logCount + LOG_SIZE) % LOG_SIZE;
    for (int i = 0; i < logCount; i++) {
        int idx = (startIdx + i) % LOG_SIZE;
        ViolationRecord& r = violationLog[idx];
        char buf[200];
        snprintf(buf, sizeof(buf),
            "%s{\"speed\":%.1f,\"limit\":%.1f,\"excess\":%.1f,"
            "\"tier\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"uptime\":%lu}",
            i ? "," : "", r.speed, r.limit, r.speed - r.limit,
            r.tier_str, r.lat, r.lon, r.timestamp);
        webServer.sendContent(buf);
    }
    char tail[40];
    snprintf(tail, sizeof(tail), "],\"total\":%d}", logCount);
    webServer.sendContent(tail);
    webServer.sendContent("");
}

// ═══════════════════════════════════════════════════════════
//  WEB SERVER ROUTES
// ═══════════════════════════════════════════════════════════
void setupWebServer() {
    webServer.on("/", HTTP_GET, sendDashboard);
    webServer.on("/wifi", HTTP_GET, sendWifiSetup);

    webServer.on("/wifi", HTTP_POST, []() {
        String ssid = webServer.hasArg("wifiSSID") ? webServer.arg("wifiSSID") : "";
        String pass = webServer.hasArg("wifiPass") ? webServer.arg("wifiPass") : "";
        ssid.trim();
        if (ssid.length() == 0) {
            webServer.sendHeader("Location", "/wifi?err=1");
            webServer.send(303);
            return;
        }

        webServer.sendHeader("Location", "/wifi?ok=1");
        webServer.send(303);
        delay(200);
        applyWiFiCredentials(ssid.c_str(), pass.c_str());
    });

    webServer.on("/settings", HTTP_GET, sendSettings);

    webServer.on("/settings", HTTP_POST, []() {
        bool wifiChanged = false;
        char prevSSID[64];
        strncpy(prevSSID, cfg.wifiSSID, 63);
        prevSSID[63] = '\0';

        if (webServer.hasArg("wifiSSID")) {
            strncpy(cfg.wifiSSID, webServer.arg("wifiSSID").c_str(), 63);
            cfg.wifiSSID[63] = '\0';
        }
        if (webServer.hasArg("wifiPass")) {
            strncpy(cfg.wifiPass, webServer.arg("wifiPass").c_str(), 63);
            cfg.wifiPass[63] = '\0';
        }
        if (webServer.hasArg("serverURL"))
            strncpy(cfg.serverURL, webServer.arg("serverURL").c_str(), 127);
        if (webServer.hasArg("deviceID"))
            strncpy(cfg.deviceID, webServer.arg("deviceID").c_str(), 31);
        if (webServer.hasArg("defLimit"))
            cfg.defaultSpeedLimit = webServer.arg("defLimit").toInt();
        if (webServer.hasArg("thrMinor"))
            cfg.threshMinor = webServer.arg("thrMinor").toInt();
        if (webServer.hasArg("thrModerate"))
            cfg.threshModerate = webServer.arg("thrModerate").toInt();
        if (webServer.hasArg("thrSevere"))
            cfg.threshSevere = webServer.arg("thrSevere").toInt();

        int n = 0;
        for (int i = 0; i < webServer.args() && n < MAX_PHONE_NUMBERS; i++) {
            if (webServer.argName(i) == "phone") {
                String num = webServer.arg(i);
                num.trim();
                if (num.length() >= 7) {
                    strncpy(cfg.phones[n], num.c_str(), 19);
                    cfg.phones[n][19] = '\0';
                    n++;
                }
            }
        }
        if (n > 0) cfg.numPhones = n;

        wifiChanged = (strcmp(prevSSID, cfg.wifiSSID) != 0) ||
                      webServer.hasArg("wifiPass");
        state.wifiConfigured = strlen(cfg.wifiSSID) > 0;
        saveConfig();

        if (wifiChanged && !state.wifiConfigured) {
            webServer.sendHeader("Location", "/wifi");
            webServer.send(303);
            delay(200);
            enterProvisioning("SSID cleared");
            startProvisioningAP();
            renderUi(true);
            return;
        }

        if (wifiChanged && state.wifiConfigured) {
            webServer.sendHeader("Location", "/?reconnecting=1");
            webServer.send(303);
            delay(200);
            applyWiFiCredentials(cfg.wifiSSID, cfg.wifiPass);
            return;
        }

        webServer.sendHeader("Location", "/settings?saved=1");
        webServer.send(303);
    });

    webServer.on("/api/status",     HTTP_GET, sendStatusJSON);
    webServer.on("/api/violations", HTTP_GET, sendViolationsJSON);

    webServer.on("/test-sms", HTTP_GET, []() {
        char msg[160];
        snprintf(msg, sizeof(msg),
            "TEST\nDevice: %s\nSMS working OK.\nSpeed: %.0f km/h",
            cfg.deviceID, state.currentSpeed);
        bool ok = sendSMS(msg);
        webServer.send(200, "text/html",
            String(F("<meta http-equiv='refresh' content='2;url=/settings'>")) +
            (ok ? F("<p style='color:green'>SMS sent!</p>")
                : F("<p style='color:red'>SMS failed</p>")));
    });

    webServer.on("/reboot", HTTP_GET, []() {
        webServer.send(200, "text/plain", "Rebooting...");
        delay(500);
        ESP.restart();
    });

    // Captive portal: redirect unknown hosts/paths to setup
    webServer.onNotFound([]() {
        if (state.mode == MODE_PROVISIONING && state.apActive) {
            String loc = String("http://") + WiFi.softAPIP().toString() + "/wifi";
            webServer.sendHeader("Location", loc, true);
            webServer.send(302, "text/plain", "");
            return;
        }
        webServer.send(404, "text/plain", "Not found");
    });

    webServer.begin();
    Serial.println(F("[Web]  Server started on port 80"));
}