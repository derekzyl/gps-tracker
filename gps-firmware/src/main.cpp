/**
 * ============================================================
 *  GPS Speed Limit Alert & Monitoring System
 *  ESP32-WROOM Firmware  —  v1.1  (crash-fix build)
 * ============================================================
 *
 *  FIXES vs v1.0
 *  -------------
 *  1. HTTP instead of HTTPS → saves ~40 KB heap (TLS was killing it)
 *  2. Web pages sent as chunked streams with F() literals → no giant
 *     String heap allocations that caused LoadProhibited crashes
 *  3. Watchdog-safe SMS send loop (Core 0 WiFi task no longer starves)
 *  4. SMS response parser waits for "+CMGS:" not just "OK"
 *  5. Heap monitor printed every 5 s for diagnostics
 *  6. yield() used in all blocking loops to keep WiFi task fed
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
 *  GPIO 18  →  Button 1  (menu / confirm)
 *  GPIO 19  →  Button 2  (scroll / cancel)
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
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
Preferences        prefs;

// ── Runtime state ────────────────────────────────────────────
struct SystemState {
    float   currentSpeed   = 0.0f;
    float   speedLimit     = DEFAULT_SPEED_LIMIT;
    float   prevLat        = 0.0f;
    float   prevLon        = 0.0f;
    unsigned long prevFixMs = 0;

    bool    gpsValid       = false;
    bool    wifiConnected  = false;
    bool    gsmReady       = false;

    int     violationTier  = 0;
    unsigned long lastSmsMs  = 0;
    unsigned long lastPostMs = 0;

    unsigned long totalViolations = 0;
    float   maxSpeedSeen   = 0.0f;
} state;

// ── NVS-backed config ────────────────────────────────────────
struct Config {
    // FIX 1: HTTP not HTTPS — saves ~40 KB of heap (no TLS context)
    char   wifiSSID[64]       = "cybergenii";
    char   wifiPass[64]       = "12341234";
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
void handleButtons();
void sendDashboard();
void sendSettings();
void sendStatusJSON();
void sendViolationsJSON();
void gsmSendAT(const char*, unsigned long timeout = 1000);
String gsmReadResponse(unsigned long timeout = 2000);
String gsmWaitFor(const char* token, unsigned long timeout);

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial.println(F("\n=== GPS Speed Monitor v1.1 — System Starting ==="));

    // ── GPIO ─────────────────────────────────────────────────
    pinMode(PIN_LED_RED,    OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_GREEN,  OUTPUT);
    pinMode(BTN_MENU,       INPUT_PULLUP);
    pinMode(BTN_SCROLL,     INPUT_PULLUP);
    digitalWrite(PIN_LED_RED,    LOW);
    digitalWrite(PIN_LED_YELLOW, LOW);
    digitalWrite(PIN_LED_GREEN,  LOW);

    // ── I2C / LCD ─────────────────────────────────────────────
    Wire.begin(LCD_SDA, LCD_SCL);
    lcd.init();
    lcd.backlight();
    updateLCD("GPS Speed v1.1", "Starting...");

    // ── NVS config ────────────────────────────────────────────
    loadConfig();

    // ── UARTs ─────────────────────────────────────────────────
    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    gsmSerial.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    Serial.println(F("[GPS]  UART2 started on RX=16 TX=17"));
    Serial.println(F("[GSM]  UART1 started on RX=13 TX=14"));

    // ── Wi-Fi ─────────────────────────────────────────────────
    updateLCD("Connecting WiFi", cfg.wifiSSID);
    connectWiFi();

    // ── GSM ───────────────────────────────────────────────────
    updateLCD("Init GSM...", "SIM800L");
    state.gsmReady = initGSM();
    Serial.printf("[GSM]  Ready: %s\n", state.gsmReady ? "YES" : "NO");

    // ── Web server ────────────────────────────────────────────
    setupWebServer();

    // ── Ready ─────────────────────────────────────────────────
    updateLCD("System Ready", "Acquiring GPS..");
    digitalWrite(PIN_LED_GREEN, HIGH);

    if (state.wifiConnected) {
        Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("[Web]  http://%s/\n", WiFi.localIP().toString().c_str());
    }

    // NOTE: Startup test POST + SMS removed — they caused TG1WDT resets
    // before the main loop could start. Use /test-sms from the web UI instead.

    Serial.println(F("[OK]   Setup complete — system running\n"));
}

// ═══════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════
void loop() {
    while (gpsSerial.available() > 0)
        gps.encode(gpsSerial.read());

    webServer.handleClient();
    handleButtons();

    if (gps.location.isUpdated() && gps.location.isValid())
        handleGPS();

    // Blink green while waiting for GPS lock
    static unsigned long lastBlink = 0;
    if (!state.gpsValid && millis() - lastBlink > 1000) {
        lastBlink = millis();
        digitalWrite(PIN_LED_GREEN, !digitalRead(PIN_LED_GREEN));
    }

    // FIX 5: Heap diagnostic every 5 s
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

    char line1[17], line2[17];
    snprintf(line1, sizeof(line1), "Spd:%3.0f Lim:%3.0f", speed, state.speedLimit);
    float excess = speed - state.speedLimit;
    if (excess > 0)
        snprintf(line2, sizeof(line2), "OVER +%.0fkm/h!", excess);
    else
        snprintf(line2, sizeof(line2), "SAFE            ");
    updateLCD(line1, line2);

    processViolation(speed, state.speedLimit, lat, lon);

    state.prevLat   = lat;
    state.prevLon   = lon;
    state.prevFixMs = now;

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
//  LCD
// ═══════════════════════════════════════════════════════════
void updateLCD(const char* line1, const char* line2) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(line1);
    lcd.setCursor(0, 1); lcd.print(line2);
}

// ═══════════════════════════════════════════════════════════
//  BUTTONS
// ═══════════════════════════════════════════════════════════
void handleButtons() {
    static unsigned long lastBtn1 = 0, lastBtn2 = 0;
    static int menuMode = 0;

    if (digitalRead(BTN_MENU) == LOW && millis() - lastBtn1 > 300) {
        lastBtn1 = millis();
        menuMode = (menuMode + 1) % 3;
        char l1[17], l2[17];
        switch (menuMode) {
            case 0:
                snprintf(l1, 17, "Spd:%3.0f Lim:%3.0f", state.currentSpeed, state.speedLimit);
                snprintf(l2, 17, "Viols: %lu", state.totalViolations);
                updateLCD(l1, l2);
                break;
            case 1:
                updateLCD("Settings IP:", state.wifiConnected
                    ? WiFi.localIP().toString().c_str() : "No WiFi");
                break;
            case 2:
                snprintf(l2, 17, "Max:%.0f km/h", state.maxSpeedSeen);
                updateLCD("Session Stats", l2);
                break;
        }
    }

    if (digitalRead(BTN_SCROLL) == LOW && millis() - lastBtn2 > 300) {
        lastBtn2 = millis();
        unsigned long held = millis();
        while (digitalRead(BTN_SCROLL) == LOW && millis() - held < 3000) {
            delay(50); yield();
        }
        if (millis() - held >= 3000) {
            updateLCD("Re-init GSM...", "");
            state.gsmReady = initGSM();
            updateLCD("GSM:", state.gsmReady ? "OK" : "FAILED");
        } else {
            char l1[17], l2[17];
            snprintf(l1, 17, "Sats:%d HDOP:%.1f",
                gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f);
            snprintf(l2, 17, "Fix:%s", state.gpsValid ? "ACTIVE" : "SEARCHING");
            updateLCD(l1, l2);
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  WI-FI
// ═══════════════════════════════════════════════════════════
void connectWiFi() {
    if (strlen(cfg.wifiSSID) == 0) {
        WiFi.softAP("GPS-SpeedMonitor", "speed1234");
        Serial.printf("[WiFi] AP mode — IP: %s\n", WiFi.softAPIP().toString().c_str());
        state.wifiConnected = true;
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    unsigned long start = millis();
    Serial.printf("[WiFi] Connecting to %s", cfg.wifiSSID);
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
        delay(500); Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        state.wifiConnected = true;
        Serial.printf("[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println(F("[WiFi] Failed — starting AP mode"));
        WiFi.softAP("GPS-SpeedMonitor", "speed1234");
        state.wifiConnected = true;
    }
}

// ═══════════════════════════════════════════════════════════
//  NVS CONFIG
// ═══════════════════════════════════════════════════════════
void loadConfig() {
    prefs.begin("gps-monitor", true);  // read-only open

    // Only read a key if it actually exists — prevents the
    // "[E][Preferences.cpp] nvs_get_str len fail: KEY NOT_FOUND" log spam
    // for fields that are simply using their hardcoded defaults.
    if (prefs.isKey("wifiSSID"))
        strncpy(cfg.wifiSSID,  prefs.getString("wifiSSID").c_str(),  63);
    if (prefs.isKey("wifiPass"))
        strncpy(cfg.wifiPass,  prefs.getString("wifiPass").c_str(),  63);
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
        // First run — populate from hardcoded defaults
        for (int i = 0; i < N_DEFAULT_NUMBERS; i++)
            strncpy(cfg.phones[i], DEFAULT_NUMBERS[i], 19);
        cfg.numPhones = N_DEFAULT_NUMBERS;
    }

    prefs.end();
    Serial.printf("[NVS]  Config loaded — %d phone number(s)\n", cfg.numPhones);
}

void saveConfig() {
    prefs.begin("gps-monitor", false);
    prefs.putString("wifiSSID",  cfg.wifiSSID);
    prefs.putString("wifiPass",  cfg.wifiPass);
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
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#0d1117;color:#e6edf3;min-height:100vh}
.hdr{background:#161b22;padding:16px 24px;border-bottom:1px solid #30363d;
     display:flex;justify-content:space-between;align-items:center}
.hdr h1{font-size:1.2rem;color:#58a6ff}
.nav{background:#161b22;padding:8px 24px;border-bottom:1px solid #30363d}
.nav a{color:#58a6ff;text-decoration:none;margin-right:16px;font-size:.9rem}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));
      gap:16px;padding:24px}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:20px;text-align:center}
.card .val{font-size:2.2rem;font-weight:700;margin:8px 0}
.card .lbl{font-size:.75rem;color:#8b949e;text-transform:uppercase;letter-spacing:.05em}
.safe{color:#3fb950}.warn{color:#e3b341}.danger{color:#f85149}
.sec{padding:0 24px 24px}
.sec h2{font-size:1rem;color:#8b949e;margin-bottom:12px;border-bottom:1px solid #30363d;padding-bottom:8px}
table{width:100%;border-collapse:collapse;font-size:.85rem}
th{background:#21262d;padding:8px 12px;text-align:left;border-bottom:1px solid #30363d;color:#8b949e}
td{padding:8px 12px;border-bottom:1px solid #21262d}
.SEVERE{color:#f85149;font-weight:700}.MODERATE{color:#e3b341;font-weight:600}.MINOR{color:#79c0ff}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
.dok{background:#3fb950}.derr{background:#f85149}.dwrn{background:#e3b341}
.badge{display:inline-block;padding:2px 8px;border-radius:12px;font-size:.75rem;
       margin-right:4px;background:#21262d;border:1px solid #30363d}
/* settings */
.fs{background:#161b22;border:1px solid #30363d;border-radius:8px;margin:20px;padding:20px}
.fs h2{font-size:.95rem;color:#79c0ff;margin-bottom:16px;border-bottom:1px solid #30363d;padding-bottom:8px}
label{display:block;font-size:.8rem;color:#8b949e;margin-bottom:4px;margin-top:12px}
input{width:100%;background:#0d1117;border:1px solid #30363d;border-radius:6px;
      padding:8px 12px;color:#e6edf3;font-size:.9rem}
input:focus{outline:none;border-color:#58a6ff}
.btn{display:inline-block;padding:10px 20px;border-radius:6px;border:none;
     font-size:.9rem;cursor:pointer;margin-top:16px}
.bp{background:#238636;color:#fff}.bp:hover{background:#2ea043}
.bd{background:#da3633;color:#fff;margin-left:8px}
.pi{display:flex;align-items:center;gap:8px;margin-bottom:6px}
.pi input{flex:1}
.pi .rm{background:#da3633;color:#fff;border:none;border-radius:4px;
        padding:6px 10px;cursor:pointer;font-size:.8rem}
.add{background:#1f6feb;color:#fff;border:none;border-radius:4px;
     padding:8px 14px;cursor:pointer;margin-top:8px;font-size:.85rem}
.hint{font-size:.75rem;color:#6e7681;margin-top:4px}
.mok{padding:10px 16px;border-radius:6px;margin:0 20px 12px;font-size:.85rem;
     background:#0f2918;border:1px solid #238636;color:#3fb950}
</style>
)CSS";

// ── Helper: open HTML head ────────────────────────────────────
static void htmlHead(const char* title, bool autoRefresh = false) {
    webServer.sendContent(F("<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"));
    if (autoRefresh) webServer.sendContent(F("<meta http-equiv='refresh' content='3'>"));
    webServer.sendContent(F("<title>"));
    webServer.sendContent(title);
    webServer.sendContent(F("</title>"));
    webServer.sendContent(PAGE_CSS);
    webServer.sendContent(F("</head><body>"));
}

static void htmlNav(bool onSettings = false) {
    webServer.sendContent(F("<div class='nav'>"
        "<a href='/'>&#x1F4CA; Dashboard</a>"
        "<a href='/settings'>&#x2699;&#xFE0F; Settings</a>"
        "<a href='/api/status'>&#x1F4E1; API</a>"
        "</div>"));
}

// ── Dashboard ─────────────────────────────────────────────────
void sendDashboard() {
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/html"), "");

    htmlHead("GPS Speed Monitor", true);

    // Header
    webServer.sendContent(F("<div class='hdr'><h1>&#x1F6E6; GPS Speed Monitor</h1><span>"));
    webServer.sendContent(cfg.deviceID);
    webServer.sendContent(F("</span></div>"));
    htmlNav();

    // Cards grid
    webServer.sendContent(F("<div class='grid'>"));

    // Speed card
    {
        const char* cls = (state.violationTier == 0) ? "safe"
                        : (state.violationTier <= 2) ? "warn" : "danger";
        char buf[80];
        snprintf(buf, sizeof(buf),
            "<div class='card'><div class='lbl'>Speed</div>"
            "<div class='val %s'>%d</div><div class='lbl'>km/h</div></div>",
            cls, (int)state.currentSpeed);
        webServer.sendContent(buf);
    }

    // Limit card
    {
        char buf[80];
        snprintf(buf, sizeof(buf),
            "<div class='card'><div class='lbl'>Limit</div>"
            "<div class='val'>%d</div><div class='lbl'>km/h</div></div>",
            (int)state.speedLimit);
        webServer.sendContent(buf);
    }

    // Tier card
    {
        const char* t = (state.violationTier == 0) ? "SAFE"
                      : (state.violationTier == 1) ? "MINOR"
                      : (state.violationTier == 2) ? "MODERATE" : "SEVERE";
        char buf[100];
        snprintf(buf, sizeof(buf),
            "<div class='card'><div class='lbl'>Alert</div>"
            "<div class='val %s'>%s</div><div class='lbl'>&nbsp;</div></div>",
            t, t);
        webServer.sendContent(buf);
    }

    // Violations, GPS, Max speed cards
    {
        char buf[200];
        snprintf(buf, sizeof(buf),
            "<div class='card'><div class='lbl'>Violations</div>"
            "<div class='val'>%lu</div><div class='lbl'>session</div></div>"
            "<div class='card'><div class='lbl'>GPS</div>"
            "<div class='val'>%s</div><div class='lbl'>%d sats</div></div>"
            "<div class='card'><div class='lbl'>Max Speed</div>"
            "<div class='val warn'>%d</div><div class='lbl'>km/h</div></div>",
            state.totalViolations,
            state.gpsValid ? "<span class='safe'>LOCK</span>" : "<span class='warn'>...</span>",
            gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
            (int)state.maxSpeedSeen);
        webServer.sendContent(buf);
    }

    webServer.sendContent(F("</div>"));  // end grid

    // System status
    {
        char buf[220];
        snprintf(buf, sizeof(buf),
            "<div class='sec'><h2>System</h2><p>"
            "<span class='dot %s'></span>WiFi &nbsp;"
            "<span class='dot %s'></span>GSM &nbsp;"
            "<span class='dot %s'></span>GPS &nbsp; IP: %s"
            " &nbsp;|&nbsp; Free heap: %u B</p></div>",
            state.wifiConnected ? "dok" : "derr",
            state.gsmReady      ? "dok" : "derr",
            state.gpsValid      ? "dok" : "dwrn",
            state.wifiConnected ? WiFi.localIP().toString().c_str() : "—",
            ESP.getFreeHeap());
        webServer.sendContent(buf);
    }

    // Phone list
    webServer.sendContent(F("<div class='sec'><h2>SMS Recipients</h2><p>"));
    for (int i = 0; i < cfg.numPhones; i++) {
        webServer.sendContent(F("<span class='badge'>"));
        webServer.sendContent(cfg.phones[i]);
        webServer.sendContent(F("</span>"));
    }
    webServer.sendContent(F("</p></div>"));

    // Violation log
    webServer.sendContent(F("<div class='sec'><h2>Recent Violations</h2>"));
    if (logCount == 0) {
        webServer.sendContent(F("<p style='color:#8b949e;font-size:.85rem'>None recorded.</p>"));
    } else {
        webServer.sendContent(F("<table><thead><tr>"
            "<th>#</th><th>Tier</th><th>Speed</th><th>Limit</th><th>Excess</th><th>Location</th>"
            "</tr></thead><tbody>"));
        int start = (logHead - logCount + LOG_SIZE) % LOG_SIZE;
        for (int i = logCount - 1; i >= 0; i--) {
            int idx = (start + i) % LOG_SIZE;
            ViolationRecord& r = violationLog[idx];
            char row[200];
            snprintf(row, sizeof(row),
                "<tr><td>%d</td><td class='%s'>%s</td>"
                "<td>%d km/h</td><td>%d km/h</td><td>+%d km/h</td>"
                "<td style='font-size:.75rem'>%.5f, %.5f</td></tr>",
                logCount - i, r.tier_str, r.tier_str,
                (int)r.speed, (int)r.limit, (int)(r.speed - r.limit),
                r.lat, r.lon);
            webServer.sendContent(row);
        }
        webServer.sendContent(F("</tbody></table>"));
    }
    webServer.sendContent(F("</div>"));

    webServer.sendContent(F(
        "<div style='padding:16px 24px;color:#8b949e;font-size:.75rem'>"
        "Auto-refreshes every 3 s &nbsp;|&nbsp; "
        "<a href='/settings' style='color:#58a6ff'>Settings</a></div>"
        "</body></html>"));

    webServer.sendContent("");  // end chunked
}

// ── Settings page ─────────────────────────────────────────────
void sendSettings() {
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/html"), "");

    htmlHead("Settings — GPS Speed Monitor");
    webServer.sendContent(F("<div class='hdr'><h1>&#x2699;&#xFE0F; Settings</h1></div>"));
    htmlNav(true);

    if (webServer.hasArg("saved"))
        webServer.sendContent(F("<div class='mok'>&#x2713; Settings saved.</div>"));

    webServer.sendContent(F("<form method='POST' action='/settings'>"));

    // Wi-Fi section
    webServer.sendContent(F("<div class='fs'><h2>&#x1F4F6; Wi-Fi</h2>"
        "<label>SSID</label><input name='wifiSSID' value='"));
    webServer.sendContent(cfg.wifiSSID);
    webServer.sendContent(F("'><label>Password</label>"
        "<input name='wifiPass' type='password' value='"));
    webServer.sendContent(cfg.wifiPass);
    webServer.sendContent(F("'><p class='hint'>Leave blank → AP mode (GPS-SpeedMonitor / speed1234)</p></div>"));

    // Server section
    webServer.sendContent(F("<div class='fs'><h2>&#x1F5A5;&#xFE0F; Remote Server</h2>"
        "<label>Server URL (HTTP POST)</label>"
        "<input name='serverURL' value='"));
    webServer.sendContent(cfg.serverURL);
    webServer.sendContent(F("'><p class='hint'>Use http:// — HTTPS requires too much RAM on ESP32</p>"
        "<label>Device ID</label><input name='deviceID' value='"));
    webServer.sendContent(cfg.deviceID);
    webServer.sendContent(F("'></div>"));

    // Thresholds section
    {
        char buf[400];
        snprintf(buf, sizeof(buf),
            "<div class='fs'><h2>&#x26A1; Speed Thresholds</h2>"
            "<label>Default Speed Limit (km/h)</label>"
            "<input name='defLimit' type='number' value='%d' min='10' max='200'>"
            "<label>Minor (km/h over limit)</label>"
            "<input name='thrMinor' type='number' value='%d' min='1' max='50'>"
            "<label>Moderate (km/h over limit)</label>"
            "<input name='thrModerate' type='number' value='%d' min='1' max='50'>"
            "<label>Severe + SMS (km/h over limit)</label>"
            "<input name='thrSevere' type='number' value='%d' min='1' max='100'>"
            "</div>",
            cfg.defaultSpeedLimit, cfg.threshMinor, cfg.threshModerate, cfg.threshSevere);
        webServer.sendContent(buf);
    }

    // Phone numbers section
    webServer.sendContent(F("<div class='fs'><h2>&#x1F4F1; SMS Recipients</h2>"
        "<div id='pl'>"));
    for (int i = 0; i < cfg.numPhones; i++) {
        webServer.sendContent(F("<div class='pi'><input name='phone' value='"));
        webServer.sendContent(cfg.phones[i]);
        webServer.sendContent(F("' placeholder='+234...'>"
            "<button type='button' class='rm' onclick='rm(this)'>Remove</button></div>"));
    }
    webServer.sendContent(F("</div>"
        "<button type='button' class='add' onclick='add()'>+ Add Number</button>"
        "<p class='hint'>International format, e.g. +2347059011222 (max 10)</p></div>"));

    // Submit row
    webServer.sendContent(F("<div style='margin:0 20px 32px'>"
        "<button class='btn bp' type='submit'>&#x1F4BE; Save Settings</button>"
        "<button class='btn bd' type='button' "
        "onclick=\"if(confirm('Send test SMS?'))location='/test-sms'\">Test SMS</button>"
        "</div></form>"));

    // Tiny inline JS
    webServer.sendContent(F("<script>"
        "function add(){"
        "var l=document.getElementById('pl');"
        "if(l.children.length>=10){alert('Max 10');return;}"
        "var d=document.createElement('div');d.className='pi';"
        "d.innerHTML=\"<input name='phone' placeholder='+234...'>"
        "<button type='button' class='rm' onclick='rm(this)'>Remove</button>\";"
        "l.appendChild(d);}"
        "function rm(b){"
        "var l=document.getElementById('pl');"
        "if(l.children.length<=1){alert('Need at least one');return;}"
        "b.parentElement.remove();}"
        "</script></body></html>"));

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

    webServer.on("/settings", HTTP_GET, sendSettings);

    webServer.on("/settings", HTTP_POST, []() {
        if (webServer.hasArg("wifiSSID"))
            strncpy(cfg.wifiSSID, webServer.arg("wifiSSID").c_str(), 63);
        if (webServer.hasArg("wifiPass"))
            strncpy(cfg.wifiPass, webServer.arg("wifiPass").c_str(), 63);
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

        saveConfig();
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

    webServer.begin();
    Serial.println(F("[Web]  Server started on port 80"));
}