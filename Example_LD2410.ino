#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#define ENABLE_WIFI_MANAGER

/*
  This program sets the serial baud rate of the HLK-LD2410
  presence sensor.

  Use these two lines to the correct values:
  [line 30] #define CURRENT_BAUD_RATE LD2410_BAUD_RATE
  [line 31] #define NEW_BAUD_RATE 115200

  #define SERIAL_BAUD_RATE sets the serial monitor baud rate

  Communication with the sensor is handled by the
  "MyLD2410" library Copyright (c) Iavor Veltchev 2024

  Use only hardware UART at the default baud rate 256000,
  or change the #define LD2410_BAUD_RATE to match your sensor.
  For ESP32 or other boards that allow dynamic UART pins,
  modify the RX_PIN and TX_PIN defines in "./board_select.h"

  Connection diagram:
  Arduino/ESP32 RX  -- TX LD2410
  Arduino/ESP32 TX  -- RX LD2410
  Arduino/ESP32 GND -- GND LD2410
  Provide sufficient power to the sensor Vcc (200mA, 5-12V)
  TEST
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <time.h>
#include <Preferences.h>

// OTA + web log viewer
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>
#include <stdarg.h>

#include <ArduinoJson.h>

#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include "ExampleFunctions.h"  // Provides the functions used in the examples.

#define Web_API_KEY "YOUR_FIREBASE_API_KEY"
#define DATABASE_URL "https://thiefdetectorapp-default-rtdb.asia-southeast1.firebasedatabase.app"
#define USER_EMAIL "YOUR_FIREBASE_EMAIL"
#define USER_PASS "YOUR_FIREBASE_PASSWORD"

// Default WiFi credentials (no DB needed)
const char* DEFAULT_WIFI_SSID[] = {"YOUR_WIFI_SSID", "YOUR_WIFI_SSID_2"};
const char* DEFAULT_WIFI_PASS[] = {"YOUR_WIFI_PASSWORD", "YOUR_WIFI_PASSWORD_2"};
const int DEFAULT_WIFI_COUNT = 2;

// Firebase paths
#define WIFI_CONFIG_PATH "board1/wifi_config"
#define WIFI_CUSTOM_SSID_PATH "board1/wifi_config/custom_ssid"
#define WIFI_CUSTOM_PASS_PATH "board1/wifi_config/custom_password"
#define WIFI_USE_CUSTOM_PATH "board1/wifi_config/use_custom"

// User functions
void processData(AsyncResult& aResult);
void pollResultCallback(AsyncResult &aResult);
bool connectToWiFi();
bool scanAndConnectDefaultWiFi();
void loadCustomWiFiFromFirebase();
void saveCustomWiFiToFirebase(String ssid, String password);
void startWiFiConfigPortal();
void handleWiFiConfigChange(String path, RealtimeDatabaseResult &RTDB);
void scanAndSendResults();
void checkWiFiConnection();
void lastOnlineWriteCallback(AsyncResult &aResult);
void triggerSTB();
bool sendWhatsAppText(const String& message);

// Authentication
UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);

SSL_CLIENT ssl_client;

// Firebase components
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

// Firebase Objects
WiFiClient net;
FirebaseApp app;
RealtimeDatabase Database;

// Timer variables for interval
unsigned long lastSendTime = 0;
unsigned long lastPullTime = 0;
const unsigned long pullInterval = 10000;  // Pull every 10 seconds (fits dashboard 30s timeout)

String stopFromString2;
String startFromString2;

long rtc_sec;
unsigned char day_of_week;
struct tm timeinfo;
const char* TypeOf(const char*) {
  return "char*";
}
const char* TypeOf(const int&) {
  return "int";
}
const char* TypeOf(const String&) {
  return "String";
}

unsigned long lastFetchTime = 0;
const unsigned long fetchInterval = 10000;  // 10 seconds
const unsigned long sendInterval = 10000;   // 10 seconds
unsigned long lastPath15LogTime = 0;
const unsigned long path15LogInterval = 30000; // log path 15 updates every 30s

String hourString;
String minuteString;
String startFromString;

int nowhour;
int nowminute;
int totalSecondNows;
int totalStartSecondsInt;
int totalStopSecondsInt;
int targetDistance;
unsigned long ms = 0;
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;  // Example: GMT+7
const int daylightOffset_sec = 0;     // Example: 1 hour daylight saving

// Internet watchdog: if last_online hasn't been written successfully for
// this long, the device has lost internet/Firebase connectivity -> restart.
const unsigned long INTERNET_WATCHDOG_MS = 5UL * 60UL * 1000UL; // 5 minutes
unsigned long lastOnlineOkMs = 0;
bool lastOnlineOkInit = false;

 // Sensor malfunction watchdog: if no LD2410 data frame arrives for this long
// while inside active hours, the sensor is considered broken -> buzzer beep,
// then auto-restart after prolonged silence to recover the board.
const unsigned long SENSOR_MALFUNCTION_MS = 30000UL; // 30s without data frame -> beep
const unsigned long MALFUNCTION_BEEP_INTERVAL_MS = 60UL * 60UL * 1000UL; // every 1 hour
const unsigned long MALFUNCTION_RESTART_MS = 120000UL; // 2min without data -> auto restart
const unsigned long MALFUNCTION_RESTART_COOLDOWN_MS = 300000UL; // min 5min between restarts
unsigned long lastSensorDataMs = 0;
unsigned long lastMalfunctionBeepMs = 0;
unsigned long lastMalfunctionRestartMs = 0;


// Database  path (where the data is)
String listenerPath = "board1/outputs/digital/";
String wifiConfigPath = "board1/wifi_config";

// Async polling state
volatile bool pollScanRequest = false;
volatile bool pollConnectRequest = false;
volatile bool pollRestartRequest = false;
volatile bool pollUseCustomChanged = false;
volatile bool pollCustomWiFiUpdated = false;
bool lastPolledUseCustom = false;
bool firstUseCustomPoll = true;
bool firstCustomSsidPoll = true;
bool firstCustomPassPoll = true;
unsigned long lastPollTime = 0;

// Polled values (set by pollResultCallback, consumed by processConnectRequest)
String connPollSSID = "";
String connPollPass = "";
bool connPollSave = false;

// Declare outputs
const int output1 = 23;
const int output2 = 22;
const int output3 = 21;

const char* serverUrl = "https://waservices.brahmayasa.com:8000/send-message";

int notificationMethod = 1; // 0 = WhatsApp, 1 = Telegram

const char* telegramBotToken = "YOUR_TELEGRAM_BOT_TOKEN";
const char* telegramChatID = "YOUR_TELEGRAM_CHAT_ID";
const char* telegramApiUrl = "https://api.telegram.org/bot";

// Define retry parameters
const int maxRetries = 3;
const unsigned long retryInterval = 10000;  // 10 seconds retry for fast debug

// STB home server (Armbian) capture trigger - fires an RTSP snapshot + notif
const char* STB_TRIGGER_URL = "http://192.168.18.240:8080/trigger";
const int STB_TRIGGER_TIMEOUT_MS = 2500;  // non-blocking, short timeout to keep latency low

// Variables to manage the state
int retryCount = 0;
bool notificationSent = false;
unsigned long lastAttemptTime = 0;
unsigned long lastNotificationTime = 0;
unsigned long notificationInterval = 180;  // seconds, pulled from Firebase path 17
int movingMinRange = 0;   // cm, pulled from Firebase path 22
int movingMaxRange = 600; // cm, pulled from Firebase path 23
int buzzerTone = 1;       // selected alarm melody, pulled from Firebase path 24
bool triggerMoving = true;   // moving target triggers notif/buzzer, path 25
bool triggerStationary = false; // stationary target triggers notif/buzzer, path 26
bool errorState = false;
bool sensorState = true;
// Active-hours monitoring-state tracking for the WhatsApp ON/OFF notice.
bool lastActiveHoursState = false;
bool activeHoursInit = false;
bool useCustomWiFi = false;
String customWiFiSSID = "";
String customWiFiPass = "";
bool wifiConnected = false;
bool pendingWifiSync = true;
Preferences prefs;
bool lastPresenceStatus = false;
bool presenceInit = false;
IPAddress lastTelegramIP;

// Best/nearest in-range sensor distance seen recently, used by the CCTV gate
// check so a CCTV event isn't wrongly blocked just because the person moved
// between the moment the camera detected them and the moment we check.
#define CCTV_DIST_WINDOW_MS 45000  // 45s tolerance window for CCTV events
int bestCctvDistance = -1;
unsigned long bestCctvDistanceTime = 0;

#define FREE_PIN 5
#define STATIC_PIN 19
#define MOVE_PIN 18
#define BUZZER_PIN 15
#define BUZZER_DURATION1 600
#define BUZZER_DURATION2 900
#define BUZZER_DURATION3 800

// pinMode(FREE_PIN, OUTPUT);
// pinMode(STATIC_PIN, OUTPUT);
// pinMode(MOVE_PIN, OUTPUT);
// pinMode(BUZZER_PIN, OUTPUT);

#if defined(ARDUINO_SAMD_NANO_33_IOT) || defined(ARDUINO_AVR_LEONARDO)
// ARDUINO_SAMD_NANO_33_IOT RX_PIN is D1, TX_PIN is D0
// ARDUINO_AVR_LEONARDO RX_PIN(RXI) is D0, TX_PIN(TXO) is D1
#define sensorSerial Serial1
#elif defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32C6)
// RX_PIN is D7, TX_PIN is D6
#define sensorSerial Serial0
#elif defined(ESP32)
// Other ESP32 device - choose available GPIO pins
#define sensorSerial Serial1
#if defined(ARDUINO_ESP32S3_DEV)
#define RX_PIN 18
#define TX_PIN 17
#else
#define RX_PIN 16
#define TX_PIN 17
#endif
#else
#error "This sketch only works on ESP32, Arduino Nano 33IoT, and Arduino Leonardo (Pro-Micro)"
#endif

/*
  This program reads all data received from
  the HLK-LD2410 presence sensor and periodically
  prints the values to the serial monitor.

  Several #defines control the behavior of the program:
  #define SERIAL_BAUD_RATE sets the serial monitor baud rate

  #define ENHANCED_MODE enables the enhanced (engineering)
  mode of the sensor. Comment that line to switch to basic mode.

  #define DEBUG_MODE enables the printing of debug information
  (all received frames are printed). Comment the line to disable
  debugging.

  Communication with the sensor is handled by the
  "MyLD2410" library Copyright (c) Iavor Veltchev 2024

  Use only hardware UART at the default baud rate 256000,
  or change the #define LD2410_BAUD_RATE to match your sensor.
  For ESP32 or other boards that allow dynamic UART pins,
  modify the RX_PIN and TX_PIN defines in "./board_select.h"

  Connection diagram:
  Arduino/ESP32 RX  -- TX LD2410
  Arduino/ESP32 TX  -- RX LD2410
  Arduino/ESP32 GND -- GND LD2410
  Provide sufficient power to the sensor Vcc (200mA, 5-12V)
*/
// #include "./board_select.h"
// Change the communication baud rate here, if previously configured
#define LD2410_BAUD_RATE 38400
#include "MyLD2410.h"

// User defines
// #define DEBUG_MODE
#define ENHANCED_MODE
#define SERIAL_BAUD_RATE 115200


#ifdef DEBUG_MODE
MyLD2410 sensor(sensorSerial, true);
#else
MyLD2410 sensor(sensorSerial);
#endif

// ===================== OTA + WEB LOG =====================
// Captures all Serial output into a ring buffer so it can be viewed
// in a browser (http://<esp-ip>/ ) and allows firmware upload via:
//   - Arduino IDE network port (ArduinoOTA)
//   - browser web updater at /update
#define WEBLOG_SIZE 16384
static char weblogBuffer[WEBLOG_SIZE];
static uint32_t weblogHead = 0;  // next write index
static uint32_t weblogSize = 0;  // total bytes stored (capped at WEBLOG_SIZE)

inline void weblogPush(uint8_t c) {
  weblogBuffer[weblogHead] = c;
  weblogHead = (weblogHead + 1) % WEBLOG_SIZE;
  if (weblogSize < WEBLOG_SIZE) weblogSize++;
}

class WebLogStream : public Print {
public:
  void begin(unsigned long baud) { Serial.begin(baud); }
  size_t write(uint8_t c) override {
    weblogPush(c);
    return Serial.write(c);
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) weblogPush(buffer[i]);
    return Serial.write(buffer, size);
  }
};

WebLogStream webLogStream;
#define Serial webLogStream

WebServer otaServer(80);

void handleRoot() {
  otaServer.sendHeader("Cache-Control", "no-store");
  otaServer.send(200, "text/html", (const char*)F(
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Thief Alarm ESP32</title>"
    "<style>body{font-family:Consolas,monospace;background:#111;color:#eee;margin:0;padding:16px}"
    "h1,h2{color:#4fc3f7}pre{background:#000;border:1px solid #333;padding:10px;height:50vh;overflow:auto;white-space:pre-wrap;font-size:12px}"
    "input,button{padding:8px;margin:4px}button{background:#4fc3f7;border:0;color:#000;font-weight:bold}"
    "#ip{color:#ffd54f}</style></head><body>"
    "<h1>Thief Alarm ESP32 - OTA &amp; Log</h1>"
    "<p>IP: <span id=\"ip\"></span></p>"
    "<h2>Serial Log</h2><pre id=\"log\">connecting...</pre>"
     "<h2>OTA Update (.bin)</h2>"
     "<form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\">"
     "<input type=\"file\" name=\"firmware\"><input type=\"submit\" value=\"Upload &amp; Restart\"></form>"
     "<h2>Control</h2>"
     "<button onclick=\"if(confirm('Restart ESP32 now?'))fetch('/restart')\">Restart ESP32</button>"
     "<script>"
    "var ipEl=document.getElementById('ip');var logEl=document.getElementById('log');"
    "fetch('/ip').then(function(r){return r.text();}).then(function(t){ipEl.textContent=t;});"
    "function refresh(){var x=new XMLHttpRequest();x.open('GET','/log',true);x.onload=function(){"
    "logEl.textContent=x.responseText;logEl.scrollTop=logEl.scrollHeight;};x.send();}"
    "setInterval(refresh,2000);refresh();"
    "</script></body></html>"));
}

void handleLog() {
  otaServer.sendHeader("Cache-Control", "no-store");
  String s;
  uint32_t start = (weblogHead + WEBLOG_SIZE - weblogSize) % WEBLOG_SIZE;
  s.reserve(weblogSize);
  for (uint32_t i = 0; i < weblogSize; i++) {
    s += weblogBuffer[(start + i) % WEBLOG_SIZE];
  }
  otaServer.send(200, "text/plain; charset=utf-8", s);
}

void handleIP() {
  otaServer.sendHeader("Cache-Control", "no-store");
  otaServer.send(200, "text/plain", WiFi.localIP().toString());
}

void handleUpdate() {
  HTTPUpload &upload = otaServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Update: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.println("OTA success, rebooting in 2s");
      delay(2000);
      ESP.restart();
    } else {
      Update.printError(Serial);
    }
  }
}
// =================== END OTA + WEB LOG ===================

const unsigned long printEvery = 1000;  // print every second

int getSecondTime(String timedb);
bool sendNotification();

void printValue(const byte& val) {
  Serial.print(' ');
  Serial.print(val);
}

// Play a familiar alarm melody on the digital buzzer.
// tone: 0=off, 1=single beep, 2=continuous siren, 3=fast pulses,
//       4=double beeps, 5=three short beeps, 6=police siren (slow-fast-slow)
void playAlarmMelody(int tone) {
  switch (tone) {
    case 0: // silent
      digitalWrite(BUZZER_PIN, LOW);
      break;
    case 1: // single beep
      digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW);
      break;
    case 2: // continuous
      digitalWrite(BUZZER_PIN, HIGH); delay(2500); digitalWrite(BUZZER_PIN, LOW);
      break;
    case 3: // fast pulses
      for (int i = 0; i < 6; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(120); digitalWrite(BUZZER_PIN, LOW); delay(120);
      }
      break;
    case 4: // double beeps
      for (int i = 0; i < 2; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(220); digitalWrite(BUZZER_PIN, LOW); delay(180);
      }
      break;
    case 5: // three short beeps
      for (int i = 0; i < 3; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(160); digitalWrite(BUZZER_PIN, LOW); delay(140);
      }
      break;
    case 6: // classic siren: slow long pulses (wailing)
      for (int i = 0; i < 3; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW); delay(300);
      }
      break;
    default: // single beep
      digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW);
      break;
  }
}

// Malfunction alert: rapid "nit-nit-nit" pattern (3 very fast beeps).
void playMalfunctionBeep() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(60); digitalWrite(BUZZER_PIN, LOW); delay(60);
  }
}

// Returns true when the current local time is inside the active-hours window
// (paths 13 start / 14 stop). Used by both printData() and the malfunction
// watchdog in loop().
bool isWithinWorkingHours() {
  if (getLocalTime(&timeinfo)) {
    nowhour = timeinfo.tm_hour;
    nowminute = timeinfo.tm_min;
  }
  totalSecondNows = (nowhour * 3600UL) + (nowminute * 60UL);
  totalStartSecondsInt = getSecondTime(startFromString2);
  totalStopSecondsInt = getSecondTime(stopFromString2);
  if (startFromString2.length() > 0 && stopFromString2.length() > 0) {
    if (totalStartSecondsInt <= totalStopSecondsInt) {
      return (totalSecondNows >= totalStartSecondsInt && totalSecondNows <= totalStopSecondsInt);
    } else {
      return (totalSecondNows >= totalStartSecondsInt || totalSecondNows <= totalStopSecondsInt);
    }
  }
  return true;
}

void printData() {

  digitalWrite(FREE_PIN, HIGH);
  digitalWrite(STATIC_PIN, LOW);
  digitalWrite(MOVE_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // A data frame arrived -> sensor is alive. Reset the malfunction timer.
  lastSensorDataMs = millis();

  // Compute active-hours window FIRST so the sensor can be disabled outside it.
  bool withinWorkingHours = isWithinWorkingHours();

  // Push presenceDetected() status to Firebase so the dashboard can show it.
  // Outside active hours the sensor is disabled: report "no presence".
  bool presenceNow = withinWorkingHours && sensor.presenceDetected();
  if (app.ready() && (!presenceInit || presenceNow != lastPresenceStatus)) {
    lastPresenceStatus = presenceNow;
    presenceInit = true;
    Database.set<bool>(aClient, "board1/outputs/digital/21", presenceNow);
  }

  // Serial.print("Data frame #: ");
  // Serial.println(sensor.getFrameCount());
  // Serial.print("Time stamp [ms]: ");
  // Serial.println(sensor.getTimestamp());
  // Serial.print(sensor.statusString());
  if (sensor.presenceDetected()) {
    digitalWrite(FREE_PIN, LOW);

    // Serial.print(", distance: ");
    // Serial.print(sensor.detectedDistance());
    // Serial.print("cm");
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
  Serial.println();
  if (sensor.movingTargetDetected()) {
    // printLocalTime();

    digitalWrite(MOVE_PIN, HIGH);
    Serial.print(" MOVING    = ");
    Serial.print(sensor.movingTargetSignal());
    Serial.print("@");
    Serial.print(sensor.movingTargetDistance());
    Serial.print("cm ");
    if (sensor.inEnhancedMode()) {
      // Serial.print("\n signals->[");
      // sensor.getMovingSignals().forEach(printValue);
      // Serial.print(" ] thresholds:[");
      // sensor.getMovingThresholds().forEach(printValue);
      // Serial.print(" ]");
    }
    Serial.println();

    // Update path 15 for moving target too
    char dtBuf19[30];
    strftime(dtBuf19, sizeof(dtBuf19), "%Y-%m-%d %H:%M:%S", &timeinfo);
    String dateTimeNow = String(dtBuf19);
    if (app.ready()) {
      if(sensor.presenceDetected() && withinWorkingHours){
        bool ok = Database.set<String>(aClient, "board1/outputs/digital/15", dateTimeNow + " MOVING " + String(sensor.movingTargetDistance()) + "cm");
        bool ok19 = Database.set<String>(aClient, "board1/outputs/digital/19", dateTimeNow + " " + String(sensor.movingTargetDistance()) + "cm");

        if (!ok) {
          Serial.printf("❌ Gagal update path 15 (moving): %s\n", aClient.lastError().message().c_str());
        } else if (millis() - lastPath15LogTime >= path15LogInterval) {
          lastPath15LogTime = millis();
          Serial.println("✅ Path 15 update OK (moving, throttled log)");
        }
        if (!ok19) {
          Serial.printf("❌ Gagal update path 19 (moving): %s\n", aClient.lastError().message().c_str());
        }
      }else{
        bool ok = Database.set<String>(aClient, "board1/outputs/digital/15", dateTimeNow + " MOVING 0cm");
        bool ok19 = Database.set<String>(aClient, "board1/outputs/digital/19", dateTimeNow +  " 0cm");
        
      }
      
    } else {
      Serial.println("⚠️  Firebase belum ready, skip update path 15 (moving)");
    }
  }
  if (sensor.stationaryTargetDetected()) {
    digitalWrite(STATIC_PIN, HIGH);

    Serial.print(" STATIONARY= ");
    Serial.print(sensor.stationaryTargetSignal());
    Serial.print("@");
    Serial.print(sensor.stationaryTargetDistance());
    Serial.print("cm ");

    char dtBuf20[30];
    strftime(dtBuf20, sizeof(dtBuf20), "%Y-%m-%d %H:%M:%S", &timeinfo);
    String dateTimeNow = String(dtBuf20);

    // Database.set<number_t>(aClient, "board1/outputs/digital/15", char*);
    if (app.ready()) {
      if(sensor.presenceDetected() && withinWorkingHours){
        bool ok = Database.set<String>(aClient, "board1/outputs/digital/15", dateTimeNow + " " + String(sensor.stationaryTargetDistance()) + "cm");
        bool ok20 = Database.set<String>(aClient, "board1/outputs/digital/20", dateTimeNow + " " + String(sensor.stationaryTargetDistance()) + "cm");

        if (!ok) {
          Serial.printf("❌ Gagal update path 15: %s\n", aClient.lastError().message().c_str());
        } else if (millis() - lastPath15LogTime >= path15LogInterval) {
          lastPath15LogTime = millis();
          Serial.println("✅ Path 15 update OK (throttled log)");
        }
        if (!ok20) {
          Serial.printf("❌ Gagal update path 20 (stationary): %s\n", aClient.lastError().message().c_str());
        }
      }else{
        bool ok = Database.set<String>(aClient, "board1/outputs/digital/15", dateTimeNow + " 0cm");
        bool ok20 = Database.set<String>(aClient, "board1/outputs/digital/20", dateTimeNow + " 0cm");
      }

      
    } else {
      Serial.println("⚠️  Firebase belum ready, skip update path 15");
    }

    if (sensor.inEnhancedMode()) {
      // Serial.print("\n signals->[");
      // sensor.getStationarySignals().forEach(printValue);
      // Serial.print(" ] thresholds:[");
      // sensor.getStationaryThresholds().forEach(printValue);
      // Serial.print(" ]");
    }
    Serial.println();
  }

  // Track the best in-range distance seen recently so the CCTV gate can
  // tolerate the delay between camera detection and our check.
  // Prefer whichever target (moving or stationary) is actually IN range.
  {
    int mv = sensor.movingTargetDetected() ? sensor.movingTargetDistance() : -1;
    int st = sensor.stationaryTargetDetected() ? sensor.stationaryTargetDistance() : -1;
    bool mvIn = (mv >= movingMinRange && mv <= movingMaxRange);
    bool stIn = (st >= movingMinRange && st <= movingMaxRange);
    int probe = -1;
    if (mvIn) {
      probe = mv;
    } else if (stIn) {
      probe = st;
    }
    if (probe >= movingMinRange && probe <= movingMaxRange) {
      bestCctvDistance = probe;
      bestCctvDistanceTime = millis();
    }
    // if no target in range, keep the last good value within the window
    if (millis() - bestCctvDistanceTime > CCTV_DIST_WINDOW_MS) {
      bestCctvDistance = -1;
    }
    // Throttled debug: see what the sensor reports and what we tracked
    static unsigned long lastCctvDbgLog = 0;
    if (millis() - lastCctvDbgLog >= 5000) {
      lastCctvDbgLog = millis();
      Serial.printf("[cctv] mv=%d st=%d best=%d\n", mv, st, bestCctvDistance);
    }
  }

  // (withinWorkingHours already computed at the top of printData() so the
  //  sensor could be disabled before any presence writes happen.)
  Serial.println(totalStartSecondsInt);
  Serial.println(totalStopSecondsInt);

  if (!withinWorkingHours) {
    // Outside active hours: sensor disabled, no buzzer, no notifications.
    notificationSent = false;
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(STATIC_PIN, LOW);
    digitalWrite(MOVE_PIN, LOW);
    targetDistance = 0;
  }  if (sensor.movingTargetDetected() && sensor.stationaryTargetDetected()) {
    Serial.print("MODE DEBUG MOVING AND TARGET STATIC DETECTED.");
  }

  // Master switch: Sensor Notification button (path 12). If OFF, force all
  // notifications and buzzer off. If ON, normal logic applies below.
  if (!sensorState) {
    notificationSent = false;
    digitalWrite(BUZZER_PIN, LOW);
  }

  if (!withinWorkingHours) {
    Serial.println("Sensor disabled (outside active hours)");
  } else if (sensor.presenceDetected()) {
    if (sensor.movingTargetDetected() || sensor.stationaryTargetDetected()) {
      bool shouldTrigger = false;
      if (sensor.movingTargetDetected() && triggerMoving) {
        targetDistance = sensor.movingTargetDistance();
        shouldTrigger = true;
      } else if (sensor.stationaryTargetDetected() && triggerStationary) {
        targetDistance = sensor.stationaryTargetDistance();
        shouldTrigger = true;
      }

      if (shouldTrigger) {
        bool withinMovingRange = (targetDistance >= movingMinRange && targetDistance <= movingMaxRange);

        if (sensorState && withinWorkingHours && withinMovingRange) {
          unsigned long now = millis();
          if (now - lastNotificationTime >= notificationInterval * 1000UL || lastNotificationTime == 0) {
            lastNotificationTime = now;
            notificationSent = true;

            playAlarmMelody(buzzerTone);
            triggerSTB();  // STB captures RTSP snapshot + sends notif w/ photo

            Serial.print("NOTIF TRIGGERED! Distance: ");
            Serial.print(targetDistance);
            Serial.println("cm");
          } else {
            unsigned long remain = (notificationInterval * 1000UL - (now - lastNotificationTime)) / 1000;
            Serial.print("✅ Notif cooldown: ");
            Serial.print(remain);
            Serial.println("s remaining");
          }
        } else {
          notificationSent = false;
        }
      } else {
        notificationSent = false;
      }
    }else{
      notificationSent = false;
    }
  }else{
    
        targetDistance = 0;
      

  }

  if (sensor.inEnhancedMode() && (sensor.getFirmwareMajor() > 1)) {
    // Serial.print("Light level: ");
    // Serial.println(sensor.getLightLevel());
    // Serial.print("Output level: ");
    // Serial.println((sensor.getOutLevel()) ? "HIGH" : "LOW");
  }

  Serial.println();
}

void printLocalTime() {
  // struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
}

void setup() {

  pinMode(FREE_PIN, OUTPUT);
  pinMode(STATIC_PIN, OUTPUT);
  pinMode(MOVE_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);
  Serial.println("[BOOT] OTA upload test OK - firmware updated via web updater");

  prefs.begin("wifi", false);

  // Connect to WiFi with retry
  int wifiRetries = 0;
  while (!connectToWiFi()) {
    wifiRetries++;
    Serial.printf("WiFi attempt %d failed. Retrying in 10s...\n", wifiRetries);
    if (wifiRetries >= 5) {
      Serial.println("Too many WiFi failures. Restarting...");
      ESP.restart();
    }
    delay(10000);
  }

  // Sync time BEFORE Firebase auth as well — TLS/JWT flows can be sensitive to system clock.
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Start OTA + web log server (after WiFi is connected)
  ArduinoOTA.setHostname("thief-alarm-esp32");
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.printf("OTA start: %s\n", type.c_str());
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA end, rebooting");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("OTA error: %u\n", err);
    ESP.restart();
  });
  ArduinoOTA.begin();
  otaServer.on("/", handleRoot);
  otaServer.on("/log", handleLog);
  otaServer.on("/ip", handleIP);
  otaServer.on("/restart", []() {
    otaServer.sendHeader("Cache-Control", "no-store");
    otaServer.send(200, "text/plain", "ESP32 restarting now...");
    delay(100);
    ESP.restart();
  });
  otaServer.on("/cctv", HTTP_GET, []() {
    // CCTV event callback: only fire a notification if the current config
    // (sensor master switch, active hours, moving range) allows it.
    // Uses the best in-range distance seen in the last CCTV_DIST_WINDOW_MS
    // so a CCTV event isn't wrongly blocked just because the person moved
    // between camera detection and our check.
    bool withinWorkingHours = isWithinWorkingHours();
    bool haveDist = (bestCctvDistance >= movingMinRange && bestCctvDistance <= movingMaxRange);
    String reason = "";

    if (!sensorState) {
      reason = "sensor_disabled";
    } else if (!withinWorkingHours) {
      reason = "outside_active_hours";
    } else if (!haveDist) {
      reason = "no_recent_in_range_target";
    }

    if (reason.length() == 0) {
      Serial.printf("CCTV check: ALLOWED (bestDist=%d)\n", bestCctvDistance);
      otaServer.send(200, "application/json",
                     "{\"allowed\":true,\"distance\":" + String(bestCctvDistance) + "}");
    } else {
      Serial.printf("CCTV check: BLOCKED (%s), bestDist=%d\n", reason.c_str(), bestCctvDistance);
      otaServer.send(200, "application/json",
                     "{\"allowed\":false,\"reason\":\"" + reason +
                     "\",\"distance\":" + String(bestCctvDistance) + "}");
    }
  });
  otaServer.on("/update", HTTP_POST, []() {
    otaServer.sendHeader("Location", "/");
    otaServer.send(303);
  }, handleUpdate);
  otaServer.begin();
  Serial.printf("OTA + web log ready: http://%s/\n", WiFi.localIP().toString().c_str());

  // Declare pins as outputs
  pinMode(output1, OUTPUT);
  pinMode(output2, OUTPUT);
  pinMode(output3, OUTPUT);
  // Configure SSL client
  ssl_client.setInsecure();
#if defined(ESP32)
  ssl_client.setTimeout(10000);
  ssl_client.setHandshakeTimeout(30);
#elif defined(ESP8266)
  ssl_client.setTimeout(10000);
  ssl_client.setBufferSizes(4096, 1024);
#endif

  // Initialize Firebase
  initializeApp(aClient, app, getAuth(user_auth), auth_debug_print, "🔐 authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  // Load custom WiFi config from Firebase
  loadCustomWiFiFromFirebase();

  // No permanent stream: all Firebase reads/writes go through the async
  // polling block in loop(). This keeps only ONE Firebase SSL client
  // (~32KB mbedTLS) alive, leaving contiguous heap for the Telegram TLS
  // client and avoiding heap fragmentation crashes.
  Serial.println("Async polling mode (no stream): using single SSL client");

  // Mark WiFi sync as pending (will run in loop when app.ready())
  if (WiFi.status() == WL_CONNECTED) {
    pendingWifiSync = true;
    Serial.println("WiFi sync deferred until Firebase ready");
  }

#if defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32C6) || defined(ARDUINO_SAMD_NANO_33_IOT) || defined(ARDUINO_AVR_LEONARDO) || defined(ARDUINO_AVR_NANO) || defined(ARDUINO_AVR_UNO)
  sensorSerial.begin(LD2410_BAUD_RATE);
#else
  sensorSerial.begin(LD2410_BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);
#endif
  delay(2000);
  Serial.println(__FILE__);
  if (!sensor.begin()) {
    Serial.println("Failed to communicate with the sensor.");
    while (true) {}
  }

#ifdef ENHANCED_MODE
  sensor.enhancedMode();
#else
  sensor.enhancedMode(false);
#endif

  delay(printEvery);
}

void loop() {

  // Maintain authentication and async tasks
  app.loop();

  // OTA + web log server
  ArduinoOTA.handle();
  otaServer.handleClient();

  // Check WiFi connection every 30 seconds
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 30000) {
    lastWiFiCheck = millis();
    checkWiFiConnection();
  }

  // Check if authentication is ready
  if (app.ready())
  {
      // Send pending WiFi status sync (deferred from setup)
      if (pendingWifiSync && WiFi.status() == WL_CONNECTED) {
        pendingWifiSync = false;
        Database.set<String>(aClient, "board1/wifi_config/last_connected", WiFi.SSID());
        Database.set<bool>(aClient, "board1/wifi_config/connected", true);
        Database.set<int>(aClient, "board1/outputs/digital/18", WiFi.RSSI());
        Serial.println("WiFi deferred sync done");
      }

      // Update last_online every 60 seconds (with success callback + watchdog)
      static unsigned long lastOnlineUpdate = 0;
      if (millis() - lastOnlineUpdate >= 60000) {
        lastOnlineUpdate = millis();
        if (getLocalTime(&timeinfo)) {
          char buf[30];
          strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
          Database.set<String>(aClient, "board1/wifi_config/last_online", String(buf), lastOnlineWriteCallback);
        }
      }

      // Internet watchdog: if no successful last_online write in a while,
      // the device likely lost internet/Firebase access -> hard restart.
      if (lastOnlineOkInit && millis() - lastOnlineOkMs >= INTERNET_WATCHDOG_MS) {
        Serial.println("⚠️  No successful last_online write for watchdog period -> restarting");
        delay(200);
        ESP.restart();
      }

      //Do nothing - everything works with callback functions
      unsigned long currentTime2 = millis();
      if (currentTime2 - lastSendTime >= sendInterval) {
        // Update the last send time
        lastSendTime = currentTime2;
        Serial.printf("Program running for %lu\n", currentTime2);
      }


      // 2. Non-blocking Pull Interval (fallback health check)
      if (currentTime2 - lastPullTime > pullInterval) {
        lastPullTime = currentTime2;
        Serial.println("Health check pull...............");
        Serial.print("notificationInterval from FB: ");
        Serial.println(notificationInterval);
        Serial.println("startFromString2 Await");
        Serial.println(startFromString2);
        Serial.println("sensorState Await");
        Serial.println(sensorState);
        Serial.println("stopFromString2 Await");
        Serial.println(stopFromString2);

        // Async (non-blocking) polls. Results arrive via pollResultCallback()
        // when app.loop() processes them. Avoids blocking the main loop and
        // heap pressure that caused TLS crashes.
        Database.get(aClient, "board1/wifi_config/scan_request", pollResultCallback, false, "scan_req");
        Database.get(aClient, "board1/wifi_config/connect_ssid", pollResultCallback, false, "conn_ssid");
        Database.get(aClient, "board1/wifi_config/connect_pass", pollResultCallback, false, "conn_pass");
        Database.get(aClient, "board1/wifi_config/connect_save", pollResultCallback, false, "conn_save");
        Database.get(aClient, "board1/wifi_config/connect_request", pollResultCallback, false, "conn_req");
        Database.get(aClient, "board1/wifi_config/restart_request", pollResultCallback, false, "restart_req");
        Database.get(aClient, WIFI_USE_CUSTOM_PATH, pollResultCallback, false, "use_custom");
        Database.get(aClient, WIFI_CUSTOM_SSID_PATH, pollResultCallback, false, "custom_ssid");
        Database.get(aClient, WIFI_CUSTOM_PASS_PATH, pollResultCallback, false, "custom_pass");
        Database.get(aClient, "board1/outputs/digital/12", pollResultCallback, false, "dig_12");
        Database.get(aClient, "board1/outputs/digital/13", pollResultCallback, false, "dig_13");
        Database.get(aClient, "board1/outputs/digital/14", pollResultCallback, false, "dig_14");
        Database.get(aClient, "board1/outputs/digital/16", pollResultCallback, false, "dig_16");
        Database.get(aClient, "board1/outputs/digital/17", pollResultCallback, false, "dig_17");
        Database.get(aClient, "board1/outputs/digital/22", pollResultCallback, false, "dig_22");
        Database.get(aClient, "board1/outputs/digital/23", pollResultCallback, false, "dig_23");
        Database.get(aClient, "board1/outputs/digital/24", pollResultCallback, false, "dig_24");
        Database.get(aClient, "board1/outputs/digital/25", pollResultCallback, false, "dig_25");
        Database.get(aClient, "board1/outputs/digital/26", pollResultCallback, false, "dig_26");
      }

      // Act on flags set by pollResultCallback (run outside the callback)
      if (pollScanRequest) {
        pollScanRequest = false;
        Serial.println(">>> MANUAL SCAN TRIGGER <<<");
        scanAndSendResults();
      }
      if (pollConnectRequest) {
        pollConnectRequest = false;
        processConnectRequest();
      }
      if (pollRestartRequest) {
        pollRestartRequest = false;
        Serial.println(">>> RESTART REQUESTED FROM DASHBOARD <<<");
        if (app.ready()) {
          Database.set<bool>(aClient, "board1/wifi_config/restart_request", false);
        }
        delay(100);
        ESP.restart();
      }
      if (pollUseCustomChanged) {
        pollUseCustomChanged = false;
        Serial.println(">>> USE CUSTOM WIFI CHANGED <<<");
        connectToWiFi();
      }
      if (pollCustomWiFiUpdated) {
        pollCustomWiFiUpdated = false;
        Serial.println(">>> CUSTOM WIFI PARAMS CHANGED, reconnecting <<<");
        connectToWiFi();
      }
      // if (Database.lastError().code() == 0) {
      // Serial.print("Data: ");
      // Serial.println(jsonStr);
      // } else {
      //     Serial.printf("Error: %s\n", Database.lastError().message().c_str());
      // }
    
  }

  static unsigned long nextPrint = 0;
  if ((sensor.check() == MyLD2410::Response::DATA) && (millis() >= nextPrint)) {
    nextPrint = millis() + printEvery;
    printData();
  }

  // Direct ESP32 push is DISABLED to avoid duplicate WhatsApp messages:
  // STB triggerSTB() already sends the notification (with camera photo).
  // notificationSent is only used to track state for the STB trigger above.
  if (notificationSent) {
    notificationSent = false;
  }

  // Sensor malfunction watchdog: while inside active hours, if no data frame
  // has arrived for SENSOR_MALFUNCTION_MS, the LD2410 is presumed broken.
  // Beep nit-nit-nit quickly once every hour until the sensor recovers.
  if (sensorState && isWithinWorkingHours()) {
    if (millis() - lastSensorDataMs >= SENSOR_MALFUNCTION_MS) {
      if (millis() - lastMalfunctionBeepMs >= MALFUNCTION_BEEP_INTERVAL_MS) {
        lastMalfunctionBeepMs = millis();
        Serial.println("⚠️  SENSOR MALFUNCTION (no data frame) -> beeping");
        playMalfunctionBeep();
      }
      // Auto-restart after prolonged silence (sensor not recovering).
      if (millis() - lastSensorDataMs >= MALFUNCTION_RESTART_MS) {
        if (millis() - lastMalfunctionRestartMs >= MALFUNCTION_RESTART_COOLDOWN_MS) {
          lastMalfunctionRestartMs = millis();
          Serial.println("⚠️  SENSOR DEAD >2min -> auto-restarting ESP32");
          delay(200);
          ESP.restart();
        }
      }
    } else {
      lastMalfunctionBeepMs = 0;
      lastMalfunctionRestartMs = millis(); // sensor healthy -> reset cooldown
    }
  }

  // WhatsApp notice when the active-hours monitoring state flips ON/OFF
  // (path 13 start / path 14 stop). Only sent if the LD2410 is healthy
  // (recent data frame) so we never claim monitoring is armed while the
  // sensor is hung. First pass only records a baseline so boot doesn't spam.
  // The tracker is armed 60s after boot so the NTP clock has settled (the
  // RTC survives soft-resets with the pre-boot time, which would otherwise
  // cause a spurious flip when NTP corrects it).
  static unsigned long lastHoursCheckMs = 0;
  static unsigned long hoursBootMs = millis();
  if (millis() - hoursBootMs >= 60000 && millis() - lastHoursCheckMs >= 1000) {
    lastHoursCheckMs = millis();
    if (getLocalTime(&timeinfo)) {
      totalSecondNows = (nowhour * 3600UL) + (nowminute * 60UL);
      totalStartSecondsInt = getSecondTime(startFromString2);
      totalStopSecondsInt = getSecondTime(stopFromString2);
      bool nowWithin = false;
      if (startFromString2.length() > 0 && stopFromString2.length() > 0) {
        if (totalStartSecondsInt <= totalStopSecondsInt) {
          nowWithin = (totalSecondNows >= totalStartSecondsInt && totalSecondNows <= totalStopSecondsInt);
        } else {
          nowWithin = (totalSecondNows >= totalStartSecondsInt || totalSecondNows <= totalStopSecondsInt);
        }
      } else {
        nowWithin = true;
      }

      if (!activeHoursInit) {
        activeHoursInit = true;
        lastActiveHoursState = nowWithin;
        Serial.printf("active-hours baseline = %d (no WA notice at boot)\n", nowWithin);
      } else if (nowWithin != lastActiveHoursState) {
        lastActiveHoursState = nowWithin;
        bool sensorHealthy = (millis() - lastSensorDataMs < SENSOR_MALFUNCTION_MS);
        if (sensorHealthy) {
          String msg = nowWithin
            ? "Notifikasi monitoring AKTIF (jam aktif mulai " + startFromString2 + ")"
            : "Notifikasi monitoring NONAKTIF (jam aktif berakhir " + stopFromString2 + ")";
          Serial.println(nowWithin ? ">>> active-hours ON -> sending WA 'AKTIF'" : ">>> active-hours OFF -> sending WA 'NONAKTIF'");
          sendWhatsAppText(msg);
        } else {
          Serial.println(">>> active-hours state flipped but sensor UNHEALTHY -> WA notice skipped");
        }
      }
    }
  }
}

// Fire a capture+notification trigger at the STB home server. Non-blocking
// (short timeout) so the sensor->capture latency stays small.
void triggerSTB() {
  HTTPClient http;
  String url = String(STB_TRIGGER_URL) + "?distance=" + String(targetDistance) + "&source=ld2410";
  http.begin(url);
  http.setTimeout(STB_TRIGGER_TIMEOUT_MS);
  int code = http.GET();
  if (code > 0) {
    Serial.printf("STB trigger: HTTP %d\n", code);
    String resp = http.getString();
    if (resp.length() > 120) resp = resp.substring(0, 120);
    Serial.printf("STB resp: %s\n", resp.c_str());
  } else {
    Serial.printf("STB trigger failed: %d\n", code);
  }
  http.end();
  Serial.flush();
}

bool sendTelegramNotification() {
  Serial.println("=== TELEGRAM NOTIFICATION START ===");
  Serial.print("Target distance: ");
  Serial.print(targetDistance);
  Serial.println("cm");

  // Resolve DNS for debugging
  IPAddress ip;
  String ipStr = "";
  if (WiFi.hostByName("api.telegram.org", ip)) {
    ipStr = ip.toString();
    Serial.printf("api.telegram.org resolves to: %s\n", ipStr.c_str());
  } else {
    Serial.println("DNS resolution FAILED");
  }

  // Diagnostik: raw TCP test sebelum HTTPS
  Serial.println("--- TCP DIAGNOSTIC ---");
  const char* testHosts[] = {"api.telegram.org", "149.154.166.110", "google.com", "waservices.brahmayasa.com"};
  for (int t = 0; t < 4; t++) {
    WiFiClient testClient;
    Serial.printf("TCP connect %s:443 ... ", testHosts[t]);
    if (testClient.connect(testHosts[t], 443)) {
      Serial.println("OK ✅");
      testClient.stop();
    } else {
      Serial.printf("FAILED (error: %d) ❌\n", testClient.available());
    }
    delay(10);
  }
  // Juga test WhatsApp port
  WiFiClient waTest;
  Serial.print("TCP connect waservices.brahmayasa.com:8000 ... ");
  if (waTest.connect("waservices.brahmayasa.com", 8000)) {
    Serial.println("OK ✅");
    waTest.stop();
  } else {
    Serial.println("FAILED ❌");
  }
  Serial.println("--- END DIAGNOSTIC ---");

  String message = "Human DETECTED (" + String(targetDistance) + "cm distance) at your Home!";
  StaticJsonDocument<512> doc;
  doc["chat_id"] = telegramChatID;
  doc["text"] = message;
  doc["parse_mode"] = "HTML";
  String payload;
  serializeJson(doc, payload);

  // Build HTTP body once
  String httpBody = "POST /bot" + String(telegramBotToken) + "/sendMessage HTTP/1.1\r\n";
  httpBody += "Host: api.telegram.org\r\n";
  httpBody += "Content-Type: application/json\r\n";
  httpBody += "Connection: close\r\n";
  httpBody += "Content-Length: " + String(payload.length()) + "\r\n";
  httpBody += "\r\n";
  httpBody += payload;

  // Try: resolved IP, domain name, fallback IPs
  String hosts[5];
  int n = 0;
  if (ipStr.length() > 0) hosts[n++] = ipStr;
  hosts[n++] = "api.telegram.org";
  hosts[n++] = "149.154.166.110";
  hosts[n++] = "149.154.167.220";
  hosts[n++] = "149.154.167.221";

  // Only ONE Firebase SSL client (aClient) is active now (no stream), so
  // the Telegram TLS client has enough contiguous heap without stopping anything.
  bool telegramSent = false;
  for (int i = 0; i < n; i++) {
    WiFiClientSecure tgClient;
    tgClient.setInsecure();
    tgClient.setTimeout(10000);

    Serial.printf("TLS connect %s:443 ... ", hosts[i].c_str());

    // Heap diagnostics before TLS
    Serial.printf("\n   Free heap: %u bytes, Max alloc: %u bytes\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // Try to free memory
    delay(100);

    int ret;
    if (hosts[i].indexOf('.') == -1 || (hosts[i].charAt(0) >= '0' && hosts[i].charAt(0) <= '9')) {
      IPAddress targetIP;
      targetIP.fromString(hosts[i]);
      ret = tgClient.connect(targetIP, 443);
    } else {
      ret = tgClient.connect(hosts[i].c_str(), 443);
    }

    if (!ret) {
      char tlsErrBuf[64];
      tgClient.lastError(tlsErrBuf, sizeof(tlsErrBuf));
      Serial.printf("FAILED (%s)\n", tlsErrBuf);
      tgClient.stop();
      delay(50);
      continue;
    }
    Serial.println("OK ✅");

    tgClient.print(httpBody);

    unsigned long timeout = millis() + 10000;
    String response = "";
    bool success = false;
    while (millis() < timeout) {
      if (tgClient.available()) {
        response = tgClient.readStringUntil('\n');
        Serial.print("> ");
        Serial.println(response);
        if (response.startsWith("HTTP/")) {
          int space1 = response.indexOf(' ');
          int space2 = response.indexOf(' ', space1 + 1);
          if (space1 > 0 && space2 > space1) {
            int httpCode = response.substring(space1 + 1, space2).toInt();
            if (httpCode == 200) {
              while (tgClient.available()) {
                String h = tgClient.readStringUntil('\n');
                if (h == "\r" || h == "") break;
              }
              String body = tgClient.readString();
              if (body.length() > 200) body = body.substring(0, 200) + "...";
              Serial.println(body);
              success = true;
            }
          }
        }
        if (success) break;
      }
    }

    tgClient.stop();
    if (success) {
      Serial.println("========== ✅ TELEGRAM SENT ✅ ==========");
      telegramSent = true;
      break;
    }
    Serial.println("❌ No valid HTTP 200 response");
    delay(50);
  }

  if (!telegramSent)
    Serial.println("❌ ALL TELEGRAM HOSTS FAILED");

  Serial.println("=== TELEGRAM NOTIFICATION END ===");
  Serial.println("=== TELEGRAM NOTIFICATION END ===");

  return telegramSent;
}

bool sendWhatsAppNotification() {
  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setTimeout(20000);
  String postData = "sender=YOUR_WA_SENDER&number=YOUR_WA_GROUP&is_group_target=1&message=Human DETECTED (" + String(targetDistance) + "cm distance) at your Home!&token_auth=YOUR_WA_TOKEN";

  int httpResponseCode = http.POST(postData);

  if (httpResponseCode > 0) {
    Serial.printf("WA HTTP Response code: %d\n", httpResponseCode);
    String response = http.getString();
    Serial.println(response);
    http.end();
    Serial.println("✅ WHATSAPP NOTIFICATION SENT!");
    return true;
  } else {
    Serial.printf("WA Error code: %d\n", httpResponseCode);
    http.end();
    return false;
  }
}

// Reusable WhatsApp TEXT send (no camera photo) — used for the active-hours
// monitoring ON/OFF notices. Message must be assembled by the caller.
bool sendWhatsAppText(const String& message) {
  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setTimeout(20000);
  String postData = "sender=YOUR_WA_SENDER&number=YOUR_WA_GROUP&is_group_target=1&message=" + message + "&token_auth=YOUR_WA_TOKEN";

  int httpResponseCode = http.POST(postData);

  if (httpResponseCode > 0) {
    Serial.printf("WA text HTTP Response code: %d\n", httpResponseCode);
    String response = http.getString();
    if (response.length() > 200) response = response.substring(0, 200);
    Serial.println(response);
    http.end();
    Serial.println("✅ WHATSAPP TEXT NOTIFICATION SENT!");
    return true;
  } else {
    Serial.printf("WA text Error code: %d\n", httpResponseCode);
    http.end();
    return false;
  }
}

bool sendNotification() {
  Serial.print("notificationMethod = ");
  Serial.println(notificationMethod);
  if (notificationMethod == 1) {
    Serial.println("Sending via Telegram...");
    return sendTelegramNotification();
  }
  Serial.println("Sending via WhatsApp...");
  return sendWhatsAppNotification();
}

void processData(AsyncResult &aResult){
  if (!aResult.isResult())
    return;

  if (aResult.isEvent()){
    Firebase.printf("Event task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.eventLog().message().c_str(), aResult.eventLog().code());
  }

  if (aResult.isDebug()){
    Firebase.printf("Debug task: %s, msg: %s\n", aResult.uid().c_str(), aResult.debug().c_str());
  }

  if (aResult.isError()){
    Firebase.printf("Error task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
  }

if (aResult.available()){
    RealtimeDatabaseResult &RTDB = aResult.to<RealtimeDatabaseResult>();
    if (RTDB.isStream()) {
      Serial.println("----------------------------");
      Firebase.printf("task: %s\n", aResult.uid().c_str());
      Firebase.printf("event: %s\n", RTDB.event().c_str());
      Firebase.printf("path: '%s'\n", RTDB.dataPath().c_str());
      Firebase.printf("etag: %s\n", RTDB.ETag().c_str());
      Firebase.printf("data: %s\n", RTDB.to<const char *>());
      Firebase.printf("type: %d\n", RTDB.type());

      // Handle WiFi config changes
      String dataPath = RTDB.dataPath();
      Serial.printf(">>> Checking WiFi config path: '%s'\n", dataPath.c_str());
      if (dataPath.startsWith("/")) {
        handleWiFiConfigChange(dataPath, RTDB);
      }

      if (RTDB.type() == 6) {
        Serial.println(RTDB.to<String>());
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, RTDB.to<String>());
        if (error) {
          Serial.print("deserializeJson() failed: ");
          Serial.println(error.c_str());
        } else {
          for (JsonPair kv : doc.as<JsonObject>()) {
            int gpioPin = atoi(kv.key().c_str());
            if (gpioPin == 12) {
              sensorState = kv.value().as<bool>();
            } else if (gpioPin == 13) {
              startFromString2 = kv.value().as<String>();
            } else if (gpioPin == 14) {
              stopFromString2 = kv.value().as<String>();
            } else if (gpioPin == 16) {
              notificationMethod = kv.value().as<int>();
            } else if (gpioPin == 17) {
              int intervalMinutes = kv.value().as<int>();
              if (intervalMinutes > 0) {
                notificationInterval = (unsigned long)intervalMinutes * 60UL;
              }
            }
          }
        }
      }

      if (RTDB.type() == 4 || RTDB.type() == 1){
        int GPIO_number = RTDB.dataPath().substring(1).toInt();
        if (GPIO_number == 16) {
          notificationMethod = RTDB.to<int>();
          Serial.printf("notificationMethod updated to %d (int type)\n", notificationMethod);
        } else if (GPIO_number == 17) {
          int intervalMinutes = RTDB.to<int>();
          if (intervalMinutes > 0) {
            notificationInterval = (unsigned long)intervalMinutes * 60UL;
            Serial.printf("notificationInterval updated to %d s (int type)\n", notificationInterval);
          }
        } else if (GPIO_number != 15) { // Skip output pin 15
          sensorState = RTDB.to<bool>();
          Serial.println("Updating INT or BOOL GPIO State");
        }
      }
      if (RTDB.type() == 5){
        int GPIO_number = RTDB.dataPath().substring(1).toInt();
        Serial.println("GPIO_number");
        Serial.println(GPIO_number);
        if (GPIO_number == 15) {
          // Skip path 15 (output data from Arduino)
        } else if(GPIO_number == 13){
          startFromString2 = RTDB.to<String>();
        } else if(GPIO_number == 14){
          stopFromString2 = RTDB.to<String>();
        } else if(GPIO_number == 16){
          notificationMethod = RTDB.to<int>();
        } else if(GPIO_number == 17){
          int intervalMinutes = RTDB.to<int>();
          if (intervalMinutes > 0) {
            notificationInterval = (unsigned long)intervalMinutes * 60UL;
          }
        }
        Serial.println("Updating STRING GPIO State");
      }

      Serial.println("startFrom");
      Serial.println(startFromString2);
      Serial.println("stopFrom");
      Serial.println(stopFromString2);
    }
    else{
        Serial.println("----------------------------");
        Firebase.printf("task: %s, payload: %s\n", aResult.uid().c_str(), aResult.c_str());
    }
  }
}

void pollResultCallback(AsyncResult &aResult) {
  if (!aResult.isResult())
    return;

  if (aResult.isError()) {
    Firebase.printf("Poll task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
    return;
  }

  String uid = aResult.uid();
  RealtimeDatabaseResult &RTDB = aResult.to<RealtimeDatabaseResult>();

  if (uid == "scan_req") {
    bool v = RTDB.to<bool>();
    Serial.printf("poll scan_request = %d\n", v);
    if (v) pollScanRequest = true;
  } else if (uid == "conn_ssid") {
    connPollSSID = RTDB.to<String>();
    Serial.printf("poll connect_ssid = '%s'\n", connPollSSID.c_str());
  } else if (uid == "conn_pass") {
    connPollPass = RTDB.to<String>();
  } else if (uid == "conn_save") {
    connPollSave = RTDB.to<bool>();
  } else if (uid == "conn_req") {
    Serial.printf("poll connect_request = %d\n", RTDB.to<bool>());
    if (RTDB.to<bool>()) pollConnectRequest = true;
  } else if (uid == "restart_req") {
    Serial.printf("poll restart_request = %d\n", RTDB.to<bool>());
    if (RTDB.to<bool>()) pollRestartRequest = true;
  } else if (uid == "use_custom") {
    bool useCustom = RTDB.to<bool>();
    // First poll just records baseline; only react to real changes afterwards.
    if (firstUseCustomPoll) {
      lastPolledUseCustom = useCustom;
      firstUseCustomPoll = false;
      Serial.printf("use_custom baseline = %d (no trigger on first poll)\n", useCustom);
      return;
    }
    if (useCustom != lastPolledUseCustom) {
      lastPolledUseCustom = useCustom;
      if (useCustom) pollUseCustomChanged = true;
    }
  } else if (uid == "custom_ssid") {
    String v = RTDB.to<String>();
    if (firstCustomSsidPoll) {
      customWiFiSSID = v;
      firstCustomSsidPoll = false;
      Serial.printf("custom_ssid baseline = '%s' (no trigger)\n", v.c_str());
      return;
    }
    if (v != customWiFiSSID) {
      customWiFiSSID = v;
      pollCustomWiFiUpdated = true;
    }
  } else if (uid == "custom_pass") {
    String v = RTDB.to<String>();
    if (firstCustomPassPoll) {
      customWiFiPass = v;
      firstCustomPassPoll = false;
      Serial.printf("custom_pass baseline (no trigger)\n");
      return;
    }
    if (v != customWiFiPass) {
      customWiFiPass = v;
      pollCustomWiFiUpdated = true;
    }
  } else if (uid == "dig_12") {
    sensorState = RTDB.to<bool>();
    Serial.printf("poll digital/12 sensorState = %d\n", sensorState);
  } else if (uid == "dig_13") {
    startFromString2 = RTDB.to<String>();
    Serial.printf("poll digital/13 start = %s\n", startFromString2.c_str());
  } else if (uid == "dig_14") {
    stopFromString2 = RTDB.to<String>();
    Serial.printf("poll digital/14 stop = %s\n", stopFromString2.c_str());
  } else if (uid == "dig_16") {
    int method = RTDB.to<int>();
    Serial.printf("poll digital/16 method = %d\n", method);
    if (method >= 0 && method <= 1) notificationMethod = method;
  } else if (uid == "dig_17") {
    int intervalMinutes = RTDB.to<int>();
    Serial.printf("poll digital/17 interval = %d min\n", intervalMinutes);
    if (intervalMinutes > 0) {
      notificationInterval = (unsigned long)intervalMinutes * 60UL;
    }
  } else if (uid == "dig_22") {
    int minRange = RTDB.to<int>();
    Serial.printf("poll digital/22 movingMinRange = %d cm\n", minRange);
    if (minRange >= 0 && minRange < movingMaxRange) {
      movingMinRange = minRange;
    }
  } else if (uid == "dig_23") {
    int maxRange = RTDB.to<int>();
    Serial.printf("poll digital/23 movingMaxRange = %d cm\n", maxRange);
    if (maxRange > movingMinRange) {
      movingMaxRange = maxRange;
    }
  } else if (uid == "dig_24") {
    int tone = RTDB.to<int>();
    Serial.printf("poll digital/24 buzzerTone = %d\n", tone);
    if (tone >= 0 && tone <= 6) {
      buzzerTone = tone;
    }
  } else if (uid == "dig_25") {
    triggerMoving = RTDB.to<bool>();
    Serial.printf("poll digital/25 triggerMoving = %d\n", triggerMoving);
  } else if (uid == "dig_26") {
    triggerStationary = RTDB.to<bool>();
    Serial.printf("poll digital/26 triggerStationary = %d\n", triggerStationary);
  }
}

// Callback for the last_online heartbeat write. Confirms internet/Firebase
// connectivity so the watchdog never triggers while we are actually online.
void lastOnlineWriteCallback(AsyncResult &aResult) {
  if (aResult.isError()) {
    Serial.printf("⚠️  last_online write FAILED: %s (code %d)\n",
                  aResult.error().message().c_str(), aResult.error().code());
    return;
  }
  lastOnlineOkMs = millis();
  lastOnlineOkInit = true;
}

int getSecondTime(String timedb) {
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return 0;
  }

  int hour = timeinfo.tm_hour;
  int minute = timeinfo.tm_min;
  // Serial.println(hour);
  // Serial.println(minute);

  // Find the position of the colons

  startFromString = timedb;
  int firstColon = startFromString.indexOf(':');

  // Serial.println(startFrom);
  // Serial.println(startFromString);
  // Extract substrings for hours, minutes, and seconds
  hourString = startFromString.substring(0, firstColon);
  minuteString = startFromString.substring(firstColon + 1);
  // Convert the substrings to integers
  int hours = hourString.toInt();
  int minutes = minuteString.toInt();
  // int seconds = secondString.toInt();

  // Calculate the total seconds
  // Use 'L' suffix for unsigned long constants to avoid potential overflow issues during calculation
  // Serial.println(hours);
  // Serial.println(minutes);
  int totalSeconds = (hours * 3600UL) + (minutes * 60UL);
  // Serial.println("totalSeconds");
  // Serial.println(totalSeconds);

  return totalSeconds;
}

void handleWiFiConfigChange(String path, RealtimeDatabaseResult &RTDB) {
  Serial.printf("WiFi Config Change: path='%s', type=%d, data='%s', event='%s'\n", 
                path.c_str(), RTDB.type(), RTDB.to<const char *>(), RTDB.event().c_str());
  
  bool boolVal = false;
  if (RTDB.type() == 4) {
    boolVal = RTDB.to<bool>();
  } else if (RTDB.type() == 1) {
    boolVal = (RTDB.to<int>() != 0);
  }
  
  Serial.printf("Parsed boolVal: %s\n", boolVal ? "true" : "false");
  
  if (path == "/scan_request" && boolVal) {
    Serial.println(">>> SCAN TRIGGERED via stream <<<");
    scanAndSendResults();
  }
  
  if (path == "/connect_request" && boolVal) {
    Serial.println(">>> CONNECT TRIGGERED via stream <<<");
    processConnectRequest();
  }
  
  if (path == "/use_custom" && (RTDB.type() == 4 || RTDB.type() == 1)) {
    bool useCustom = RTDB.to<bool>();
    Serial.printf("Use custom WiFi changed to: %s\n", useCustom ? "true" : "false");
    if (useCustom) {
      // Reconnect with custom WiFi
      connectToWiFi();
    }
  }
  
  if (path == "/custom_ssid" && RTDB.type() == 5) {
    customWiFiSSID = RTDB.to<String>();
    Serial.printf("Custom SSID updated: %s\n", customWiFiSSID.c_str());
  }
  
  if (path == "/custom_password" && RTDB.type() == 5) {
    customWiFiPass = RTDB.to<String>();
    Serial.println("Custom password updated");
  }
}

void scanAndSendResults() {
  Serial.println("Scanning WiFi networks...");
  int n = WiFi.scanNetworks();
  Serial.printf("Found %d networks\n", n);

  if (n <= 0) {
    WiFi.scanDelete();
    Serial.println("No networks found");
    Database.set<int>(aClient, "board1/wifi_config/scan_count", 0);
    Database.set<String>(aClient, "board1/wifi_config/scan_time", String(millis()));
    Database.set<bool>(aClient, "board1/wifi_config/scan_request", false);
    return;
  }

  for (int wait = 0; wait < 50 && WiFi.status() != WL_CONNECTED; wait++) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected after scan, reconnecting...");
    WiFi.scanDelete();
    Database.set<String>(aClient, "board1/wifi_config/scan_time", String(millis()));
    Database.set<bool>(aClient, "board1/wifi_config/scan_request", false);
    connectToWiFi();
    return;
  }

  int maxNetworks = min(n, 5);
  Serial.printf("Sending %d networks to Firebase...\n", maxNetworks);

  // Build the network list as a single JSON object and write it in ONE
  // async request (instead of 15 blocking sync sets that froze the loop).
  StaticJsonDocument<1024> doc;
  JsonObject nets = doc.to<JsonObject>();
  for (int idx = 0; idx < maxNetworks; idx++) {
    String ssid = WiFi.SSID(idx);
    bool isDefault = false;
    for (int j = 0; j < DEFAULT_WIFI_COUNT; j++) {
      if (ssid.equalsIgnoreCase(DEFAULT_WIFI_SSID[j])) {
        isDefault = true;
        break;
      }
    }
    nets[String(idx)]["ssid"] = ssid;
    nets[String(idx)]["rssi"] = WiFi.RSSI(idx);
    nets[String(idx)]["def"] = isDefault;
    Serial.printf("Adding network %d: %s\n", idx, ssid.c_str());
  }
  String jsonStr;
  serializeJson(nets, jsonStr);

  WiFi.scanDelete();

  // Async writes (non-blocking). Order matters: networks/count first, then
  // scan_time (dashboard triggers on scan_time change), then reset request.
  Database.set<object_t>(aClient, "board1/wifi_config/networks", object_t(jsonStr), pollResultCallback, "scan_nets");
  Database.set<int>(aClient, "board1/wifi_config/scan_count", maxNetworks, pollResultCallback, "scan_count");
  Database.set<String>(aClient, "board1/wifi_config/scan_time", String(millis()), pollResultCallback, "scan_time");
  Database.set<bool>(aClient, "board1/wifi_config/scan_request", false, pollResultCallback, "scan_req_reset");
  Serial.println("Scan results queued to Firebase (async)");
}

bool connectToWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  String savedSSID = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");

  // Try custom WiFi from Firebase (if configured via Use Custom WiFi)
  if (customWiFiSSID.length() > 0) {
    Serial.printf("Trying custom Firebase SSID: %s\n", customWiFiSSID.c_str());
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(customWiFiSSID.c_str(), customWiFiPass.c_str());

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ Connected to custom Firebase SSID!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("SSID: ");
      Serial.println(customWiFiSSID);
      Serial.print("RSSI: ");
      Serial.println(WiFi.RSSI());
      return true;
    }
    Serial.println("\n❌ Custom SSID failed, trying saved/defaults...");
  }

  // Try last saved (latest) SSID first
  if (savedSSID.length() > 0) {
    Serial.printf("Trying saved SSID: %s\n", savedSSID.c_str());
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ Connected to saved SSID!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("SSID: ");
      Serial.println(savedSSID);
      Serial.print("RSSI: ");
      Serial.println(WiFi.RSSI());
      return true;
    }
    Serial.println("\n❌ Saved SSID failed, trying defaults...");
  }

  for (int i = 0; i < DEFAULT_WIFI_COUNT; i++) {
    Serial.printf("Trying: %s\n", DEFAULT_WIFI_SSID[i]);
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(DEFAULT_WIFI_SSID[i], DEFAULT_WIFI_PASS[i]);
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 30000) {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ Connected!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("SSID: ");
      Serial.println(DEFAULT_WIFI_SSID[i]);
      Serial.print("RSSI: ");
      Serial.println(WiFi.RSSI());
      
      return true;
    }
    Serial.println("\n❌ Failed");
  }
  
  Serial.println("❌ All WiFi attempts failed");
  return false;
}

bool scanAndConnectDefaultWiFi() {
  return connectToWiFi();
}

void processConnectRequest() {
  Serial.println(">>> PROCESS CONNECT <<<");
  String connSSID = connPollSSID;
  String connPass = connPollPass;
  bool connSave = connPollSave;
  
  Serial.printf("Connecting to SSID: %s\n", connSSID.c_str());
  
  if (connSSID.length() > 0) {
    WiFi.disconnect();
    delay(500);
    WiFi.begin(connSSID.c_str(), connPass.c_str());
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ Connected!");
      if (connSave) saveCustomWiFiToFirebase(connSSID, connPass);
      prefs.begin("wifi", false);
      prefs.putString("ssid", connSSID);
      prefs.putString("pass", connPass);
      prefs.end();
      Serial.println("Saved credentials to NVS (latest SSID)");
      Database.set<bool>(aClient, "board1/wifi_config/connect_result/success", true);
      Database.set<String>(aClient, "board1/wifi_config/last_connected", connSSID);
      Database.set<bool>(aClient, "board1/wifi_config/connected", true);
    } else {
      Serial.println("\n❌ Connection failed");
      Database.set<bool>(aClient, "board1/wifi_config/connect_result/success", false);
      Database.set<String>(aClient, "board1/wifi_config/connect_result/error", "Timeout");
      Database.set<bool>(aClient, "board1/wifi_config/connected", false);
    }
  } else {
    Database.set<bool>(aClient, "board1/wifi_config/connect_result/success", false);
    Database.set<String>(aClient, "board1/wifi_config/connect_result/error", "No SSID");
  }
  
  Database.set<bool>(aClient, "board1/wifi_config/connect_request", false);
  connPollSSID = "";
  connPollPass = "";
  connPollSave = false;
  Serial.println("connect_request reset to false");
}

void startWiFiConfigPortal() {
  Serial.println("WiFi connection failed. Retrying in 30 seconds...");
  Serial.println("Tip: Set WiFi credentials via Firebase at board1/wifi_config/, or restart the device.");
  for (int i = 30; i > 0; i--) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("\nRestarting...");
  ESP.restart();
}

void saveCustomWiFiToFirebase(String ssid, String password) {
  if (!app.ready()) return;
  
  Serial.printf("Saving custom WiFi to Firebase: %s\n", ssid.c_str());
  
  bool ok1 = Database.set<String>(aClient, WIFI_CUSTOM_SSID_PATH, ssid);
  bool ok2 = Database.set<String>(aClient, WIFI_CUSTOM_PASS_PATH, password);
  bool ok3 = Database.set<bool>(aClient, WIFI_USE_CUSTOM_PATH, true);
  
  if (ok1 && ok2 && ok3) {
    Serial.println("✅ Custom WiFi saved to Firebase");
  } else {
    Serial.println("❌ Failed to save custom WiFi to Firebase");
  }
}

void loadCustomWiFiFromFirebase() {
  if (!app.ready()) return;
  
  Database.get(aClient, WIFI_USE_CUSTOM_PATH);
  if (aClient.lastError().code() == 0) {
    bool useCustom = aClient.to<bool>();
    if (useCustom) {
      Database.get(aClient, WIFI_CUSTOM_SSID_PATH);
      if (aClient.lastError().code() == 0) {
        String ssid = aClient.to<String>();
        Serial.printf("Custom WiFi SSID from Firebase: %s\n", ssid.c_str());
      }
    }
  }
}

void checkWiFiConnection() {
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck < 30000) return;
  lastWifiCheck = millis();
  
  if (WiFi.status() == WL_CONNECTED) {
    // If connected, just update RSSI periodically
    if (app.ready()) {
      Database.set<int>(aClient, "board1/outputs/digital/18", WiFi.RSSI());
    }
    return;
  }
  
  // WiFi disconnected - try to reconnect
  Serial.println("⚠️ WiFi disconnected, reconnecting...");
  if (connectToWiFi()) {
    if (app.ready()) {
      Database.set<String>(aClient, "board1/wifi_config/last_connected", WiFi.SSID());
      Database.set<bool>(aClient, "board1/wifi_config/connected", true);
      Database.set<int>(aClient, "board1/outputs/digital/18", WiFi.RSSI());
      Serial.println("WiFi reconnected, status synced");
    }
  }
}