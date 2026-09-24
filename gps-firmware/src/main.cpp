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
 *  GPIO 25  →  Red  LED + Active Buzzer (alert)
 *  GPIO 26  →  Yellow LED               (moderate warning)
 *  GPIO 27  →  Green LED                (speed compliant)
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
#include <ArduinoOTA.h>
#include <esp_sleep.h>
#include <qrcode.h>

// ═══════════════════════════════════════════════════════════
//  PIN DEFINITIONS
// ═══════════════════════════════════════════════════════════
#define PIN_BUZZER       25
#define PIN_LED_RED      25
// Red LED + buzzer on GPIO 25 sound when the pin is driven HIGH.
#define ALARM_ON         HIGH
#define ALARM_OFF        LOW
#define PIN_LED_YELLOW   26
#define PIN_LED_GREEN    27

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
#define BTN_DEBOUNCE_MS       30     // level must be stable this long to count
#define BTN_LONG_MS           3000
#define BTN_HOLD_HINT_MS      800    // tap if released sooner; show hold bar after this
#define INTERNET_TEST_MS      4000
#define PROV_LCD_ROTATE_MS    3000
#define HEARTBEAT_MS          30000
#define GEO_SYNC_MS           300000
#define TRACK_BUF             60         // GPS log points kept in RAM (~5 min moving)
#define TRACK_MOVING_MS       5000
#define TRACK_PARKED_MS       60000
#define TRACK_UPLOAD_MS       15000
#define TRACK_RETRY_MS        60000      // back-off after a failed upload
#define TRACK_BATCH           20
#define QUEUE_SIZE            12
#define MAX_DYN_ZONES         16
#define PARKED_SPEED_KPH      2.0f
#define PARKED_TIMEOUT_MS     300000UL   // 5 min stationary → deep sleep
#define SLEEP_DURATION_US     120000000ULL // wake every 2 min to check
#define WAKE_BTN_PIN          BTN_MENU

#define AP_SSID               "GPS-SpeedMonitor"
#define AP_PASS               "speed1234"
#define DNS_PORT              53

// ── UI / network modes ───────────────────────────────────────
enum UiScreen : uint8_t {
    UI_SPEED = 0,
    UI_LIMIT,
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
    "+2348135993811",
    "+2348056322139",
    "+2347059011222",
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

    unsigned long lastHeartbeatMs = 0;
    unsigned long lastGeoSyncMs   = 0;
    int     pendingQueue   = 0;
    int     dynZoneCount   = 0;
    uint32_t geofenceVersion = 0;
    unsigned long parkedSinceMs = 0;
    bool    sleepEnabled   = true;
    unsigned long buzzerMuteUntilMs = 0;
} state;

// ── NVS-backed config ────────────────────────────────────────
struct Config {
    // Empty SSID → first boot enters SoftAP onboarding
    char   wifiSSID[64]       = "";
    char   wifiPass[64]       = "";
    char   serverURL[128]     = "http://visiting-carmella-cybergenii-895c1fde.koyeb.app/api/violation";
    char   deviceID[32]       = "ESP32-SPEED-01";
    char   apiKey[48]         = "";
    int    defaultSpeedLimit  = DEFAULT_SPEED_LIMIT;  // the limit you set (manual mode)
    bool   autoZones          = false;                // true: map zones override the set limit
    uint32_t limitRev         = 0;                    // last server limit revision applied
    int    threshMinor        = THRESH_MINOR;
    int    threshModerate     = THRESH_MODERATE;
    int    threshSevere       = THRESH_SEVERE;
    int    numPhones          = N_DEFAULT_NUMBERS;
    char   phones[MAX_PHONE_NUMBERS][20];
} cfg;

// Dynamic geofences from server (fallback = compiled ZONES)
struct DynZone {
    float latMin, latMax, lonMin, lonMax, limitKph;
    char  label[24];
} dynZones[MAX_DYN_ZONES];

// Offline store-and-forward queue
struct QueuedPost {
    bool  used;
    float speed, limit, lat, lon;
    char  tier[10];
} postQueue[QUEUE_SIZE];

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

// ── GPS track ring buffer (newest trackUnsent points await upload) ──
struct TrackPoint {
    float lat, lon, speed, limit;
    unsigned long ms;
};
TrackPoint trackBuf[TRACK_BUF];
int trackHead   = 0;
int trackCount  = 0;
int trackUnsent = 0;
unsigned long trackTotal        = 0;
unsigned long trackUploaded     = 0;
unsigned long lastTrackMs       = 0;
unsigned long nextTrackUploadMs = 0;

// ── Alert blink state ────────────────────────────────────────
unsigned long lastBlinkMs = 0;
bool blinkState = false;

// Transient LCD message (e.g. "Alarm muted") protected from the live refresh until this time.
unsigned long lcdHoldUntil = 0;

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
void setSpeedLimit(int kph, bool autoZones);
void processViolation(float, float, float, float);
void triggerAlert(int);
void silenceAlert();
bool sendSMS(const char*);
bool sendSMSTo(const char* phone, const char* message);
void sanitizeSmsText(char* dest, size_t destLen, const char* src);
void saveRecipientsFromRequest();
bool postToServer(float, float, float, float, const char*);
void enqueuePost(float, float, float, float, const char*);
void flushPostQueue();
bool sendHeartbeat();
bool syncGeofences();
void recordTrackPoint(float lat, float lon, float speed, float limit);
bool uploadTrack();
void applyServerLimit(JsonVariantConst lim);
void sendTrackJSON();
static void showLcdMsg(const char* l1, const char* l2, unsigned long ms);
void setupOTA();
String serverBaseURL();
void addApiHeaders(HTTPClient& http);
void maybeEnterDeepSleep();
void enterDeepSleep();
void printWakeReason();
void updateLCD(const char*, const char*);
void logViolation(float, float, float, float, int);
void truncate16(char* dest, const char* src);
void renderUi(bool force = false);
void renderProvisioningLcd();
void renderStatusScreen(UiScreen screen);
void setupButtons();
void handleButtons();
bool buttonHoldActive();
void sendDashboard();
void sendSettings();
void sendWifiSetup();
void sendSmsTest();
void sendWifiScanJSON();
void sendSetupQrHtml(const char* url);
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
// ── Custom LCD 5x8 Glyphs ────────────────────────────────────
const uint8_t GLYPH_SAT[8]   = { 0b00100, 0b01110, 0b11111, 0b00100, 0b01010, 0b10001, 0b00000, 0b00000 };
const uint8_t GLYPH_WIFI[8]  = { 0b00000, 0b01110, 0b10001, 0b00100, 0b01010, 0b00000, 0b00100, 0b00000 };
const uint8_t GLYPH_GSM[8]   = { 0b10001, 0b01010, 0b00100, 0b00100, 0b01110, 0b00100, 0b00100, 0b00100 };
const uint8_t GLYPH_ALERT[8] = { 0b00100, 0b01110, 0b01110, 0b01110, 0b00100, 0b00000, 0b00100, 0b00000 };
const uint8_t GLYPH_FULL[8]  = { 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111 };
const uint8_t GLYPH_HALF[8]  = { 0b11100, 0b11100, 0b11100, 0b11100, 0b11100, 0b11100, 0b11100, 0b11100 };
const uint8_t GLYPH_CHECK[8] = { 0b00000, 0b00001, 0b00011, 0b10110, 0b11100, 0b01000, 0b00000, 0b00000 };

void initLcdGlyphs() {
    lcd.createChar(0, (uint8_t*)GLYPH_SAT);
    lcd.createChar(1, (uint8_t*)GLYPH_WIFI);
    lcd.createChar(2, (uint8_t*)GLYPH_GSM);
    lcd.createChar(3, (uint8_t*)GLYPH_ALERT);
    lcd.createChar(4, (uint8_t*)GLYPH_FULL);
    lcd.createChar(5, (uint8_t*)GLYPH_HALF);
    lcd.createChar(6, (uint8_t*)GLYPH_CHECK);
}

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println(F("\n=== Velocis Firmware v1.4 — System Starting ==="));
    printWakeReason();

    memset(postQueue, 0, sizeof(postQueue));

    digitalWrite(PIN_LED_RED, ALARM_OFF);   // latch OFF before enabling output: no chirp at boot
    pinMode(PIN_LED_RED,    OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_GREEN,  OUTPUT);
    pinMode(BTN_MENU,       INPUT_PULLUP);
    pinMode(BTN_SCROLL,     INPUT_PULLUP);
    digitalWrite(PIN_LED_RED,    ALARM_OFF);
    digitalWrite(PIN_LED_YELLOW, LOW);
    digitalWrite(PIN_LED_GREEN,  LOW);

    Wire.begin(LCD_SDA, LCD_SCL);
    lcd.init();
    lcd.backlight();
    initLcdGlyphs();
    updateLCD("Velocis v1.4", "Starting...");

    loadConfig();
    state.speedLimit = (float)cfg.defaultSpeedLimit;
    setupButtons();

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
    if (state.wifiConnected) {
        setupOTA();
        syncGeofences();
        sendHeartbeat();
    }

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
    if (state.wifiConnected)
        ArduinoOTA.handle();

    handleButtons();
    renderUi(false);

    if (gps.location.isUpdated() && gps.location.isValid())
        handleGPS();

    unsigned long now = millis();

    // Fix lost: stop checking a stale speed and silence any alarm.
    if (state.gpsValid && gps.location.age() > GPS_STALE_MS * 2) {
        state.gpsValid = false;
        state.currentSpeed = 0.0f;
        state.prevFixMs = 0;
        state.violationTier = 0;
        silenceAlert();
        Serial.println(F("[GPS]  Fix lost"));
    }

    // Alarm blink cadence (200–500 ms) needs a faster tick than 1 Hz GPS fixes.
    if (state.gpsValid && state.violationTier > 0)
        triggerAlert(state.violationTier);
    if (state.wifiConnected && state.internetOk) {
        if (now - state.lastHeartbeatMs >= HEARTBEAT_MS) {
            state.lastHeartbeatMs = now;
            sendHeartbeat();
            flushPostQueue();
        }
        if (trackUnsent > 0 && (long)(now - nextTrackUploadMs) >= 0) {
            nextTrackUploadMs = now + (uploadTrack() ? TRACK_UPLOAD_MS : TRACK_RETRY_MS);
        }
        if (now - state.lastGeoSyncMs >= GEO_SYNC_MS) {
            state.lastGeoSyncMs = now;
            syncGeofences();
        }
    }

    static unsigned long lastBlink = 0;
    if (!state.gpsValid && millis() - lastBlink > 1000) {
        lastBlink = millis();
        digitalWrite(PIN_LED_GREEN, !digitalRead(PIN_LED_GREEN));
    }

    static unsigned long lastHeap = 0;
    if (millis() - lastHeap > 5000) {
        lastHeap = millis();
        Serial.printf("[MEM]  Free heap: %u bytes  queue=%d zones=%d\n",
                      ESP.getFreeHeap(), state.pendingQueue, state.dynZoneCount);
    }

    maybeEnterDeepSleep();
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
    // Blend with module-reported speed when available (smoother at low speed)
    if (gps.speed.isValid()) {
        float gpsKph = gps.speed.kmph();
        if (speed <= 0.5f) speed = gpsKph;
        else speed = 0.65f * speed + 0.35f * gpsKph;
    }
    // Reject impossible spikes
    if (state.currentSpeed > 1.0f && speed > state.currentSpeed * 2.5f && speed > 40.0f)
        speed = state.currentSpeed;

    state.currentSpeed = speed;
    if (speed > state.maxSpeedSeen) state.maxSpeedSeen = speed;

    // LCD is owned by renderUi() — do not overwrite here
    processViolation(speed, state.speedLimit, lat, lon);
    recordTrackPoint(lat, lon, speed, state.speedLimit);

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
    if (!cfg.autoZones) return (float)cfg.defaultSpeedLimit;

    for (int i = 0; i < state.dynZoneCount; i++)
        if (lat >= dynZones[i].latMin && lat <= dynZones[i].latMax &&
            lon >= dynZones[i].lonMin && lon <= dynZones[i].lonMax)
            return dynZones[i].limitKph;

    for (int i = 0; i < N_ZONES; i++)
        if (lat >= ZONES[i].latMin && lat <= ZONES[i].latMax &&
            lon >= ZONES[i].lonMin && lon <= ZONES[i].lonMax)
            return ZONES[i].limitKph;
    return (float)cfg.defaultSpeedLimit;
}

// Applies immediately (display + alarm) and persists to NVS.
void setSpeedLimit(int kph, bool autoZones) {
    if (kph < 5)   kph = 5;
    if (kph > 250) kph = 250;
    cfg.defaultSpeedLimit = kph;
    cfg.autoZones = autoZones;
    state.speedLimit = state.gpsValid ? getSpeedLimit(state.prevLat, state.prevLon)
                                      : (float)kph;
    if (state.currentSpeed <= state.speedLimit && state.violationTier > 0) {
        state.violationTier = 0;
        silenceAlert();
    }
    saveConfig();
    Serial.printf("[LIMIT] %d km/h (%s) -> effective %.0f\n",
                  kph, autoZones ? "AUTO zones" : "MANUAL", state.speedLimit);
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

    // One violation per event: entering over-limit or escalating tier, not every fix.
    bool newEvent = tier > state.violationTier;
    state.violationTier = tier;
    triggerAlert(tier);

    const char* tierStr = (tier == 3) ? "SEVERE" : (tier == 2) ? "MODERATE" : "MINOR";
    if (newEvent) {
        state.totalViolations++;
        logViolation(speed, limit, lat, lon, tier);
        Serial.printf("[ALERT] Tier=%d (%s)  Excess=+%.1f km/h\n", tier, tierStr, excess);
    }

    unsigned long now = millis();

    if (tier == 3) {
        if (now - state.lastSmsMs > SMS_COOLDOWN_MS) {
            // Keep under 160 chars (one SMS); the link opens Google Maps on the phone.
            char msg[160];
            snprintf(msg, sizeof(msg),
                "SPEED ALERT [%s]\nDevice: %s\nSpeed: %.0f km/h in %.0f km/h zone\n"
                "Excess: +%.0f km/h\nhttps://maps.google.com/?q=%.5f,%.5f",
                tierStr, cfg.deviceID, speed, limit, excess, lat, lon);
            if (sendSMS(msg)) state.lastSmsMs = now;
        }
        if (now - state.lastPostMs > 5000) {
            if (!postToServer(speed, limit, lat, lon, tierStr))
                enqueuePost(speed, limit, lat, lon, tierStr);
            else
                state.lastPostMs = millis();
        }
    } else if (tier == 2) {
        if (now - state.lastPostMs > 5000) {
            if (!postToServer(speed, limit, lat, lon, tierStr))
                enqueuePost(speed, limit, lat, lon, tierStr);
            else
                state.lastPostMs = millis();
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  ALERT OUTPUTS
// ═══════════════════════════════════════════════════════════
void triggerAlert(int tier) {
    unsigned long now = millis();
    bool buzzerMuted = (now < state.buzzerMuteUntilMs);
    switch (tier) {
        case 0:
            digitalWrite(PIN_LED_GREEN,  HIGH);
            digitalWrite(PIN_LED_YELLOW, LOW);
            digitalWrite(PIN_LED_RED,    ALARM_OFF);
            break;
        case 1:
            digitalWrite(PIN_LED_GREEN, LOW);
            if (now - lastBlinkMs > 500) { blinkState = !blinkState; lastBlinkMs = now; }
            digitalWrite(PIN_LED_YELLOW, blinkState);
            digitalWrite(PIN_LED_RED,    ALARM_OFF);
            break;
        case 2:
            digitalWrite(PIN_LED_GREEN,  LOW);
            digitalWrite(PIN_LED_YELLOW, HIGH);
            if (now - lastBlinkMs > 300) { blinkState = !blinkState; lastBlinkMs = now; }
            digitalWrite(PIN_LED_RED, (!buzzerMuted && blinkState) ? ALARM_ON : ALARM_OFF);
            break;
        case 3:
            digitalWrite(PIN_LED_GREEN,  LOW);
            digitalWrite(PIN_LED_YELLOW, LOW);
            if (now - lastBlinkMs > 200) { blinkState = !blinkState; lastBlinkMs = now; }
            digitalWrite(PIN_LED_RED, (!buzzerMuted && blinkState) ? ALARM_ON : ALARM_OFF);
            break;
    }
}

void silenceAlert() {
    digitalWrite(PIN_LED_RED,    ALARM_OFF);
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
//  SMS
// ═══════════════════════════════════════════════════════════
void sanitizeSmsText(char* dest, size_t destLen, const char* src) {
    if (!dest || destLen == 0) return;
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < destLen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 32 && c < 127) dest[j++] = (char)c;
        else if (c == '\n' || c == '\r') dest[j++] = ' ';
        else dest[j++] = '-';
    }
    dest[j] = '\0';
}

bool sendSMSTo(const char* phone, const char* message) {
    if (!phone || strlen(phone) < 7 || !message) return false;

    if (!state.gsmReady) {
        Serial.println(F("[SMS]  GSM not ready — re-init"));
        state.gsmReady = initGSM();
        if (!state.gsmReady) return false;
    }

    char body[161];
    sanitizeSmsText(body, sizeof(body), message);

    Serial.printf("[SMS]  Sending to %s ...\n", phone);
    gsmSendAT("AT+CMGF=1", 300);
    gsmReadResponse(300);

    String cmd = String("AT+CMGS=\"") + phone + "\"";
    gsmSerial.println(cmd);

    String prompt = gsmWaitFor(">", 8000);
    if (prompt.indexOf(">") == -1) {
        Serial.printf("[SMS]  No > prompt for %s\n", phone);
        gsmSerial.write(27);
        delay(300);
        return false;
    }

    gsmSerial.print(body);
    gsmSerial.write(26);

    String resp = gsmWaitFor("+CMGS:", 20000);
    bool ok = resp.indexOf("+CMGS:") != -1 || resp.indexOf("OK") != -1;
    Serial.printf("[SMS]  %s -> %s\n", phone, ok ? "OK" : "FAIL");

    unsigned long gap = millis();
    while (millis() - gap < 1200) yield();
    return ok;
}

bool sendSMS(const char* message) {
    bool any = false;
    for (int i = 0; i < cfg.numPhones; i++) {
        if (strlen(cfg.phones[i]) < 7) continue;
        if (sendSMSTo(cfg.phones[i], message)) any = true;
    }
    return any;
}

void saveRecipientsFromRequest() {
    int n = 0;
    // Prefer indexed phone0..phone9 (reliable on ESP WebServer)
    for (int i = 0; i < MAX_PHONE_NUMBERS; i++) {
        char key[12];
        snprintf(key, sizeof(key), "phone%d", i);
        if (!webServer.hasArg(key)) continue;
        String num = webServer.arg(key);
        num.trim();
        num.replace(" ", "");
        if (num.length() >= 7 && n < MAX_PHONE_NUMBERS) {
            strncpy(cfg.phones[n], num.c_str(), 19);
            cfg.phones[n][19] = '\0';
            n++;
        }
    }
    // Fallback: repeated name=phone
    if (n == 0) {
        for (int i = 0; i < webServer.args() && n < MAX_PHONE_NUMBERS; i++) {
            if (webServer.argName(i) != "phone") continue;
            String num = webServer.arg(i);
            num.trim();
            num.replace(" ", "");
            if (num.length() >= 7) {
                strncpy(cfg.phones[n], num.c_str(), 19);
                cfg.phones[n][19] = '\0';
                n++;
            }
        }
    }
    if (webServer.hasArg("clearPhones") && webServer.arg("clearPhones") == "1") {
        cfg.numPhones = n; // allow empty
    } else if (n > 0) {
        cfg.numPhones = n;
    }
    saveConfig();
}

// ═══════════════════════════════════════════════════════════
//  HTTP helpers + store-and-forward + heartbeat + geofences
// ═══════════════════════════════════════════════════════════
String serverBaseURL() {
    String u = cfg.serverURL;
    int api = u.indexOf("/api/");
    if (api > 0) return u.substring(0, api);
    int slash = u.lastIndexOf('/');
    if (slash > 8) return u.substring(0, slash);
    return u;
}

void addApiHeaders(HTTPClient& http) {
    http.addHeader("Content-Type", "application/json");
    if (strlen(cfg.apiKey) > 0)
        http.addHeader("X-API-Key", cfg.apiKey);
}

void enqueuePost(float speed, float limit, float lat, float lon, const char* tier) {
    for (int i = 0; i < QUEUE_SIZE; i++) {
        if (!postQueue[i].used) {
            postQueue[i].used = true;
            postQueue[i].speed = speed;
            postQueue[i].limit = limit;
            postQueue[i].lat = lat;
            postQueue[i].lon = lon;
            strncpy(postQueue[i].tier, tier ? tier : "MINOR", 9);
            postQueue[i].tier[9] = '\0';
            state.pendingQueue++;
            Serial.printf("[QUEUE] Stored offline post (%d pending)\n", state.pendingQueue);
            return;
        }
    }
    Serial.println(F("[QUEUE] Full — dropping oldest slot"));
    // overwrite slot 0
    postQueue[0].speed = speed;
    postQueue[0].limit = limit;
    postQueue[0].lat = lat;
    postQueue[0].lon = lon;
    strncpy(postQueue[0].tier, tier ? tier : "MINOR", 9);
    postQueue[0].tier[9] = '\0';
    postQueue[0].used = true;
}

bool postToServer(float speed, float limit, float lat, float lon, const char* tier) {
    if (!state.wifiConnected || strlen(cfg.serverURL) < 10) return false;

    HTTPClient http;
    http.begin(cfg.serverURL);
    addApiHeaders(http);
    http.setTimeout(SERVER_TIMEOUT_MS);

    StaticJsonDocument<320> doc;
    doc["device"]    = cfg.deviceID;
    doc["speed"]     = speed;
    doc["limit"]     = limit;
    doc["excess"]    = speed - limit;
    doc["tier"]      = tier;
    doc["lat"]       = lat;
    doc["lon"]       = lon;
    doc["timestamp"] = millis();
    if (strlen(cfg.apiKey) > 0) doc["api_key"] = cfg.apiKey;

    String payload;
    payload.reserve(240);
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

void flushPostQueue() {
    if (!state.wifiConnected || !state.internetOk || state.pendingQueue <= 0) return;
    for (int i = 0; i < QUEUE_SIZE; i++) {
        if (!postQueue[i].used) continue;
        if (postToServer(postQueue[i].speed, postQueue[i].limit,
                         postQueue[i].lat, postQueue[i].lon, postQueue[i].tier)) {
            postQueue[i].used = false;
            if (state.pendingQueue > 0) state.pendingQueue--;
            Serial.println(F("[QUEUE] Flushed one pending post"));
        } else {
            break; // stop if network failing
        }
        yield();
    }
}

bool sendHeartbeat() {
    if (!state.wifiConnected || strlen(cfg.serverURL) < 10) return false;

    String url = serverBaseURL() + "/api/heartbeat";
    HTTPClient http;
    http.begin(url);
    addApiHeaders(http);
    http.setTimeout(SERVER_TIMEOUT_MS);

    StaticJsonDocument<512> doc;
    doc["device"]        = cfg.deviceID;
    doc["speed"]         = state.currentSpeed;
    doc["limit"]         = state.speedLimit;
    doc["lat"]           = state.gpsValid ? state.prevLat : 0;
    doc["lon"]           = state.gpsValid ? state.prevLon : 0;
    doc["gps_valid"]     = state.gpsValid;
    doc["wifi_rssi"]     = WiFi.RSSI();
    doc["free_heap"]     = ESP.getFreeHeap();
    doc["internet_ok"]   = state.internetOk;
    doc["queue"]         = state.pendingQueue;
    doc["track_unsent"]  = trackUnsent;
    doc["limit_setting"] = cfg.defaultSpeedLimit;
    doc["limit_mode"]    = cfg.autoZones ? "auto" : "manual";
    doc["limit_rev"]     = cfg.limitRev;
    if (strlen(cfg.apiKey) > 0) doc["api_key"] = cfg.apiKey;

    String payload;
    serializeJson(doc, payload);
    int code = http.POST(payload);

    bool ok = (code == 200 || code == 201);
    if (ok) {
        // Optionally refresh geofences from heartbeat response
        String body = http.getString();
        StaticJsonDocument<2048> resp;
        bool parsed = !deserializeJson(resp, body);
        if (parsed) applyServerLimit(resp["limit"]);
        if (parsed && resp["geofences"].is<JsonArray>()) {
            JsonArray arr = resp["geofences"].as<JsonArray>();
            int n = 0;
            for (JsonObject z : arr) {
                if (n >= MAX_DYN_ZONES) break;
                dynZones[n].latMin = z["lat_min"] | 0.0f;
                dynZones[n].latMax = z["lat_max"] | 0.0f;
                dynZones[n].lonMin = z["lon_min"] | 0.0f;
                dynZones[n].lonMax = z["lon_max"] | 0.0f;
                dynZones[n].limitKph = z["limit_kph"] | (float)cfg.defaultSpeedLimit;
                const char* name = z["name"] | "Zone";
                strncpy(dynZones[n].label, name, 23);
                dynZones[n].label[23] = '\0';
                n++;
            }
            state.dynZoneCount = n;
            state.geofenceVersion = resp["geofences_version"] | state.geofenceVersion;
        }
        Serial.printf("[HB]   OK zones=%d\n", state.dynZoneCount);
    } else {
        Serial.printf("[HB]   HTTP %d\n", code);
    }
    http.end();
    return ok;
}

// Server response {"limit":{"kph","mode","rev"}} = a dashboard edit newer than ours.
void applyServerLimit(JsonVariantConst lim) {
    if (lim.isNull()) return;
    uint32_t rev = lim["rev"] | 0u;
    if (rev <= cfg.limitRev) return;
    int kph = lim["kph"] | cfg.defaultSpeedLimit;
    const char* mode = lim["mode"] | "manual";
    bool autoZ = strcmp(mode, "auto") == 0;
    cfg.limitRev = rev;
    setSpeedLimit(kph, autoZ);   // also persists limitRev
    char l2[17];
    snprintf(l2, sizeof(l2), "%d km/h %s", cfg.defaultSpeedLimit, autoZ ? "AUTO" : "SET");
    showLcdMsg("Limit from srv", l2, 2500);
    Serial.printf("[LIMIT] Applied server limit rev %u\n", (unsigned)rev);
}

void recordTrackPoint(float lat, float lon, float speed, float limit) {
    if (fabsf(lat) < 0.0001f && fabsf(lon) < 0.0001f) return;
    unsigned long now = millis();
    unsigned long every = (speed >= PARKED_SPEED_KPH) ? TRACK_MOVING_MS : TRACK_PARKED_MS;
    if (trackTotal > 0 && now - lastTrackMs < every) return;
    lastTrackMs = now;

    TrackPoint& p = trackBuf[trackHead];
    p.lat = lat; p.lon = lon; p.speed = speed; p.limit = limit; p.ms = now;
    trackHead = (trackHead + 1) % TRACK_BUF;
    if (trackCount  < TRACK_BUF) trackCount++;
    if (trackUnsent < TRACK_BUF) trackUnsent++;
    trackTotal++;
}

// POSTs the oldest unsent points (up to TRACK_BATCH) to /api/track.
bool uploadTrack() {
    if (!state.wifiConnected || !state.internetOk || trackUnsent <= 0 ||
        strlen(cfg.serverURL) < 10) return false;

    int n = trackUnsent < TRACK_BATCH ? trackUnsent : TRACK_BATCH;
    int start = (trackHead - trackUnsent + TRACK_BUF) % TRACK_BUF;
    unsigned long now = millis();

    DynamicJsonDocument doc(4096);
    doc["device"]        = cfg.deviceID;
    doc["limit_setting"] = cfg.defaultSpeedLimit;
    doc["limit_mode"]    = cfg.autoZones ? "auto" : "manual";
    doc["limit_rev"]     = cfg.limitRev;
    if (strlen(cfg.apiKey) > 0) doc["api_key"] = cfg.apiKey;
    JsonArray pts = doc.createNestedArray("points");
    for (int i = 0; i < n; i++) {
        const TrackPoint& p = trackBuf[(start + i) % TRACK_BUF];
        JsonObject o = pts.createNestedObject();
        o["lat"]   = serialized(String(p.lat, 6));
        o["lon"]   = serialized(String(p.lon, 6));
        o["speed"] = serialized(String(p.speed, 1));
        o["limit"] = (int)p.limit;
        o["age_s"] = (now - p.ms) / 1000UL;
    }

    String payload;
    serializeJson(doc, payload);
    doc.clear();

    HTTPClient http;
    http.begin(serverBaseURL() + "/api/track");
    addApiHeaders(http);
    http.setTimeout(SERVER_TIMEOUT_MS);
    int code = http.POST(payload);

    bool ok = (code == 200 || code == 201);
    if (ok) {
        trackUnsent -= n;
        if (trackUnsent < 0) trackUnsent = 0;
        trackUploaded += n;
        StaticJsonDocument<256> resp;
        if (!deserializeJson(resp, http.getString()))
            applyServerLimit(resp["limit"]);
        Serial.printf("[TRACK] Uploaded %d pts (%d left)\n", n, trackUnsent);
    } else {
        Serial.printf("[TRACK] Upload HTTP %d\n", code);
    }
    http.end();
    return ok;
}

bool syncGeofences() {
    if (!state.wifiConnected) return false;
    String url = serverBaseURL() + "/api/geofences";
    HTTPClient http;
    http.begin(url);
    addApiHeaders(http);
    http.setTimeout(SERVER_TIMEOUT_MS);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[GEO]  Sync HTTP %d\n", code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    StaticJsonDocument<2048> doc;
    if (deserializeJson(doc, body)) return false;
    JsonArray arr = doc["geofences"].as<JsonArray>();
    if (arr.isNull()) return false;

    int n = 0;
    for (JsonObject z : arr) {
        if (n >= MAX_DYN_ZONES) break;
        bool active = z["active"] | true;
        if (!active) continue;
        dynZones[n].latMin = z["lat_min"] | 0.0f;
        dynZones[n].latMax = z["lat_max"] | 0.0f;
        dynZones[n].lonMin = z["lon_min"] | 0.0f;
        dynZones[n].lonMax = z["lon_max"] | 0.0f;
        dynZones[n].limitKph = z["limit_kph"] | (float)cfg.defaultSpeedLimit;
        const char* name = z["name"] | "Zone";
        strncpy(dynZones[n].label, name, 23);
        dynZones[n].label[23] = '\0';
        n++;
    }
    state.dynZoneCount = n;
    state.geofenceVersion = doc["geofences_version"] | 0;
    Serial.printf("[GEO]  Synced %d zones\n", n);
    return true;
}

void setupOTA() {
    ArduinoOTA.setHostname(cfg.deviceID);
    ArduinoOTA.onStart([]() { updateLCD("OTA Update...", "Do not power off"); });
    ArduinoOTA.onEnd([]() { updateLCD("OTA Done", "Rebooting..."); });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("[OTA] Error %u\n", e);
        updateLCD("OTA Failed", "Check serial");
    });
    ArduinoOTA.begin();
    Serial.println(F("[OTA]  Ready"));
}

void printWakeReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:  Serial.println(F("[SLEEP] Wake: MENU button")); break;
        case ESP_SLEEP_WAKEUP_TIMER: Serial.println(F("[SLEEP] Wake: timer")); break;
        default: Serial.println(F("[SLEEP] Wake: power-on / reset")); break;
    }
}

void enterDeepSleep() {
    flushPostQueue();
    if (state.wifiConnected && state.internetOk) {
        // RAM track buffer is lost in deep sleep: push what we can first.
        for (int i = 0; i < 3 && trackUnsent > 0; i++)
            if (!uploadTrack()) break;
        sendHeartbeat();
    }

    updateLCD("Parked sleep", "MENU to wake");
    Serial.printf("[SLEEP] Deep sleep %llu s (parked)\n", SLEEP_DURATION_US / 1000000ULL);
    delay(400);

    lcd.noBacklight();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
    // MENU (GPIO18) LOW wakes — INPUT_PULLUP, wake on low
    esp_sleep_enable_ext0_wakeup((gpio_num_t)WAKE_BTN_PIN, 0);
    esp_deep_sleep_start();
}

void maybeEnterDeepSleep() {
    if (!state.sleepEnabled) return;
    if (state.mode == MODE_PROVISIONING || state.mode == MODE_CONNECTING) return;
    if (state.pendingQueue > 0) return;          // finish uploads first
    if (state.violationTier > 0) {               // active alert
        state.parkedSinceMs = 0;
        return;
    }

    unsigned long now = millis();
    if (!state.gpsValid) {
        // No fix yet — don't sleep during acquisition for first 3 minutes
        if (now < 180000UL) return;
    }

    if (state.currentSpeed >= PARKED_SPEED_KPH) {
        state.parkedSinceMs = 0;
        return;
    }

    if (state.parkedSinceMs == 0)
        state.parkedSinceMs = now;
    else if (now - state.parkedSinceMs >= PARKED_TIMEOUT_MS)
        enterDeepSleep();
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
    // Overwrite padded lines instead of lcd.clear(): no flicker on 1 s live refresh.
    char b[17];
    snprintf(b, sizeof(b), "%-16.16s", line1 ? line1 : "");
    lcd.setCursor(0, 0); lcd.print(b);
    snprintf(b, sizeof(b), "%-16.16s", line2 ? line2 : "");
    lcd.setCursor(0, 1); lcd.print(b);
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
    memset(l1, ' ', 16); l1[16] = '\0';
    memset(l2, ' ', 16); l2[16] = '\0';

    switch (screen) {
        // Glyph 0 is written as \x08 (CGRAM alias): a literal \x00 would end the C string.
        case UI_SPEED: {
            if (state.gpsValid) {
                snprintf(l1, sizeof(l1), "\x08 %3.0fkm/h L:%-3.0f",
                         state.currentSpeed, state.speedLimit);
            } else {
                snprintf(l1, sizeof(l1), "\x08 Acquiring GPS");
            }

            float excess = state.currentSpeed - state.speedLimit;
            if (!state.gpsValid) {
                snprintf(l2, sizeof(l2), "Searching fix...");
            } else if (excess > 0) {
                snprintf(l2, sizeof(l2), "\x03 OVER +%2.0fkm/h!", excess);
            } else {
                int barCount = 0;
                if (state.speedLimit > 0) {
                    barCount = (int)((state.currentSpeed / state.speedLimit) * 8.0f);
                    if (barCount > 8) barCount = 8;
                    if (barCount < 0) barCount = 0;
                }
                char bar[9];
                for (int b = 0; b < 8; b++) {
                    bar[b] = (b < barCount) ? '\x04' : '-';
                }
                bar[8] = '\0';
                snprintf(l2, sizeof(l2), "%s \x06 SAFE", bar);
            }
            break;
        }
        case UI_LIMIT: {
            snprintf(l1, sizeof(l1), "Limit %3.0f %s",
                     state.speedLimit, cfg.autoZones ? "AUTO" : "SET");
            if (!state.gpsValid) {
                snprintf(l2, sizeof(l2), "SCROLL = change");
            } else {
                float diff = state.currentSpeed - state.speedLimit;
                if (diff > 0)
                    snprintf(l2, sizeof(l2), "Now %3.0f OVER%3.0f", state.currentSpeed, diff);
                else
                    snprintf(l2, sizeof(l2), "Now %3.0f ok -%.0f", state.currentSpeed, -diff);
            }
            break;
        }
        case UI_WIFI: {
            if (state.wifiConnected) {
                char ssid[12];
                truncate16(ssid, cfg.wifiSSID);
                snprintf(l1, sizeof(l1), "\x01 WiFi:%.9s", ssid);
                snprintf(l2, sizeof(l2), "%s %s",
                         WiFi.localIP().toString().c_str(),
                         state.internetOk ? "\x06" : "noNet");
            } else if (state.apActive) {
                snprintf(l1, sizeof(l1), "\x01 Setup AP Mode");
                snprintf(l2, sizeof(l2), "%s", WiFi.softAPIP().toString().c_str());
            } else {
                snprintf(l1, sizeof(l1), "\x01 WiFi: Offline");
                snprintf(l2, sizeof(l2), "Hold MENU setup");
            }
            break;
        }
        case UI_GPS: {
            int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
            float hdop = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f;
            snprintf(l1, sizeof(l1), "\x08 Sats:%-2d HDOP:%.1f", sats, hdop);
            snprintf(l2, sizeof(l2), "Fix:%s %4.1fkm",
                     state.gpsValid ? "\x06 3D" : "NO", state.currentSpeed);
            break;
        }
        case UI_STATS:
        default: {
            snprintf(l1, sizeof(l1), "Max:%-3.0f Viol:%-3lu",
                     state.maxSpeedSeen, state.totalViolations);
            snprintf(l2, sizeof(l2), "\x02 GSM:%s Q:%d",
                     state.gsmReady ? "\x06OK" : "--", state.pendingQueue);
            break;
        }
    }
    updateLCD(l1, l2);
}

void renderProvisioningLcd() {
    char l1[17], l2[17];
    switch (state.provPage % 4) {
        case 0:
            snprintf(l1, sizeof(l1), "WiFi Setup Mode");
            snprintf(l2, sizeof(l2), "Scan QR on phone");
            break;
        case 1:
            snprintf(l1, sizeof(l1), "Hotspot name:");
            snprintf(l2, sizeof(l2), "%.16s", AP_SSID);
            break;
        case 2:
            snprintf(l1, sizeof(l1), "Pass:%s", AP_PASS);
            snprintf(l2, sizeof(l2), "%s", WiFi.softAPIP().toString().c_str());
            break;
        default:
            snprintf(l1, sizeof(l1), "Open in browser");
            snprintf(l2, sizeof(l2), "%s/wifi", WiFi.softAPIP().toString().c_str());
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

    static unsigned long lastLiveMs = 0;
    if (!force && (now < lcdHoldUntil || buttonHoldActive())) return;
    if (!state.uiPaused && (force || now - state.lastUiMs >= UI_ROTATE_MS)) {
        if (!force)
            state.uiScreen = (UiScreen)((state.uiScreen + 1) % UI_COUNT);
        state.lastUiMs = now;
        lastLiveMs = now;
        renderStatusScreen(state.uiScreen);
    } else if (force) {
        state.lastUiMs = now;
        lastLiveMs = now;
        renderStatusScreen(state.uiScreen);
    } else if (now - lastLiveMs >= 1000) {
        lastLiveMs = now;
        renderStatusScreen(state.uiScreen);
    }
}

// ═══════════════════════════════════════════════════════════
//  BUTTONS  (active-LOW: 10k pull-up to 3.3V, press pulls pin to GND)
//  MENU   tap  → next screen (if the alarm is sounding: mute 60 s)
//  MENU   hold → Wi-Fi setup hotspot (3 s, progress bar shown)
//  SCROLL tap  → previous screen; on LIMIT screen: next limit preset
//  SCROLL hold → re-init GSM modem (3 s, progress bar shown)
// ═══════════════════════════════════════════════════════════
struct Button {
    uint8_t pin;
    bool    rawDown;
    bool    down;              // debounced state
    unsigned long rawAt;       // last raw level change
    unsigned long downAt;
    unsigned long upAt;
    bool    longDone;
    volatile bool          isrHit;
    volatile unsigned long isrAt;
};
Button btnMenu   = { BTN_MENU };
Button btnScroll = { BTN_SCROLL };

enum BtnEvent : uint8_t { BTN_NONE, BTN_TAP, BTN_HOLD, BTN_CANCEL };

void IRAM_ATTR isrMenu()   { btnMenu.isrHit = true;   btnMenu.isrAt = millis(); }
void IRAM_ATTR isrScroll() { btnScroll.isrHit = true; btnScroll.isrAt = millis(); }

void setupButtons() {
    pinMode(BTN_MENU,   INPUT_PULLUP);
    pinMode(BTN_SCROLL, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BTN_MENU),   isrMenu,   FALLING);
    attachInterrupt(digitalPinToInterrupt(BTN_SCROLL), isrScroll, FALLING);
}

static BtnEvent pollButton(Button& b, unsigned long now) {
    bool raw = digitalRead(b.pin) == LOW;
    if (raw != b.rawDown) { b.rawDown = raw; b.rawAt = now; }

    if (raw != b.down && now - b.rawAt >= BTN_DEBOUNCE_MS) {
        b.down = raw;
        b.isrHit = false;
        if (b.down) {
            b.downAt = now;
            b.longDone = false;
        } else {
            b.upAt = now;
            if (!b.longDone)
                return (now - b.downAt < BTN_HOLD_HINT_MS) ? BTN_TAP : BTN_CANCEL;
        }
    }

    if (b.down && !b.longDone && now - b.downAt >= BTN_LONG_MS) {
        b.longDone = true;
        return BTN_HOLD;
    }

    // Press and release both happened while loop() was blocked (SMS send, HTTP):
    // the FALLING-edge ISR latched it, so it still counts as a tap.
    if (!b.down && !raw && b.isrHit) {
        unsigned long at = b.isrAt;
        b.isrHit = false;
        if ((long)(at - b.upAt) > 150) return BTN_TAP;
    }
    return BTN_NONE;
}

static bool inHoldHint(const Button& b, unsigned long now) {
    return b.down && !b.longDone && now - b.downAt >= BTN_HOLD_HINT_MS;
}

bool buttonHoldActive() {
    unsigned long now = millis();
    return inHoldHint(btnMenu, now) || inHoldHint(btnScroll, now);
}

static void drawHoldBar(const Button& b, const char* title, unsigned long now) {
    static unsigned long lastDraw = 0;
    if (!inHoldHint(b, now) || now - lastDraw < 100) return;
    lastDraw = now;
    unsigned long span = BTN_LONG_MS - BTN_HOLD_HINT_MS;
    int filled = (int)(((now - b.downAt - BTN_HOLD_HINT_MS) * 16UL) / span);
    if (filled > 16) filled = 16;
    char bar[17];
    for (int i = 0; i < 16; i++) bar[i] = (i < filled) ? '\x04' : '-';
    bar[16] = '\0';
    updateLCD(title, bar);
}

static void showLcdMsg(const char* l1, const char* l2, unsigned long ms) {
    updateLCD(l1, l2);
    lcdHoldUntil = millis() + ms;
}

static void showScreen(UiScreen sc) {
    unsigned long now = millis();
    state.uiScreen = sc;
    state.uiPaused = true;
    state.uiPauseUntil = now + UI_PAUSE_MS;
    state.lastUiMs = now;
    lcdHoldUntil = 0;
    renderStatusScreen(sc);
}

static void cycleLimitPreset() {
    static const int PRESETS[] = { 30, 40, 50, 60, 70, 80, 100, 120 };
    const int n = sizeof(PRESETS) / sizeof(PRESETS[0]);
    if (cfg.autoZones) {
        setSpeedLimit(PRESETS[0], false);
    } else {
        int next = -1;
        for (int i = 0; i < n; i++)
            if (PRESETS[i] > cfg.defaultSpeedLimit) { next = PRESETS[i]; break; }
        if (next < 0) setSpeedLimit(DEFAULT_SPEED_LIMIT, true);   // past 120 → AUTO zones
        else          setSpeedLimit(next, false);
    }
    showScreen(UI_LIMIT);
}

void handleButtons() {
    unsigned long now = millis();
    BtnEvent m = pollButton(btnMenu, now);
    BtnEvent s = pollButton(btnScroll, now);
    bool prov = state.mode == MODE_PROVISIONING;

    drawHoldBar(btnMenu,   "Hold: WiFi setup", now);
    drawHoldBar(btnScroll, "Hold: GSM reset",  now);

    // ── MENU ─────────────────────────────────────────────────
    if (m == BTN_TAP) {
        if (state.violationTier > 0 && now >= state.buzzerMuteUntilMs) {
            state.buzzerMuteUntilMs = now + 60000;
            silenceAlert();
            showLcdMsg("Alarm muted", "for 60 seconds", 2000);
        } else if (prov) {
            state.provPage++;
            state.lastUiMs = 0;
            renderUi(true);
        } else {
            showScreen((UiScreen)((state.uiScreen + 1) % UI_COUNT));
        }
    } else if (m == BTN_HOLD) {
        enterProvisioning("MENU hold");
        updateLCD("WiFi Setup...", "Starting AP");
        startProvisioningAP();
        state.provPage = 0;
        state.lastUiMs = 0;
        renderUi(true);
    } else if (m == BTN_CANCEL) {
        if (prov) renderUi(true); else showScreen(state.uiScreen);
    }

    // ── SCROLL ───────────────────────────────────────────────
    if (s == BTN_TAP) {
        if (prov) {
            state.provPage = 2;          // password / IP page
            state.lastUiMs = 0;
            renderUi(true);
        } else if (state.uiScreen == UI_LIMIT) {
            cycleLimitPreset();
        } else {
            showScreen((UiScreen)((state.uiScreen + UI_COUNT - 1) % UI_COUNT));
        }
    } else if (s == BTN_HOLD) {
        updateLCD("Re-init GSM...", "please wait");
        state.gsmReady = initGSM();
        showLcdMsg("GSM modem:", state.gsmReady ? "Ready \x06" : "FAILED", 2500);
    } else if (s == BTN_CANCEL) {
        if (prov) renderUi(true); else showScreen(state.uiScreen);
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
    // AP+STA so nearby SSIDs can be scanned during setup
    WiFi.mode(WIFI_AP_STA);
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
        setupOTA();
        if (state.internetOk) {
            syncGeofences();
            sendHeartbeat();
            flushPostQueue();
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
        setupOTA();
        if (state.internetOk) {
            syncGeofences();
            sendHeartbeat();
            flushPostQueue();
        }
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
    if (prefs.isKey("apiKey")) {
        strncpy(cfg.apiKey, prefs.getString("apiKey").c_str(), 47);
        cfg.apiKey[47] = '\0';
    }
    if (prefs.isKey("sleepEn"))
        state.sleepEnabled = prefs.getBool("sleepEn");

    if (prefs.isKey("defLimit"))    cfg.defaultSpeedLimit = prefs.getInt("defLimit");
    if (prefs.isKey("autoZones"))   cfg.autoZones         = prefs.getBool("autoZones");
    if (prefs.isKey("limitRev"))    cfg.limitRev          = prefs.getUInt("limitRev");
    if (prefs.isKey("thrMinor"))    cfg.threshMinor       = prefs.getInt("thrMinor");
    if (prefs.isKey("thrModerate")) cfg.threshModerate    = prefs.getInt("thrModerate");
    if (prefs.isKey("thrSevere"))   cfg.threshSevere      = prefs.getInt("thrSevere");
    if (prefs.isKey("numPhones"))   cfg.numPhones         = prefs.getInt("numPhones");

    if (cfg.numPhones <= 0 || cfg.numPhones > MAX_PHONE_NUMBERS)
        cfg.numPhones = N_DEFAULT_NUMBERS;

    // Hash of DEFAULT_NUMBERS: changing the list in code re-seeds NVS once.
    uint32_t defHash = 2166136261u;
    for (int i = 0; i < N_DEFAULT_NUMBERS; i++) {
        for (const char* p = DEFAULT_NUMBERS[i]; *p; ++p) {
            defHash ^= (uint8_t)*p;
            defHash *= 16777619u;
        }
        defHash ^= ',';
        defHash *= 16777619u;
    }
    bool defaultsChanged = prefs.getUInt("phonesDefH", 0) != defHash;

    if (!defaultsChanged && prefs.isKey("phonesStored") && prefs.getBool("phonesStored")) {
        for (int i = 0; i < cfg.numPhones; i++) {
            String key = "phone" + String(i);
            if (prefs.isKey(key.c_str()))
                strncpy(cfg.phones[i], prefs.getString(key.c_str()).c_str(), 19);
        }
    } else {
        for (int i = 0; i < N_DEFAULT_NUMBERS; i++) {
            strncpy(cfg.phones[i], DEFAULT_NUMBERS[i], 19);
            cfg.phones[i][19] = '\0';
        }
        cfg.numPhones = N_DEFAULT_NUMBERS;
        prefs.end();
        prefs.begin("gps-monitor", false);
        prefs.putUInt("phonesDefH", defHash);
        prefs.end();
        saveConfig();
        Serial.println(F("[NVS]  Phone defaults changed in firmware — re-seeded recipients"));
        prefs.begin("gps-monitor", true);
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
    prefs.putString("apiKey",    cfg.apiKey);
    prefs.putBool("sleepEn",     state.sleepEnabled);
    prefs.putInt("defLimit",     cfg.defaultSpeedLimit);
    prefs.putBool("autoZones",   cfg.autoZones);
    prefs.putUInt("limitRev",    cfg.limitRev);
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
.wrap{max-width:1320px;width:100%;margin:0 auto;padding:24px 20px 48px;box-sizing:border-box}
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

/* Dashboard Top Grid */
.dash-grid{display:grid;grid-template-columns:1.2fr 1fr;gap:16px;margin-bottom:16px;width:100%}
.hero{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:20px 22px;box-shadow:var(--sh);margin-bottom:14px;width:100%}
.eyebrow{font:500 .68rem monospace;letter-spacing:.12em;text-transform:uppercase;color:var(--mut);margin-bottom:8px}
.hero h2{font-size:1.35rem;margin:0;transition:color .3s ease}.hero h2.ok{color:var(--td)}.hero h2.bad{color:var(--rose)}
.hero .sub{color:var(--soft);font-size:.9rem;margin-top:6px;line-height:1.45}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}
.chip{font:500 .7rem monospace;background:#eef5f3;color:var(--td);border:1px solid #cce8e2;border-radius:8px;padding:5px 9px}
.chip.warn{background:#fff4e5;color:#9a5b05;border-color:#f5d7a6}
.chip.err{background:#fde8ec;color:var(--rose);border-color:#f5c2cd}

.gauge-card{text-align:center;padding:18px 16px;background:var(--card);border:1px solid var(--line);border-radius:var(--r);box-shadow:var(--sh);width:100%}
.gauge-box{position:relative;width:220px;height:130px;margin:8px auto}
.gauge-svg{width:100%;height:100%}
.gauge-bg{fill:none;stroke:#e2e8f0;stroke-width:14;stroke-linecap:round}
.gauge-val{fill:none;stroke:var(--teal);stroke-width:14;stroke-linecap:round;stroke-dasharray:283;stroke-dashoffset:283;transition:all .4s cubic-bezier(0.4,0,0.2,1)}
.gauge-center{position:absolute;bottom:6px;left:0;right:0}
.gauge-val-text{font:700 2.4rem monospace;line-height:1}
.gauge-unit{font-size:.7rem;color:var(--mut);text-transform:uppercase;letter-spacing:.08em}

/* KPI Summaries Row: Full width, evenly distributed cards with zero awkward orphans */
.kpis{display:grid;grid-template-columns:repeat(6,1fr);gap:12px;margin-bottom:16px;width:100%}
.kpi{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:14px 16px;box-shadow:var(--sh);width:100%;min-width:0}
.kpi .l{font-size:.68rem;color:var(--mut);text-transform:uppercase;letter-spacing:.06em}
.kpi .v{font:600 1.65rem monospace;margin-top:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.kpi .u{font-size:.7rem;color:var(--mut)}
.safe{color:var(--td)}.warn{color:var(--amber)}.danger{color:var(--rose)}

/* Full width panels and tables */
.panel{background:var(--card);border:1px solid var(--line);border-radius:var(--r);box-shadow:var(--sh);margin-bottom:16px;width:100%;overflow:hidden}
.phd{padding:14px 18px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:center}
.phd h3{font-size:.95rem;margin:0}.pbd{padding:16px 18px}
.fs{margin:0 0 14px;padding:16px;background:var(--card);border:1px solid var(--line);border-radius:var(--r);box-shadow:var(--sh);width:100%}
.fs.hi{border:2px solid var(--teal);background:#f3fbf9}
.fs h2{font-size:1rem;margin:0 0 4px}.fs .lead{color:var(--mut);font-size:.8rem;margin:0 0 12px;line-height:1.4}
label{display:block;font-size:.8rem;color:var(--soft);margin:14px 0 6px;font-weight:600}
input[type=text],input[type=password],input[type=number],input[type=tel],select,textarea{
display:block;width:100%;min-height:48px;background:#fff;border:2px solid #9eb6c8;border-radius:10px;
padding:12px 14px;color:var(--ink);font:inherit;font-size:1rem;-webkit-appearance:none;appearance:none}
input[type=text]:focus,input[type=password]:focus,input[type=number]:focus,input[type=tel]:focus,select:focus,textarea:focus{
outline:none;border-color:var(--teal);box-shadow:0 0 0 3px rgba(15,157,138,.2)}
input[type=checkbox]{display:inline-block;width:18px;height:18px;min-height:0;margin:0 8px 0 0;vertical-align:middle;
-webkit-appearance:checkbox;appearance:auto;accent-color:var(--teal)}
label.chk{display:flex;align-items:flex-start;gap:8px;font-weight:500;line-height:1.35;margin-top:14px}
label.chk span{flex:1}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
.hint{font-size:.72rem;color:var(--mut);margin-top:6px;line-height:1.35}
.btn{display:inline-block;padding:12px 18px;border-radius:10px;border:none;font:600 .9rem system-ui,sans-serif;cursor:pointer;text-decoration:none;margin:10px 8px 0 0}
.bp{background:var(--teal);color:#fff}.bd{background:var(--rose);color:#fff}
.bg{background:var(--card);color:var(--ink);border:1px solid var(--line)}
.pi{display:flex;gap:8px;align-items:center;margin-bottom:8px}.pi input{flex:1;min-width:0}
.rm,.add{border:none;border-radius:8px;padding:9px 12px;cursor:pointer;font-size:.8rem;font-weight:600;flex-shrink:0}
.rm{background:#fde8ec;color:var(--rose)}.add{background:#e6f4fc;color:#0369a1;margin-top:4px}
.mok{padding:11px 14px;border-radius:10px;margin-bottom:12px;font-size:.84rem;background:#e8f7f3;border:1px solid #9ad9ce;color:var(--td)}
.merr{padding:11px 14px;border-radius:10px;margin-bottom:12px;font-size:.84rem;background:#fde8ec;border:1px solid #f5c2cd;color:var(--rose)}
.table-wrap{width:100%;overflow-x:auto;-webkit-overflow-scrolling:touch}
table{width:100%;min-width:100%;border-collapse:collapse;font-size:.82rem}
th{text-align:left;padding:10px 14px;color:var(--mut);font-size:.7rem;text-transform:uppercase;letter-spacing:.05em;border-bottom:1px solid var(--line);background:#f7fafc}
td{padding:12px 14px;border-bottom:1px solid #eef2f6;word-break:break-word}
tr:last-child td{border-bottom:none}
tr:hover td{background:#fafbfc}
.tier-tag{display:inline-block;padding:3px 8px;border-radius:6px;font:700 .7rem monospace;letter-spacing:.04em}
.tier-SEVERE{background:#fde8ec;color:var(--rose);border:1px solid #f5c2cd}
.tier-MODERATE{background:#fef3c7;color:var(--amber);border:1px solid #fde68a}
.tier-MINOR{background:#e0f2fe;color:var(--sky);border:1px solid #bae6fd}
.tier-SAFE{background:#e8f7f3;color:var(--td);border:1px solid #9ad9ce}
.badge{display:inline-block;padding:3px 9px;border-radius:999px;font-size:.7rem;background:#eef5f3;border:1px solid #cce8e2;color:var(--td);margin:0 4px 4px 0}
.foot{margin-top:18px;color:var(--mut);font-size:.72rem;display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap}
.foot a{color:var(--td)}
.onb{max-width:480px;margin:12px auto;width:100%}
.ipbox{font:600 .95rem monospace;color:var(--td);background:#e8f7f3;border:1px solid #9ad9ce;border-radius:12px;padding:14px;text-align:center;margin:14px 0;word-break:break-all}
.steps{margin:12px 0;padding-left:18px;color:var(--soft);font-size:.84rem;line-height:1.55}
.mono{font-family:monospace}.empty{color:var(--mut);padding:24px;text-align:center;font-size:.88rem}
.qr-wrap{overflow:auto;max-width:100%}
.pinmap{font:500 .78rem monospace;background:#f7fafc;border:1px solid var(--line);border-radius:10px;padding:12px;line-height:1.55;margin-top:8px}
.tap{cursor:pointer;transition:transform .12s ease,box-shadow .12s ease,border-color .12s ease}
.tap:hover,.tap.on{border-color:var(--teal);box-shadow:0 0 0 2px rgba(15,157,138,.18);transform:translateY(-1px)}
.tap.on{background:#d9f3ee}
.statusline{min-height:1.2em;font-size:.82rem;color:var(--soft);margin-top:10px}
.statusline.busy{color:#0369a1}.statusline.ok{color:var(--td)}.statusline.err{color:var(--rose)}
.btn:disabled{opacity:.55;cursor:wait}
button,.btn{transition:transform .08s ease,filter .08s ease,box-shadow .08s ease;-webkit-tap-highlight-color:rgba(15,157,138,.25);touch-action:manipulation}
button:active,.btn:active{transform:scale(.95);filter:brightness(.88)}
.btn.busy{position:relative;pointer-events:none;opacity:.75}
.btn.busy:after{content:'';display:inline-block;width:12px;height:12px;margin-left:8px;vertical-align:-2px;border:2px solid currentColor;border-right-color:transparent;border-radius:50%;animation:spin .7s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
.flash{animation:flash .5s ease}
@keyframes flash{0%{box-shadow:0 0 0 0 rgba(15,157,138,.55)}100%{box-shadow:0 0 0 10px rgba(15,157,138,0)}}
.target{font-size:.82rem;color:var(--soft);margin-top:8px}.target b{color:var(--td)}
.chkline{display:flex;align-items:baseline;gap:6px}
.chkline .big{font:700 2.3rem monospace;line-height:1}.chkline .sep{font-size:1.6rem;color:var(--mut)}
.lbar{height:10px;background:#eef2f6;border-radius:99px;overflow:hidden;margin:10px 0 2px}
.lbar div{height:100%;width:0;background:var(--teal);transition:width .4s ease,background .3s}
.limset{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.limset input[type=number]{width:110px;flex:0 0 110px;text-align:center;font:600 1.15rem monospace}
.limset .btn{margin:0}
.step{border:1px solid var(--line);background:#f7fafc;border-radius:10px;min-width:52px;min-height:48px;font:600 1rem monospace;cursor:pointer;color:var(--ink)}
.cnt{font:500 .72rem monospace;color:var(--mut);float:right}
.quick{display:flex;flex-wrap:wrap;gap:6px;margin:8px 0 4px}
.quick button{border:1px solid var(--line);background:#f7fafc;border-radius:8px;padding:7px 10px;font-size:.75rem;cursor:pointer;color:var(--soft)}
.quick button:hover,.quick button.on{border-color:var(--teal);color:var(--td);background:#d9f3ee}
.bench{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
.hud-mode{background:#0b1320;color:#fff}
.hud-mode .top,.hud-mode .panel,.hud-mode .foot,.hud-mode .fs,.hud-mode .dash-col.side-cockpit,.hud-mode .kpis{display:none!important}
.hud-mode .dash-grid{grid-template-columns:1fr}
.hud-mode .hero{background:transparent;border:none;box-shadow:none;text-align:center;padding:50px 10px}
.hud-mode .hero h2{font-size:3.8rem;color:#00e5ff}
@media(max-width:960px){
.dash-grid{grid-template-columns:1fr}
.kpis{grid-template-columns:repeat(3,1fr)}
}
@media(max-width:560px){
.wrap{padding:14px 12px 32px}
.brand h1{font-size:1.2rem}
.kpis{grid-template-columns:repeat(2,1fr)}
.kpi .v{font-size:1.35rem}
.nav a{padding:7px 11px;font-size:.75rem}
}
</style>
)CSS";


static const char DASHBOARD_JS[] PROGMEM = R"JS(
<script>
var lastViolTotal = -1;
function updateLive(){
  fetch('/api/status').then(function(r){return r.json();}).then(function(d){
    var spd = Math.round(d.speed);
    var lim = Math.round(d.speed_limit);
    var tier = d.violation_tier || 0;
    var viols = d.total_violations || 0;
    var peak = Math.round(d.max_speed || 0);
    var sats = d.satellites || 0;
    var gpsOk = !!d.gps_valid;

    var sEl = document.getElementById('liveSpeedNum'); if(sEl) sEl.textContent = spd;
    var arc = document.getElementById('liveArc');
    if(arc){
      var maxScale = Math.max(120, lim * 1.5);
      var pct = Math.min(1, Math.max(0, spd / maxScale));
      arc.style.strokeDashoffset = 283 - (pct * 283);
      arc.style.stroke = (tier >= 3) ? 'var(--rose)' : (tier >= 1 ? 'var(--amber)' : 'var(--teal)');
    }

    var hTitle = document.getElementById('heroTitle');
    if(hTitle){
      if(!gpsOk){ hTitle.textContent = 'Acquiring GPS fix'; hTitle.className = 'bad'; }
      else if(tier > 0){ hTitle.textContent = 'Speed limit exceeded'; hTitle.className = 'bad'; }
      else { hTitle.textContent = 'Speed compliant'; hTitle.className = 'ok'; }
    }

    var cMode = document.getElementById('chipMode'); if(cMode) cMode.textContent = d.mode || 'NORMAL';
    var cNet = document.getElementById('chipNet');
    if(cNet){
      cNet.textContent = d.wifi_connected ? (d.internet_ok ? 'Internet OK' : 'Wi-Fi · no net') : (d.ap_active ? 'Setup AP' : 'Offline');
      cNet.className = 'chip' + (d.internet_ok ? '' : ' warn');
    }
    var cIp = document.getElementById('chipIp'); if(cIp) cIp.textContent = d.ip || '';

    var ks = document.getElementById('kpiSpd');
    if(ks){
      ks.textContent = spd;
      ks.className = 'v ' + (tier === 0 ? 'safe' : (tier <= 2 ? 'warn' : 'danger'));
    }
    var kl = document.getElementById('kpiLim'); if(kl) kl.textContent = lim;
    var kt = document.getElementById('kpiTier');
    if(kt){
      kt.textContent = tier === 0 ? 'SAFE' : (tier === 1 ? 'MINOR' : (tier === 2 ? 'MODERATE' : 'SEVERE'));
      kt.className = 'v ' + (tier === 0 ? 'safe' : (tier <= 2 ? 'warn' : 'danger'));
    }
    var kv = document.getElementById('kpiViols'); if(kv) kv.textContent = viols;
    var kgps = document.getElementById('kpiGps'); if(kgps) kgps.textContent = gpsOk ? 'LOCK' : '…';
    var ksats = document.getElementById('kpiSats'); if(ksats) ksats.textContent = sats + ' sats';
    var kp = document.getElementById('kpiPeak'); if(kp) kp.textContent = peak;

    var sysWifi = document.getElementById('sysWifiDot');
    if(sysWifi) sysWifi.className = 'dot ' + (d.wifi_connected ? 'dok' : (d.ap_active ? 'dwrn' : 'derr'));
    var sysGsm = document.getElementById('sysGsmDot');
    if(sysGsm) sysGsm.className = 'dot ' + (d.gsm_ready ? 'dok' : 'derr');
    var sysGps = document.getElementById('sysGpsDot');
    if(sysGps) sysGps.className = 'dot ' + (gpsOk ? 'dok' : 'dwrn');
    var sysIp = document.getElementById('sysIp'); if(sysIp) sysIp.textContent = 'IP ' + (d.ip || '0.0.0.0');
    var sysHeap = document.getElementById('sysHeap'); if(sysHeap) sysHeap.textContent = 'Heap ' + (d.free_heap || 0) + ' B';
    updateLimit(d);
    updateLoc(d);

    if(viols !== lastViolTotal){
      lastViolTotal = viols;
      updateViolations();
    }
  }).catch(function(){});
}

function updateViolations(){
  fetch('/api/violations').then(function(r){return r.json();}).then(function(res){
    var box = document.getElementById('violationsBox');
    if(!box) return;
    var list = res.violations || [];
    if(list.length === 0){
      box.innerHTML = '<div class="empty">No violations this session.</div>';
      return;
    }
    var h = '<div class="table-wrap"><table><thead><tr>' +
      '<th>#</th><th>Tier</th><th>Speed</th><th>Limit</th><th>Excess</th><th>Location</th>' +
      '</tr></thead><tbody>';
    for(var i = list.length - 1; i >= 0; i--){
      var r = list[i];
      var tier = r.tier || 'UNKNOWN';
      var excess = Math.round(r.excess || (r.speed - r.limit));
      h += '<tr><td class="mono">' + (list.length - i) + '</td>' +
        '<td><span class="tier-tag tier-' + tier + '">' + tier + '</span></td>' +
        '<td class="mono"><b>' + Math.round(r.speed) + '</b> <span class="u">km/h</span></td>' +
        '<td class="mono">' + Math.round(r.limit) + '</td>' +
        '<td class="mono" style="color:var(--rose)">+' + excess + '</td>' +
        '<td class="mono" style="font-size:.75rem">' + (r.lat ? '<a href="' + gmUrl(r.lat, r.lon) + '" target=_blank rel=noopener>' + r.lat.toFixed(4) + ', ' + r.lon.toFixed(4) + ' &#8599;</a>' : 'No GPS') + '</td></tr>';
    }
    h += '</tbody></table></div>';
    box.innerHTML = h;
  }).catch(function(){});
}

function gmUrl(lat, lon){ return 'https://www.google.com/maps/search/?api=1&query=' + (+lat).toFixed(6) + ',' + (+lon).toFixed(6); }
function updateLoc(d){
  var ok = d.lat !== undefined && d.lon !== undefined;
  var c = gid('locCoords'), a = gid('locGmaps'), f = gid('locFix'), t = gid('trkInfo');
  if(c) c.textContent = ok ? (+d.lat).toFixed(6) + ', ' + (+d.lon).toFixed(6) : 'Waiting for GPS fix';
  if(a){ a.style.display = ok ? '' : 'none'; if(ok) a.href = gmUrl(d.lat, d.lon); }
  if(f) f.textContent = !ok ? 'No fix' : (d.gps_valid ? 'Live fix' : 'Last known');
  if(t) t.textContent = 'GPS log: ' + (d.track_points || 0) + ' in memory · ' + (d.track_uploaded || 0) +
    ' uploaded · ' + (d.track_unsent || 0) + ' waiting' + (d.internet_ok ? '' : ' (no internet)');
}
function updateTrack(){
  fetch('/api/track').then(function(r){return r.json();}).then(function(res){
    var p = res.points || [], a = gid('trkGmaps');
    if(!a) return;
    if(p.length < 2){ a.style.display = 'none'; return; }
    var n = Math.min(10, p.length), parts = [];
    for(var i = 0; i < n; i++){
      var q = p[Math.round(i * (p.length - 1) / (n - 1))];
      parts.push(q.lat.toFixed(6) + ',' + q.lon.toFixed(6));
    }
    a.href = 'https://www.google.com/maps/dir/' + parts.join('/');
    a.style.display = '';
  }).catch(function(){});
}

setInterval(updateLive, 1000);
setInterval(updateTrack, 15000);
updateLive();
updateViolations();
updateTrack();

function muteBuzzer(){
  fetch('/api/mute',{method:'POST'}).then(function(r){return r.json();}).then(function(){alert('Buzzer muted for 60s');}).catch(function(){});
}
function toggleHUD(){
  document.body.classList.toggle('hud-mode');
}

var limDirty = false;
function gid(i){ return document.getElementById(i); }
function limStatus(t, c){ var e = gid('limStatus'); if(e){ e.className = 'statusline ' + (c || ''); e.textContent = t || ''; } }
function markPreset(v, mode){
  var bs = document.querySelectorAll('#limPresets button');
  for(var i = 0; i < bs.length; i++)
    bs[i].classList.toggle('on', mode !== 'auto' && parseInt(bs[i].getAttribute('data-v'), 10) === v);
}
function updateLimit(d){
  var spd = Math.round(d.speed || 0), lim = Math.round(d.speed_limit || 0), gpsOk = !!d.gps_valid;
  var s = gid('chkSpd'), l = gid('chkLim'), bar = gid('chkBar'), st = gid('chkState'), pill = gid('limModePill');
  if(!s) return;
  s.textContent = spd; l.textContent = lim;
  var pct = lim > 0 ? Math.min(100, spd / lim * 100) : 0;
  bar.style.width = pct + '%';
  bar.style.background = spd > lim ? 'var(--rose)' : (pct > 85 ? 'var(--amber)' : 'var(--teal)');
  if(!gpsOk){ st.className = 'statusline'; st.textContent = 'Waiting for GPS fix — the check starts when GPS locks'; }
  else if(spd > lim){ st.className = 'statusline err'; st.textContent = 'OVER the limit by ' + (spd - lim) + ' km/h' + (d.alarm_muted ? ' (alarm muted)' : ''); }
  else { st.className = 'statusline ok'; st.textContent = 'Within limit — ' + (lim - spd) + ' km/h below'; }
  pill.textContent = d.limit_mode === 'auto' ? 'AUTO zones' : 'MANUAL';
  if(!limDirty){
    gid('limIn').value = d.limit_setting;
    gid('limAuto').checked = d.limit_mode === 'auto';
    markPreset(d.limit_setting, d.limit_mode);
  }
}
function stepLim(delta){
  var inp = gid('limIn'), v = parseInt(inp.value, 10) || 50;
  v = Math.max(5, Math.min(250, v + delta));
  inp.value = v; limDirty = true; markPreset(v, 'manual');
  limStatus('Tap "Set limit" to apply ' + v + ' km/h', '');
}
function presetLim(b){
  gid('limIn').value = b.getAttribute('data-v');
  gid('limAuto').checked = false;
  saveLim(gid('limSave'));
}
function saveLim(btn){
  var v = parseInt(gid('limIn').value, 10), auto = gid('limAuto').checked;
  if(!(v >= 5 && v <= 250)){ limStatus('Enter a limit between 5 and 250 km/h', 'err'); return; }
  if(btn){ btn.classList.add('busy'); btn.disabled = true; }
  limStatus('Saving…', 'busy');
  fetch('/api/limit', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'limit=' + v + '&mode=' + (auto ? 'auto' : 'manual')})
  .then(function(r){ return r.json(); }).then(function(d){
    if(btn){ btn.classList.remove('busy'); btn.disabled = false; }
    if(!d.ok){ limStatus(d.detail || 'Could not save', 'err'); return; }
    limDirty = false;
    gid('chkLim').textContent = Math.round(d.speed_limit);
    gid('kpiLim') && (gid('kpiLim').textContent = Math.round(d.speed_limit));
    markPreset(d.limit_setting, d.limit_mode);
    limStatus(d.limit_mode === 'auto'
      ? 'AUTO: map zone limits apply (' + d.limit_setting + ' km/h outside zones)'
      : 'Speed limit set to ' + d.limit_setting + ' km/h', 'ok');
  }).catch(function(){
    if(btn){ btn.classList.remove('busy'); btn.disabled = false; }
    limStatus('No response from device', 'err');
  });
}
</script>
)JS";

static void htmlHead(const char* title, bool autoRefresh = false) {
    (void)autoRefresh; // Never refresh full page - dynamic values refresh in-place
    webServer.sendContent(F("<!DOCTYPE html><html lang=en><head>"
        "<meta charset=UTF-8>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>"));
    webServer.sendContent(title);
    webServer.sendContent(F("</title>"));
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

    webServer.sendContent(F(
        "<a href=/settings"));
    if (strcmp(active, "settings") == 0) webServer.sendContent(F(" class=on"));
    webServer.sendContent(F(">Settings</a>"));

    webServer.sendContent(F("<a href=/sms"));
    if (strcmp(active, "sms") == 0) webServer.sendContent(F(" class=on"));
    webServer.sendContent(F(">SMS Test</a></nav></div>"));
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
    // One chunk per call: per-character chunks overflow the TCP buffer and truncate pages.
    String out;
    out.reserve(strlen(s) + 16);
    for (; *s; ++s) {
        if (*s == '&')       out += F("&amp;");
        else if (*s == '\'') out += F("&#39;");
        else if (*s == '"')  out += F("&quot;");
        else if (*s == '<')  out += F("&lt;");
        else                 out += *s;
    }
    if (out.length()) webServer.sendContent(out);
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
        "<p class=lead>Type your home/office network below, then tap Save &amp; Connect.</p>"));

    {
        char url[48];
        IPAddress ip = state.apActive ? WiFi.softAPIP() : currentDeviceIP();
        snprintf(url, sizeof(url), "http://%s/wifi", ip.toString().c_str());
        sendSetupQrHtml(url);
    }

    webServer.sendContent(F(
        "<form method=POST action=/wifi>"
        "<label for=wifiSSID>Wi-Fi name (SSID)</label>"
        "<input id=wifiSSID name=wifiSSID type=text required maxlength=63 "
        "placeholder=\"Your network name\" value=\""));
    sendHtmlAttr(cfg.wifiSSID);
    webServer.sendContent(F("\" autocomplete=off autocapitalize=none spellcheck=false>"
        "<label for=wifiPass>Wi-Fi password</label>"
        "<input id=wifiPass name=wifiPass type=password maxlength=63 "
        "placeholder=\"Your network password\" value=\""));
    sendHtmlAttr(cfg.wifiPass);
    webServer.sendContent(F("\" autocomplete=off autocapitalize=none spellcheck=false>"
        "<label class=chk><input type=checkbox onchange=\"document.getElementById('wifiPass').type=this.checked?'text':'password'\"> <span>Show password</span></label>"
        "<p class=hint>Not the hotspot password. Leave blank only for open networks.</p>"
        "<button class=\"btn bp\" type=submit>Save &amp; Connect</button>"
        "</form>"
        "<p class=lead style=\"margin-top:12px\">Nearby networks</p>"
        "<div id=scanBox class=hint>Scanning…</div>"
        "<button type=button class=\"btn bg\" onclick=doScan()>Refresh scan</button>"
        "</div>"));

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
            "<li>Pick a network above or type your router Wi-Fi</li>"
            "<li>Tap Save &amp; Connect</li>"
            "</ol>"
            "<p class=hint style=\"text-align:center\"><a href=/settings>All settings</a></p>"
            "</div>",
            ip.toString().c_str(), AP_SSID, AP_PASS);
        webServer.sendContent(buf);
    }

    webServer.sendContent(F(
        "<script>"
        "function pick(s){document.getElementById('wifiSSID').value=s;"
        "document.getElementById('wifiPass').focus();}"
        "function doScan(){var b=document.getElementById('scanBox');"
        "b.textContent='Scanning…';"
        "fetch('/api/scan').then(r=>r.json()).then(d=>{"
        "if(!d.networks||!d.networks.length){b.textContent='No networks found';return;}"
        "b.innerHTML=d.networks.map(n=>{"
        "var b=n.rssi>-60?'●●●●':(n.rssi>-70?'●●●○':(n.rssi>-80?'●●○○':'●○○○'));"
        "return '<button type=button class=\"btn bg\" style=\"display:flex;justify-content:space-between;width:100%;margin:4px 0;text-align:left\" '+"
        "'onclick=\"pick(\\''+n.ssid.replace(/'/g,\"\\\\'\")+'\")\"><span>'+n.ssid+'</span><span style=\"color:#0f9d8a\">'+b+' ('+n.rssi+' dBm)</span></button>';"
        "}).join('');"
        "}).catch(()=>{b.textContent='Scan failed';});}"
        "doScan();"
        "</script>"));

    htmlFoot();
    webServer.sendContent("");
}

void sendWifiScanJSON() {
    int n = WiFi.scanNetworks(false, true);
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", "");
    webServer.sendContent(F("{\"ok\":true,\"networks\":["));
    int sent = 0;
    for (int i = 0; i < n && sent < 20; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        char buf[160];
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        snprintf(buf, sizeof(buf), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%d}",
                 sent ? "," : "", ssid.c_str(), WiFi.RSSI(i), (int)WiFi.encryptionType(i));
        webServer.sendContent(buf);
        sent++;
    }
    webServer.sendContent(F("]}"));
    webServer.sendContent("");
    WiFi.scanDelete();
}

void sendSetupQrHtml(const char* url) {
    QRCode qrcode;
    uint8_t qrBuf[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrBuf, 3, ECC_LOW, url);
    webServer.sendContent(F(
        "<div style=\"text-align:center;margin:12px 0 16px\">"
        "<div class=eyebrow>Scan with phone camera</div>"
        "<div class=qr-wrap><table cellspacing=0 cellpadding=0 style=\"margin:10px auto;border-collapse:collapse;"
        "background:#fff;padding:8px;border:1px solid #d5dee8;border-radius:8px\">"));
    for (uint8_t y = 0; y < qrcode.size; y++) {
        webServer.sendContent(F("<tr>"));
        for (uint8_t x = 0; x < qrcode.size; x++) {
            if (qrcode_getModule(&qrcode, x, y))
                webServer.sendContent(F("<td style=\"width:5px;height:5px;background:#0b1f33\"></td>"));
            else
                webServer.sendContent(F("<td style=\"width:5px;height:5px;background:#fff\"></td>"));
        }
        webServer.sendContent(F("</tr>"));
    }
    webServer.sendContent(F("</table></div><p class=hint mono>"));
    webServer.sendContent(url);
    webServer.sendContent(F("</p></div>"));
}

// ── Dashboard ─────────────────────────────────────────────────
void sendDashboard() {
    if (state.mode == MODE_PROVISIONING) {
        sendWifiSetup();
        return;
    }
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/html"), "");

    htmlHead("Velocis — Live", false);
    htmlTop("live");

    webServer.sendContent(F("<div class=dash-grid><div class='dash-col main-cockpit'>"));

    // Status hero
    {
        const bool alert = state.violationTier > 0;
        const char* title = !state.gpsValid ? "Acquiring GPS fix"
                          : alert ? "Speed limit exceeded"
                          : "Speed compliant";
        webServer.sendContent(F("<div class='hero'><div class='eyebrow'>Live monitor</div><h2 id='heroTitle' class='"));
        webServer.sendContent(alert ? "bad" : "ok");
        webServer.sendContent(F("'>"));
        webServer.sendContent(title);
        webServer.sendContent(F("</h2><p class='sub' id='heroSub'>"));
        webServer.sendContent(cfg.deviceID);
        webServer.sendContent(F(" · session desk</p><div class='chips'>"));

        char chip[256];
        snprintf(chip, sizeof(chip),
            "<span class='chip' id='chipMode'>%s</span>"
            "<span class='chip%s' id='chipNet'>%s</span>"
            "<span class='chip' id='chipIp'>%s</span>",
            modeLabel(),
            state.internetOk ? "" : " warn",
            state.wifiConnected ? (state.internetOk ? "Internet OK" : "Wi-Fi · no net")
                                : (state.apActive ? "Setup AP" : "Offline"),
            currentDeviceIP().toString().c_str());
        webServer.sendContent(chip);
        webServer.sendContent(F("</div></div>"));
    }

    // Live Gauge Dial Card
    webServer.sendContent(F(
        "<div class='gauge-card'>"
        "<div class='eyebrow'>Live Telemetry Speed</div>"
        "<div class='gauge-box'>"
        "<svg class='gauge-svg' viewBox='0 0 200 120'>"
        "<path class='gauge-bg' d='M 20 105 A 80 80 0 0 1 180 105'></path>"
        "<path class='gauge-val' id='liveArc' d='M 20 105 A 80 80 0 0 1 180 105'></path>"
        "</svg>"
        "<div class='gauge-center'>"
        "<div class='gauge-val-text' id='liveSpeedNum'>0</div>"
        "<div class='gauge-unit'>km / h</div>"
        "</div></div>"
        "<div class='row' style='justify-content:center;margin-top:6px'>"
        "<button type=button class='btn bp' onclick=muteBuzzer()>Mute Buzzer (60s)</button>"
        "<button type=button class='btn bg' onclick=toggleHUD()>HUD Mode</button>"
        "</div></div>"));

    // Speed limit: live check + set
    {
        char buf[1800];
        snprintf(buf, sizeof(buf),
            "<div class='panel' id=limitCard><div class='phd'><h3>Speed limit check</h3>"
            "<span class='pill' id=limModePill>%s</span></div><div class='pbd'>"
            "<div class=chkline><span class=big id=chkSpd>%d</span><span class=sep>/</span>"
            "<span class=big id=chkLim>%d</span><span class=u>km/h</span></div>"
            "<div class=lbar><div id=chkBar></div></div>"
            "<div class=statusline id=chkState>%s</div>"
            "<label for=limIn>Set speed limit (km/h)</label>"
            "<div class=limset>"
            "<button type=button class=step onclick=stepLim(-5)>&minus;5</button>"
            "<input id=limIn type=number min=5 max=250 value=%d oninput='limDirty=true'>"
            "<button type=button class=step onclick=stepLim(5)>+5</button>"
            "<button type=button class='btn bp' id=limSave onclick=saveLim(this)>Set limit</button>"
            "</div>"
            "<div class=quick id=limPresets>"
            "<button type=button data-v=30 onclick=presetLim(this)>30</button>"
            "<button type=button data-v=40 onclick=presetLim(this)>40</button>"
            "<button type=button data-v=50 onclick=presetLim(this)>50</button>"
            "<button type=button data-v=60 onclick=presetLim(this)>60</button>"
            "<button type=button data-v=80 onclick=presetLim(this)>80</button>"
            "<button type=button data-v=100 onclick=presetLim(this)>100</button>"
            "<button type=button data-v=120 onclick=presetLim(this)>120</button>"
            "</div>"
            "<label class=chk><input type=checkbox id=limAuto%s onchange=saveLim(null)>"
            "<span>AUTO: use map zone limits where available</span></label>"
            "<div class=statusline id=limStatus></div>"
            "</div></div>",
            cfg.autoZones ? "AUTO zones" : "MANUAL",
            (int)state.currentSpeed, (int)state.speedLimit,
            state.gpsValid ? "Checking…" : "Waiting for GPS fix — the check starts when GPS locks",
            cfg.defaultSpeedLimit,
            cfg.autoZones ? " checked" : "");
        webServer.sendContent(buf);
    }

    // Location + GPS log (filled by updateLoc / updateTrack)
    webServer.sendContent(F(
        "<div class='panel' id=locCard><div class='phd'><h3>Location</h3>"
        "<span class='pill' id=locFix>No fix</span></div><div class='pbd'>"
        "<div class=mono id=locCoords style='font-size:1.05rem'>Waiting for GPS fix</div>"
        "<div class=row style='margin-top:10px'>"
        "<a class='btn bp' id=locGmaps href='#' target=_blank rel=noopener style='display:none'>Open in Google Maps</a>"
        "<a class='btn bg' id=trkGmaps href='#' target=_blank rel=noopener style='display:none'>Recent route in Google Maps</a>"
        "</div>"
        "<div class=statusline id=trkInfo>GPS log: waiting for a fix</div>"
        "<p class=hint style='margin:6px 0 0'>A point is logged every 5 s while moving (60 s parked) "
        "and uploaded to the server every 15 s when online.</p>"
        "</div></div>"));
    webServer.sendContent(F("</div>"));

    // Secondary column: Diagnostics & Hardware Bench & Recipients
    webServer.sendContent(F("<div class='dash-col side-cockpit'>"));

    // System Diagnostics card
    {
        char buf[768];
        snprintf(buf, sizeof(buf),
            "<div class='panel'><div class='phd'><h3>System Diagnostics</h3>"
            "<span class='pill'><span class='dot %s' id='sysWifiDot'></span>Wi-Fi</span></div>"
            "<div class='pbd' style='display:flex;flex-wrap:wrap;gap:8px;align-items:center'>"
            "<span class='pill'><span class='dot %s' id='sysGsmDot'></span>GSM</span>"
            "<span class='pill'><span class='dot %s' id='sysGpsDot'></span>GPS</span>"
            "<span class='pill mono' id='sysIp'>IP %s</span>"
            "<span class='pill' id='sysHeap'>Heap %u B</span>"
            "<a class='btn bp' href='/wifi' style='margin:0;padding:6px 12px;font-size:.78rem'>Change Wi-Fi</a>"
            "</div></div>",
            state.wifiConnected ? "dok" : (state.apActive ? "dwrn" : "derr"),
            state.gsmReady ? "dok" : "derr",
            state.gpsValid ? "dok" : "dwrn",
            currentDeviceIP().toString().c_str(),
            ESP.getFreeHeap());
        webServer.sendContent(buf);
    }

    // Hardware Bench card
    webServer.sendContent(F(
        "<div class='panel'><div class='phd'><h3>Hardware Diagnostics</h3></div>"
        "<div class='pbd'><div class='bench'>"
        "<button type=button class='btn bg' onclick=\"fetch('/api/test/buzzer',{method:'POST'}).then(()=>alert('Buzzer pulsed!'))\">Test Buzzer</button>"
        "<button type=button class='btn bg' onclick=\"fetch('/api/test/leds',{method:'POST'}).then(()=>alert('Cycled LEDs!'))\">Cycle LEDs</button>"
        "<button type=button class='btn bg' onclick=\"fetch('/api/test/lcd',{method:'POST'}).then(()=>alert('Blinked LCD!'))\">Blink LCD</button>"
        "</div></div></div>"));

    // Physical button guide
    webServer.sendContent(F(
        "<div class='panel'><div class='phd'><h3>Device buttons</h3></div>"
        "<div class=table-wrap><table>"
        "<thead><tr><th>Button</th><th>Tap</th><th>Hold 3 s</th></tr></thead><tbody>"
        "<tr><td><b>MENU</b><br><span class=u>GPIO 18</span></td>"
        "<td>Next screen<br><span class=u>mutes alarm 60 s while it sounds</span></td>"
        "<td>Wi-Fi setup hotspot</td></tr>"
        "<tr><td><b>SCROLL</b><br><span class=u>GPIO 19</span></td>"
        "<td>Previous screen<br><span class=u>on the Limit screen: next limit 30→120→AUTO</span></td>"
        "<td>Restart GSM modem</td></tr>"
        "</tbody></table></div>"
        "<div class=pbd><p class=hint style='margin:0'>LCD screens: Speed → Limit → Wi-Fi → GPS → Stats. "
        "While you hold a button a bar fills on the LCD; let go early to cancel.</p></div></div>"));

    // SMS recipients card
    webServer.sendContent(F("<div class='panel'><div class='phd'><h3>SMS recipients</h3>"
        "<a class='btn bp' href=/sms style='margin:0;padding:6px 12px;font-size:.75rem'>Test &amp; edit</a>"
        "</div><div class='pbd'>"));
    for (int i = 0; i < cfg.numPhones; i++) {
        webServer.sendContent(F("<span class='badge'>"));
        webServer.sendContent(cfg.phones[i]);
        webServer.sendContent(F("</span>"));
    }
    if (cfg.numPhones == 0)
        webServer.sendContent(F("<p class=empty style=\"padding:8px 0;text-align:left\">None yet — open SMS Test to add numbers.</p>"));
    webServer.sendContent(F("</div></div></div></div>"));

    // Full Width KPI row (6 cards evenly distributed across 100% width)
    webServer.sendContent(F("<div class='kpis'>"));
    {
        const char* cls = (state.violationTier == 0) ? "safe"
                        : (state.violationTier <= 2) ? "warn" : "danger";
        char buf[900];
        snprintf(buf, sizeof(buf),
            "<div class='kpi'><div class='l'>Speed</div><div class='v %s' id='kpiSpd'>%d</div><div class='u'>km/h</div></div>"
            "<div class='kpi'><div class='l'>Limit</div><div class='v' id='kpiLim'>%d</div><div class='u'>km/h</div></div>"
            "<div class='kpi'><div class='l'>Alert</div><div class='v %s' id='kpiTier'>%s</div></div>"
            "<div class='kpi'><div class='l'>Violations</div><div class='v' id='kpiViols'>%lu</div><div class='u'>this session</div></div>"
            "<div class='kpi'><div class='l'>GPS</div><div class='v' id='kpiGps'>%s</div><div class='u' id='kpiSats'>%d sats</div></div>"
            "<div class='kpi'><div class='l'>Peak</div><div class='v warn' id='kpiPeak'>%d</div><div class='u'>km/h</div></div>",
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

    // Full Width Recent Violations Card (dynamic in-place updates, zero page reload)
    webServer.sendContent(F(
        "<div class='panel'><div class='phd'>"
        "<h3>Recent violations</h3>"
        "<span class='pill' style='font-size:.72rem'>Live Telemetry</span>"
        "</div><div id='violationsBox'>"));
    if (logCount == 0) {
        webServer.sendContent(F("<div class='empty'>No violations this session.</div>"));
    } else {
        webServer.sendContent(F("<div class=table-wrap><table><thead><tr>"
            "<th>#</th><th>Tier</th><th>Speed</th><th>Limit</th><th>Excess</th><th>Location</th>"
            "</tr></thead><tbody>"));
        int start = (logHead - logCount + LOG_SIZE) % LOG_SIZE;
        for (int i = logCount - 1; i >= 0; i--) {
            int idx = (start + i) % LOG_SIZE;
            ViolationRecord& r = violationLog[idx];
            char row[600];
            snprintf(row, sizeof(row),
                "<tr><td class='mono'>%d</td>"
                "<td><span class='tier-tag tier-%s'>%s</span></td>"
                "<td class='mono'><b>%d</b> <span class='u'>km/h</span></td>"
                "<td class='mono'>%d</td>"
                "<td class='mono' style='color:var(--rose)'>+%d</td>"
                "<td class='mono' style='font-size:.75rem'>"
                "<a href='https://www.google.com/maps/search/?api=1&amp;query=%.6f,%.6f' target=_blank rel=noopener>"
                "%.4f, %.4f &#8599;</a></td></tr>",
                logCount - i, r.tier_str, r.tier_str,
                (int)r.speed, (int)r.limit, (int)(r.speed - r.limit),
                r.lat, r.lon, r.lat, r.lon);
            webServer.sendContent(row);
        }
        webServer.sendContent(F("</tbody></table></div>"));
    }
    webServer.sendContent(F("</div></div>"));

    // Stream dashboard JS from flash
    webServer.sendContent_P(DASHBOARD_JS);

    htmlFoot();
    webServer.sendContent("");
}

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
    webServer.sendContent(F("'>"
        "<label>API key (optional)</label><input name='apiKey' value='"));
    webServer.sendContent(cfg.apiKey);
    webServer.sendContent(F("'><p class=hint>Must match the key on the Velocis server device registry when REQUIRE_AUTH=true.</p>"
        "<label class=chk><input name=sleepEn type=checkbox value=1"));
    if (state.sleepEnabled) webServer.sendContent(F(" checked"));
    webServer.sendContent(F("><span>Enable parked deep-sleep (5 min idle → sleep, MENU wakes)</span></label></div>"));

    // Hardware Diagnostic Bench
    webServer.sendContent(F(
        "<div class=fs><h2>Hardware Diagnostic Bench</h2>"
        "<p class=lead>Test onboard indicators and alert hardware directly from this browser console.</p>"
        "<div class=bench>"
        "<button type=button class='btn bg' onclick=\"fetch('/api/test/buzzer',{method:'POST'}).then(()=>alert('Buzzer pulsed!'))\">Test Buzzer</button>"
        "<button type=button class='btn bg' onclick=\"fetch('/api/test/leds',{method:'POST'}).then(()=>alert('Cycled LEDs!'))\">Cycle LEDs (G/Y/R)</button>"
        "<button type=button class='btn bg' onclick=\"fetch('/api/test/lcd',{method:'POST'}).then(()=>alert('Cycled LCD backlight!'))\">Blink LCD</button>"
        "<button type=button class='btn bg' onclick=\"fetch('/api/mute',{method:'POST'}).then(()=>alert('Buzzer muted 60s!'))\">Mute Buzzer (60s)</button>"
        "</div></div>"
        "<div class=fs><h2>Hardware wiring</h2>"
        "<p class=lead>GPS TX/RX are crossed to the ESP32 UART.</p>"
        "<div class=pinmap>"
        "GPS NEO-6M TX  →  ESP32 GPIO 16 (RX)<br>"
        "GPS NEO-6M RX  →  ESP32 GPIO 17 (TX)<br>"
        "GPS GND        →  ESP32 GND<br>"
        "GPS VCC        →  3.3V or 5V (per module)<br><br>"
        "SIM800L TX     →  ESP32 GPIO 13 (RX)<br>"
        "SIM800L RX     →  ESP32 GPIO 14 (TX) &nbsp;(level shift if 5V logic)<br><br>"
        "Green LED      →  ESP32 GPIO 27<br>"
        "Yellow LED     →  ESP32 GPIO 26<br>"
        "Red LED + buzzer → ESP32 GPIO 25"
        "</div></div>"));

    // Thresholds
    {
        char buf[960];
        snprintf(buf, sizeof(buf),
            "<div class='fs'><h2>Speed limit &amp; thresholds</h2>"
            "<label>Speed limit (km/h)</label>"
            "<input name='defLimit' type='number' value='%d' min='5' max='250'>"
            "<label class=chk><input name=autoZones type=checkbox value=1%s>"
            "<span>Use map zone limits (AUTO) where available — otherwise the limit above always applies</span></label>"
            "<label>Minor (km/h over)</label>"
            "<input name='thrMinor' type='number' value='%d' min='1' max='50'>"
            "<label>Moderate (km/h over)</label>"
            "<input name='thrModerate' type='number' value='%d' min='1' max='50'>"
            "<label>Severe + SMS (km/h over)</label>"
            "<input name='thrSevere' type='number' value='%d' min='1' max='100'>"
            "</div>",
            cfg.defaultSpeedLimit, cfg.autoZones ? " checked" : "",
            cfg.threshMinor, cfg.threshModerate, cfg.threshSevere);
        webServer.sendContent(buf);
    }

    // Phones (indexed names — reliable on ESP WebServer)
    webServer.sendContent(F("<div class='fs'><h2>SMS recipients</h2><div id='pl'>"));
    for (int i = 0; i < cfg.numPhones; i++) {
        char nm[12];
        snprintf(nm, sizeof(nm), "phone%d", i);
        webServer.sendContent(F("<div class='pi'><input type=tel maxlength=19 name='"));
        webServer.sendContent(nm);
        webServer.sendContent(F("' value='"));
        webServer.sendContent(cfg.phones[i]);
        webServer.sendContent(F("' placeholder='+234...'>"
            "<button type='button' class='rm' onclick='rm(this)'>Remove</button></div>"));
    }
    if (cfg.numPhones == 0) {
        webServer.sendContent(F("<div class='pi'><input type=tel maxlength=19 name='phone0' "
            "placeholder='+234...'>"
            "<button type='button' class='rm' onclick='rm(this)'>Remove</button></div>"));
    }
    webServer.sendContent(F("</div>"
        "<input type=hidden name=clearPhones value=1>"
        "<button type='button' class='add' onclick='add()'>+ Add number</button>"
        "<p class='hint'>International format, e.g. +2347059011222 (max 10)</p>"
        "<div class=row>"
        "<a class='btn bp' href=/sms>Open interactive SMS Test</a>"
        "</div></div>"));

    webServer.sendContent(F(
        "<button class='btn bp' type='submit'>Save settings</button>"
        "</form>"
        "<script>"
        "function reindex(){var l=document.getElementById('pl'),is=l.querySelectorAll('input');"
        "for(var i=0;i<is.length;i++)is[i].name='phone'+i;}"
        "function add(){var l=document.getElementById('pl');"
        "if(l.children.length>=10){alert('Max 10');return;}"
        "var d=document.createElement('div');d.className='pi';"
        "d.innerHTML=\"<input type=tel maxlength=19 placeholder='+234...'>"
        "<button type='button' class='rm' onclick='rm(this)'>Remove</button>\";"
        "l.appendChild(d);reindex();d.querySelector('input').focus();}"
        "function rm(b){var l=document.getElementById('pl');"
        "if(l.children.length<=1){l.querySelector('input').value='';reindex();return;}"
        "b.parentElement.remove();reindex();}"
        "</script>"));

    htmlFoot();
    webServer.sendContent("");
}

static const char SMS_PAGE_JS[] PROGMEM = R"JS(<script>
function E(i){return document.getElementById(i)}
function st(id,t,c){var e=E(id);if(e){e.className='statusline '+(c||'');e.textContent=t||''}}
function busy(b,on,label){if(!b)return;if(on){b._t=b.textContent;b.textContent=label;b.classList.add('busy');b.disabled=true}
else{b.textContent=b._t||b.textContent;b.classList.remove('busy');b.disabled=false}}
function flash(el){if(!el)return;el.classList.remove('flash');void el.offsetWidth;el.classList.add('flash')}
function nAll(){return E('quick').querySelectorAll('button[data-n]').length-1}
function target(){var v=E('phone').value.trim(),t=E('target');if(!t)return;
if(v){t.innerHTML='Sending to: <b></b>';t.firstElementChild.textContent=v}
else t.innerHTML='Sending to: <b>all '+nAll()+' saved recipients</b>'}
function pick(b){E('phone').value=b.getAttribute('data-n')||'';
var q=E('quick').querySelectorAll('button');for(var i=0;i<q.length;i++)q[i].classList.remove('on');
b.classList.add('on');flash(b);target()}
function typed(){var v=E('phone').value.trim(),q=E('quick').querySelectorAll('button');
for(var i=0;i<q.length;i++)q[i].classList.toggle('on',(q[i].getAttribute('data-n')||'')===v);target()}
function cnt(){var m=E('message'),c=E('cnt');if(m&&c)c.textContent=m.value.length+'/140'}
function post(url,body){return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body||''})
.then(function(r){return r.json()})}
function sendSms(b){var v=E('phone').value.trim(),body='message='+encodeURIComponent(E('message').value);
if(v)body+='&phone='+encodeURIComponent(v);
busy(b,true,'Sending...');
st('sendStatus',v?'Sending to '+v+' (can take ~10s)...':'Sending to all saved recipients (~10s each)...','busy');
post('/api/sms',body).then(function(d){busy(b,false);st('sendStatus',d.detail||(d.ok?'Sent':'Failed'),d.ok?'ok':'err')})
.catch(function(){busy(b,false);st('sendStatus','No response from device','err')})}
function reinit(b){busy(b,true,'Re-initialising...');st('sendStatus','Re-initialising modem...','busy');
post('/api/gsm-reinit').then(function(d){busy(b,false);var t=E('gsmTitle'),c=E('gsmChip');
t.textContent=d.ok?'Modem ready':'Modem not ready';t.className=d.ok?'ok':'bad';c.textContent='GSM: '+(d.ok?'OK':'FAILED');
st('sendStatus',d.detail||'',d.ok?'ok':'err')})
.catch(function(){busy(b,false);st('sendStatus','No response from device','err')})}
function addRow(){var l=E('pl');if(l.children.length>=10){st('phStatus','Maximum 10 numbers','err');return}
var d=document.createElement('div');d.className='pi';
d.innerHTML="<input type=tel maxlength=19 placeholder='+234...'><button type=button class=rm onclick=rmRow(this)>Remove</button>";
l.appendChild(d);var i=d.querySelector('input');i.focus();flash(i);st('phStatus','Type the new number, then tap Save','')}
function rmRow(b){var l=E('pl'),row=b.parentNode;
if(l.children.length<=1){row.querySelector('input').value='';return}
l.removeChild(row);st('phStatus','Removed - tap Save to apply','')}
function drawQuick(list){var q=E('quick');q.innerHTML='';
var a=document.createElement('button');a.type='button';a.setAttribute('data-n','');a.textContent='All ('+list.length+')';
a.onclick=function(){pick(a)};q.appendChild(a);
list.forEach(function(n){var b=document.createElement('button');b.type='button';b.setAttribute('data-n',n);b.textContent=n;
b.onclick=function(){pick(b)};q.appendChild(b)});
E('rcChip').textContent='Recipients: '+list.length;typed()}
function saveNums(b){var ins=E('pl').querySelectorAll('input'),list=[],seen={};
for(var i=0;i<ins.length;i++){var v=ins[i].value.replace(/\s+/g,'');if(v.length>=7&&!seen[v]){seen[v]=1;list.push(v)}}
var body='clearPhones=1';list.forEach(function(n,i){body+='&phone'+i+'='+encodeURIComponent(n)});
busy(b,true,'Saving...');st('phStatus','Saving...','busy');
post('/api/recipients',body).then(function(d){busy(b,false);
if(!d.ok){st('phStatus',d.detail||'Save failed','err');return}
var saved=d.phones||list;drawQuick(saved);st('phStatus','Saved '+saved.length+' recipient(s)','ok')})
.catch(function(){busy(b,false);st('phStatus','No response from device','err')})}
</script>)JS";

void sendSmsTest() {
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/html"), "");

    htmlHead("SMS Test — Velocis");
    htmlTop("sms");

    // Handlers first, in one chunk, so buttons work even if the page tail is slow.
    webServer.sendContent_P(SMS_PAGE_JS);

    {
        char buf[560];
        snprintf(buf, sizeof(buf),
            "<div class=hero><div class=eyebrow>GSM tools</div>"
            "<h2 class='%s' id=gsmTitle>%s</h2>"
            "<p class=sub>Tap a number to select it, edit the message, then send.</p>"
            "<div class=chips>"
            "<span class=chip id=gsmChip>GSM: %s</span>"
            "<span class=chip id=rcChip>Recipients: %d</span>"
            "</div>"
            "<div class=row style=\"margin-top:12px\">"
            "<button type=button class='btn bg' id=btnReinit onclick=reinit(this)>Re-init modem</button>"
            "</div></div>",
            state.gsmReady ? "ok" : "bad",
            state.gsmReady ? "Modem ready" : "Modem not ready",
            state.gsmReady ? "OK" : "FAILED",
            cfg.numPhones);
        webServer.sendContent(buf);
    }

    webServer.sendContent(F(
        "<div class='fs hi'>"
        "<h2>Send test SMS</h2>"
        "<p class=lead>Saved recipients — tap one to send only to that number:</p>"
        "<div class=quick id=quick>"));
    {
        char b[112];
        snprintf(b, sizeof(b),
            "<button type=button class=on data-n='' onclick=pick(this)>All (%d)</button>",
            cfg.numPhones);
        webServer.sendContent(b);
    }
    for (int i = 0; i < cfg.numPhones; i++) {
        webServer.sendContent(F("<button type=button data-n='"));
        sendHtmlAttr(cfg.phones[i]);
        webServer.sendContent(F("' onclick=pick(this)>"));
        sendHtmlAttr(cfg.phones[i]);
        webServer.sendContent(F("</button>"));
    }
    if (cfg.numPhones == 0)
        webServer.sendContent(F("<span class=hint>No saved numbers yet — add some below.</span>"));

    webServer.sendContent(F("</div>"
        "<label for=phone>To</label>"
        "<input id=phone type=tel maxlength=19 oninput=typed() "
        "placeholder=\"Blank = all saved recipients\">"));
    {
        char t[128];
        snprintf(t, sizeof(t),
            "<div class=target id=target>Sending to: <b>all %d saved recipients</b></div>",
            cfg.numPhones);
        webServer.sendContent(t);
    }

    webServer.sendContent(F(
        "<label for=message>Message <span class=cnt id=cnt></span></label>"
        "<textarea id=message rows=4 maxlength=140 oninput=cnt()>"));
    {
        char def[120];
        snprintf(def, sizeof(def),
            "Velocis TEST from %s - SMS OK. Speed %.0f km/h",
            cfg.deviceID, state.currentSpeed);
        sendHtmlAttr(def);
    }
    webServer.sendContent(F("</textarea>"
        "<div class=row>"
        "<button class='btn bp' type=button id=btnSend onclick=sendSms(this)>Send SMS</button>"
        "</div>"
        "<div class=statusline id=sendStatus></div>"
        "<p class=hint>SIM800L: TX→GPIO13, RX→GPIO14, shared GND, solid 2A supply. "
        "Non-ASCII characters are replaced automatically.</p>"
        "</div>"));

    webServer.sendContent(F(
        "<div class=fs>"
        "<h2>Edit recipients</h2>"
        "<p class=lead>Add, change or remove numbers, then tap Save.</p>"
        "<div id=pl>"));
    int rows = cfg.numPhones > 0 ? cfg.numPhones : 1;
    for (int i = 0; i < rows; i++) {
        webServer.sendContent(F("<div class=pi><input type=tel maxlength=19 placeholder='+234...' value='"));
        if (i < cfg.numPhones) sendHtmlAttr(cfg.phones[i]);
        webServer.sendContent(F("'><button type=button class=rm onclick=rmRow(this)>Remove</button></div>"));
    }
    webServer.sendContent(F("</div>"
        "<button type=button class=add onclick=addRow()>+ Add number</button>"
        "<div class=row>"
        "<button class='btn bp' type=button onclick=saveNums(this)>Save recipients</button>"
        "</div>"
        "<div class=statusline id=phStatus></div>"
        "<p class=hint>International format, e.g. +2347059011222 (max 10)</p>"
        "</div>"
        "<script>cnt();</script>"));

    htmlFoot();
    webServer.sendContent("");
}

// ── Status JSON ───────────────────────────────────────────────
void sendStatusJSON() {
    StaticJsonDocument<1152> doc;
    doc["device"]           = cfg.deviceID;
    doc["speed"]            = state.currentSpeed;
    doc["speed_limit"]      = state.speedLimit;
    doc["limit_setting"]    = cfg.defaultSpeedLimit;
    doc["limit_mode"]       = cfg.autoZones ? "auto" : "manual";
    doc["alarm_muted"]      = millis() < state.buzzerMuteUntilMs;
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
    doc["queue_pending"]    = state.pendingQueue;
    doc["dyn_zones"]        = state.dynZoneCount;
    doc["geofence_version"] = state.geofenceVersion;
    if (gps.satellites.isValid()) doc["satellites"] = gps.satellites.value();
    if (gps.hdop.isValid())       doc["hdop"]       = gps.hdop.hdop();
    if (gps.location.isValid()) {
        doc["lat"]   = serialized(String(gps.location.lat(), 6));
        doc["lon"]   = serialized(String(gps.location.lng(), 6));
        doc["fix_age_ms"] = gps.location.age();
    }
    doc["limit_rev"]      = cfg.limitRev;
    doc["track_points"]   = trackCount;
    doc["track_unsent"]   = trackUnsent;
    doc["track_total"]    = trackTotal;
    doc["track_uploaded"] = trackUploaded;
    String out;
    out.reserve(700);
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

// ── GPS track JSON (RAM buffer, oldest → newest) ─────────────
void sendTrackJSON() {
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", "");
    webServer.sendContent(F("{\"points\":["));
    unsigned long now = millis();
    int startIdx = (trackHead - trackCount + TRACK_BUF) % TRACK_BUF;
    for (int i = 0; i < trackCount; i++) {
        const TrackPoint& p = trackBuf[(startIdx + i) % TRACK_BUF];
        char buf[128];
        snprintf(buf, sizeof(buf),
            "%s{\"lat\":%.6f,\"lon\":%.6f,\"speed\":%.1f,\"limit\":%.0f,\"age_s\":%lu,\"sent\":%s}",
            i ? "," : "", p.lat, p.lon, p.speed, p.limit, (now - p.ms) / 1000UL,
            i < trackCount - trackUnsent ? "true" : "false");
        webServer.sendContent(buf);
    }
    char tail[96];
    snprintf(tail, sizeof(tail), "],\"count\":%d,\"unsent\":%d,\"total\":%lu,\"uploaded\":%lu}",
             trackCount, trackUnsent, trackTotal, trackUploaded);
    webServer.sendContent(tail);
    webServer.sendContent("");
}

// ═══════════════════════════════════════════════════════════
//  WEB SERVER ROUTES
// ═══════════════════════════════════════════════════════════
void setupWebServer() {
    webServer.on("/", HTTP_GET, sendDashboard);

    webServer.on("/api/test/buzzer", HTTP_POST, []() {
        digitalWrite(PIN_BUZZER, ALARM_ON);
        delay(150);
        digitalWrite(PIN_BUZZER, ALARM_OFF);
        webServer.send(200, F("application/json"), F("{\"ok\":true}"));
    });

    webServer.on("/api/test/leds", HTTP_POST, []() {
        digitalWrite(PIN_LED_GREEN, HIGH);
        delay(120);
        digitalWrite(PIN_LED_GREEN, LOW);
        digitalWrite(PIN_LED_YELLOW, HIGH);
        delay(120);
        digitalWrite(PIN_LED_YELLOW, LOW);
        digitalWrite(PIN_LED_RED, ALARM_ON);
        delay(120);
        digitalWrite(PIN_LED_RED, ALARM_OFF);
        webServer.send(200, F("application/json"), F("{\"ok\":true}"));
    });

    webServer.on("/api/test/lcd", HTTP_POST, []() {
        lcd.noBacklight();
        delay(250);
        lcd.backlight();
        webServer.send(200, F("application/json"), F("{\"ok\":true}"));
    });

    webServer.on("/api/mute", HTTP_POST, []() {
        state.buzzerMuteUntilMs = millis() + 60000;
        silenceAlert();
        webServer.send(200, F("application/json"), F("{\"ok\":true,\"muted_s\":60}"));
    });
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
    webServer.on("/sms", HTTP_GET, sendSmsTest);

    webServer.on("/api/sms", HTTP_POST, []() {
        String phone = webServer.hasArg("phone") ? webServer.arg("phone") : "";
        String message = webServer.hasArg("message") ? webServer.arg("message") : "";
        phone.trim();
        message.trim();
        if (message.length() == 0) {
            char def[120];
            snprintf(def, sizeof(def),
                "Velocis TEST from %s - SMS OK. Speed %.0f km/h",
                cfg.deviceID, state.currentSpeed);
            message = def;
        }

        bool ok = false;
        const char* detail;
        if (phone.length() >= 7) {
            ok = sendSMSTo(phone.c_str(), message.c_str());
            detail = ok ? "Sent to selected number" : "Failed — check GSM, SIM, antenna, number format";
        } else if (cfg.numPhones == 0) {
            detail = "No recipients saved";
        } else {
            ok = sendSMS(message.c_str());
            detail = ok ? "Sent to saved recipients" : "Send failed — check GSM power, SIM, antenna";
        }

        StaticJsonDocument<192> doc;
        doc["ok"] = ok;
        doc["detail"] = detail;
        doc["gsm"] = state.gsmReady;
        String out;
        serializeJson(doc, out);
        webServer.send(200, F("application/json"), out);
    });

    webServer.on("/api/recipients", HTTP_POST, []() {
        saveRecipientsFromRequest();
        StaticJsonDocument<384> doc;
        doc["ok"] = true;
        doc["count"] = cfg.numPhones;
        doc["detail"] = "Recipients saved";
        JsonArray arr = doc.createNestedArray("phones");
        for (int i = 0; i < cfg.numPhones; i++) arr.add(cfg.phones[i]);
        String out;
        serializeJson(doc, out);
        webServer.send(200, F("application/json"), out);
    });

    webServer.on("/api/limit", HTTP_POST, []() {
        int kph = webServer.hasArg("limit") ? webServer.arg("limit").toInt() : cfg.defaultSpeedLimit;
        bool autoZ = webServer.hasArg("mode") ? webServer.arg("mode") == "auto" : cfg.autoZones;
        if (kph < 5 || kph > 250) {
            webServer.send(400, F("application/json"),
                F("{\"ok\":false,\"detail\":\"Limit must be 5-250 km/h\"}"));
            return;
        }
        setSpeedLimit(kph, autoZ);
        if (state.uiScreen == UI_LIMIT) renderUi(true);
        StaticJsonDocument<160> doc;
        doc["ok"] = true;
        doc["limit_setting"] = cfg.defaultSpeedLimit;
        doc["limit_mode"] = cfg.autoZones ? "auto" : "manual";
        doc["speed_limit"] = state.speedLimit;
        String out;
        serializeJson(doc, out);
        webServer.send(200, F("application/json"), out);
    });

    webServer.on("/api/gsm-reinit", HTTP_POST, []() {
        state.gsmReady = initGSM();
        StaticJsonDocument<128> doc;
        doc["ok"] = state.gsmReady;
        doc["detail"] = state.gsmReady ? "Modem ready" : "Modem init failed";
        String out;
        serializeJson(doc, out);
        webServer.send(200, F("application/json"), out);
    });

    // Legacy form POST still works
    webServer.on("/sms", HTTP_POST, []() {
        String phone = webServer.hasArg("phone") ? webServer.arg("phone") : "";
        String message = webServer.hasArg("message") ? webServer.arg("message") : "";
        phone.trim();
        message.trim();
        if (message.length() == 0) {
            char def[120];
            snprintf(def, sizeof(def),
                "Velocis TEST from %s - SMS OK. Speed %.0f km/h",
                cfg.deviceID, state.currentSpeed);
            message = def;
        }
        bool ok = false;
        if (phone.length() >= 7) ok = sendSMSTo(phone.c_str(), message.c_str());
        else ok = sendSMS(message.c_str());
        webServer.sendHeader("Location", ok ? "/sms?ok=1" : "/sms?err=1");
        webServer.send(303);
    });

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
        if (webServer.hasArg("apiKey")) {
            strncpy(cfg.apiKey, webServer.arg("apiKey").c_str(), 47);
            cfg.apiKey[47] = '\0';
        }
        state.sleepEnabled = webServer.hasArg("sleepEn");
        if (webServer.hasArg("defLimit")) {
            int v = webServer.arg("defLimit").toInt();
            if (v >= 5 && v <= 250) cfg.defaultSpeedLimit = v;
        }
        cfg.autoZones = webServer.hasArg("autoZones");
        state.speedLimit = state.gpsValid ? getSpeedLimit(state.prevLat, state.prevLon)
                                          : (float)cfg.defaultSpeedLimit;
        if (webServer.hasArg("thrMinor"))
            cfg.threshMinor = webServer.arg("thrMinor").toInt();
        if (webServer.hasArg("thrModerate"))
            cfg.threshModerate = webServer.arg("thrModerate").toInt();
        if (webServer.hasArg("thrSevere"))
            cfg.threshSevere = webServer.arg("thrSevere").toInt();

        // Phones via shared helper (phone0.. or name=phone)
        {
            // Don't double-saveConfig — saveRecipientsFromRequest saves;
            // temporarily skip if no phone args? Always call — clearPhones from settings form.
            saveRecipientsFromRequest();
        }

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
    webServer.on("/api/track",      HTTP_GET, sendTrackJSON);
    webServer.on("/api/scan",       HTTP_GET, sendWifiScanJSON);

    webServer.on("/test-sms", HTTP_GET, []() {
        webServer.sendHeader("Location", "/sms");
        webServer.send(303);
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