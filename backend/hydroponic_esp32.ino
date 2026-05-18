/*
 * =====================================================================
 *  Smart Hydroponic Farming System – ESP32 Autonomous Control Node
 *  Purpose : Read all sensors, control actuators AUTONOMOUSLY,
 *            POST full status JSON to Flask, receive manual overrides
 *
 *  Autonomous Rules:
 *    ✅ Pump     → ALWAYS ON (continuous circulation)
 *    ✅ Shed     → AUTO-CLOSE when sunlight > SHED_CLOSE_THRESHOLD %
 *                  AUTO-OPEN  when sunlight < SHED_OPEN_THRESHOLD  %
 *    ✅ Light    → AUTO-ON when sunlight < LIGHT_ON_THRESHOLD %
 *                  AUTO-OFF when sunlight >= LIGHT_OFF_THRESHOLD %
 *    ✅ Mist     → AUTO-ON when humidity < HUMIDITY_MIN %
 *                  AUTO-OFF when humidity >= HUMIDITY_MAX %
 *
 *  Manual overrides come from dashboard via /control endpoint.
 *
 *  ┌─────────────────────────────────────────────────────────────────┐
 *  │  RELAY DEBUG MODE                                               │
 *  │  Uncomment the #define below, flash, open Serial at 115200.    │
 *  │  Every GPIO pin is pulsed LOW/HIGH — watch relay LED blink.    │
 *  │  Re-comment and reflash when done.                             │
 *  └─────────────────────────────────────────────────────────────────┘
 */

// #define RELAY_TEST_ONLY_MODE   // ← UNCOMMENT TO HALT after raw GPIO test

#define STRINGIFY_(x) #x
#define STRINGIFY(x)  STRINGIFY_(x)

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>

// ─── OTA Libraries ────────────────────────────────────────────────
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>

struct ActuatorOverride;

// ─── WiFi Credentials ─────────────────────────────────────────────
const char* WIFI_SSID     = "Galaxy M3285D4";
const char* WIFI_PASSWORD = "987654321";

// ─── Flask Server ─────────────────────────────────────────────────
const char* FLASK_HOST       = "https://hydrophonics-server-1.onrender.com";  // ← your Flask server IP
const char* ENDPOINT_SENSOR  = "/sensor-data";
const char* ENDPOINT_CONTROL = "/control";

// ─── OTA Configuration ────────────────────────────────────────────
const char* OTA_HOSTNAME = "hydro-esp32";
const char* OTA_PASSWORD = "hydro1234";

// ─── Pin Definitions ──────────────────────────────────────────────
#define DHTPIN          4       // DHT22 data pin
#define DHTTYPE         DHT22

#define ONE_WIRE_BUS    5       // DS18B20 primary data pin
#define ONE_WIRE_BUS_FALLBACK 15 // Optional fallback DS18B20 data pin

#define PH_PIN          36      // pH sensor analog input (VN)
#define TDS_PIN         35      // TDS sensor analog input
#define TRIG_PIN        26      // Ultrasonic sensor Trigger pin
#define ECHO_PIN        18      // Ultrasonic sensor Echo pin
#define LDR_PIN         33      // LDR (sunlight) analog input

// ─── Relay Configuration ──────────────────────────────────────────
//
// HOW TO DETERMINE YOUR RELAY POLARITY:
//   1. Power up the ESP32 with Serial Monitor open.
//   2. Check if relays click/energize during boot before any command.
//      - If YES → relays are ACTIVE_LOW and GPIOs are floating LOW during boot.
//        Set RELAYS_ACTIVE_LOW true  and wire loads to NO (Normally Open) contact.
//      - If NO  → relays are ACTIVE_HIGH.
//        Set RELAYS_ACTIVE_LOW false and wire loads to NO contact.
//   3. Use the built-in relay test: open Serial Monitor and press 'T'.
//      Each relay should click ON then OFF. If ON/OFF is swapped, toggle this value.
//
// MOST common blue relay boards (SRD-05VDC-SL-C) = ACTIVE_LOW = true.
// "Relay always ON" = symptom of WRONG polarity or load wired to NC instead of NO.
//
#define RELAYS_ACTIVE_LOW true    // ← true = coil energizes on LOW signal (standard blue board)

// CONTACT WIRING:
//   false = load wired to NO (Normally Open)  → load OFF when relay de-energized (RECOMMENDED)
//   true  = load wired to NC (Normally Closed) → load ON  when relay de-energized (INVERT behaviour)
#define RELAY_CONTACT_NORMALLY_CLOSED false

#define PUMP_RELAY_PIN  13      // Pump relay
#define LIGHT_RELAY_PIN 25      // LED grow light relay (avoid ESP32 GPIO12 strapping pin)
#define MIST_RELAY_PIN  14      // Mist maker relay
#define SERVO_PIN       27      // Shed servo motor

// ─── Servo Control Configuration ─────────────────────────────────
// The dashboard stores shed positions as angles. The ESP32 maps them
// to servo pulses so the same profile can tune different hardware.
#define SERVO_MIN_US         1000
#define SERVO_MAX_US         2000
#define SERVO_STOP_US        1500  // stop / neutral pulse
#define SERVO_MOVE_MS         650   // run time for one direction change

int servoAngleToPulseUs(int angle) {
  angle = constrain(angle, 0, 180);
  return map(angle, 0, 180, SERVO_MIN_US, SERVO_MAX_US);
}

// ─── ADC Calibration ─────────────────────────────────────────────
#define ADC_RESOLUTION    4095.0f
#define VREF              3.3f
#define PH_SLOPE          3.5f
#define PH_INTERCEPT      0.0f
#define TDS_FACTOR        0.5f
// Temperature compensation coefficient for TDS/EC (per °C).
// Typical value ~0.02 (2% per °C) — used to compensate measured TDS to 25°C.
#define TDS_TEMP_COEFFICIENT 0.02f

// ─── DS18B20 Water Temperature Limits ────────────────────────────
#define WATER_TEMP_MIN_C     0.0f
#define WATER_TEMP_MAX_C     80.0f

// ─── Ultrasonic Tank Calibration ──────────────────────────────────
#define TANK_DEPTH_CM     20.0f // Distance in cm from sensor to water when empty
#define TANK_FULL_CM      5.0f  // Distance in cm from sensor to water when full
#define WATER_LEVEL_SAMPLES       3
#define ULTRASONIC_TIMEOUT_US     5000UL   // FIX: was 30000µs (30ms block). 5ms covers ~85cm — plenty for a tank.
#define ULTRASONIC_INTERPING_MS   40

// ─── DS18B20 Reliability ─────────────────────────────────────────
#define DS18B20_REPROBE_INTERVAL_MS      30000UL
#define DS18B20_REINIT_FAILURE_COUNT     5

// ═══════════════════════════════════════════════════════════════════
//  AUTONOMOUS THRESHOLDS – NOW DYNAMIC
//  Thresholds are no longer hardcoded here. They are stored in the
//  PlantConditions struct below and fetched from Flask /current-plant
//  every PLANT_FETCH_INTERVAL ms. Default values are set in the struct.
// ═══════════════════════════════════════════════════════════════════

// ─── Timing ───────────────────────────────────────────────────────
#define SENSOR_INTERVAL      2000    // ms between sensor reads
#define TELEMETRY_INTERVAL   2000    // ms between telemetry POSTs to Flask
#define CONTROL_INTERVAL      100    // ms between control polls — faster manual response
#define OVERRIDE_TIMEOUT_MS  120000  // ms manual override lasts (2 min)
#define PLANT_FETCH_INTERVAL 30000   // ms between /current-plant refreshes
#define KEEPALIVE_INTERVAL   300000  // ms between backend keepalive pings (5 min)

// ─── HTTP Configuration ────────────────────────────────────────────
// FIX: Keepalive pings every 5 min keep the Render server warm, so a 30s first-
// attempt timeout is never needed in practice and blocks the loop for up to 30s.
// 8s is generous for a warm server over WiFi. If the server is cold despite the
// keepalive, the retry will handle it with a further 4s window.
#define HTTP_TIMEOUT_MS          8000   // POST/GET timeout (1st attempt) — server kept warm by keepalive
#define HTTP_TIMEOUT_RETRY_MS    4000   // Retry timeout (fast fallback)
#define CONTROL_HTTP_TIMEOUT_MS  4000   // Control poll MUST be short — actuator response depends on it
#define HTTP_MAX_RETRIES            2   // 2 attempts: 8s + 4s = 12s worst case (was 30+10+10 = 50s)
#define HTTP_RETRY_DELAY_MS      300    // 300ms → 600ms backoff (was 500ms → 1s → 2s)

// ─── Objects ──────────────────────────────────────────────────────
DHT dht(DHTPIN, DHTTYPE);
OneWire oneWirePrimary(ONE_WIRE_BUS);
OneWire oneWireFallback(ONE_WIRE_BUS_FALLBACK);
DallasTemperature waterTempPrimary(&oneWirePrimary);
DallasTemperature waterTempFallback(&oneWireFallback);
DallasTemperature* waterTempSensor = nullptr;
Servo shedServo;

// Forward declarations needed by tasks and setup (defined later in the file)
void applyDesiredRelayOutputs();
void tickShed();
void controlPollTask(void* pvParameters);
void pollControlCommands(unsigned long now);  // FIX: was missing — caused undefined symbol in controlPollTask

// ─── FreeRTOS Handles & Mutex ─────────────────────────────────────
// FIX: The control task (Core 1) and main loop both use HTTP clients.
// httpControl is used ONLY by controlPollTask; httpTelemetry is used ONLY
// by the main loop (postSensorData, fetchPlantConditions, keepalive).
// They share separate WiFiClientSecure objects so there is NO cross-task
// conflict between the two HTTP objects. However, postSensorData,
// fetchPlantConditions, and sendKeepalivePing all share httpTelemetry —
// these are all called from the main loop (single thread), so no mutex needed.
//
// Relay GPIO is driven by relayControlTask (FreeRTOS, Core 1, max priority).
// It accesses only desiredRelayState and actuatorState (bool fields, atomic
// on Xtensa LX6 for single-writer/single-reader). No mutex needed.
TaskHandle_t relayTaskHandle   = NULL;
TaskHandle_t controlTaskHandle = NULL;

void relayControlTask(void* pvParameters) {
  const TickType_t period = pdMS_TO_TICKS(5);
  TickType_t lastWake      = xTaskGetTickCount();
  for (;;) {
    applyDesiredRelayOutputs();
    tickShed();
    vTaskDelayUntil(&lastWake, period);  // precise 5ms period, no drift
  }
}

void controlPollTask(void* pvParameters) {
  const TickType_t period = pdMS_TO_TICKS(CONTROL_INTERVAL);
  TickType_t lastWake      = xTaskGetTickCount();
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      pollControlCommands(millis());
    }
    vTaskDelayUntil(&lastWake, period);
  }
}

// ─── Dedicated HTTP clients ────────────────────────────────────────
// clientControl  / httpControl   → used ONLY by controlPollTask (Core 1)
// clientTelemetry/ httpTelemetry → used ONLY by main loop
WiFiClientSecure clientControl;
WiFiClientSecure clientTelemetry;
HTTPClient httpControl;
HTTPClient httpTelemetry;
bool cloudInit = false;

// ─── Dynamic Plant Thresholds (fetched from Flask) ────────────────
// Replaces all hardcoded threshold #defines. Updated every 30 s.
struct PlantConditions {
  float temperatureMin  =  18.0f;
  float temperatureMax  =  24.0f;
  float humidityMin     =  50.0f;
  float humidityMax     =  70.0f;
  float phMin           =   5.5f;
  float phMax           =   6.5f;
  float tdsMin          = 500.0f;
  float tdsMax          = 800.0f;
  int   lightOnThreshold    = 40;
  int   lightOffThreshold   = 55;
  int   shedCloseThreshold  = 70;
  int   shedOpenThreshold   = 50;
  int   shedClosedAngle     = 10;
  int   shedOpenAngle       = 170;
  char  plantName[32]       = "lettuce";
  bool  fetched             = false;
};
PlantConditions plantCond;

// ─── Sensor State ─────────────────────────────────────────────────
struct SensorData {
  float airTemperature;
  float airHumidity;
  float waterTemperature;
  float pH;
  float tds;
  int   waterLevel;   // 0–100 %
  int   sunlight;     // 0–100 %
};

// ─── Actuator State ───────────────────────────────────────────────
struct ActuatorState {
  bool pump;
  bool light;
  bool mist;
  bool shed;   // true = OPEN (plants exposed), false = CLOSED (plants covered)
};

struct RelayTargets {
  bool pump;
  bool light;
  bool mist;
  bool shed;
};

// ─── Override Tracking ────────────────────────────────────────────
struct ActuatorOverride {
  bool     active;
  unsigned long expiresAt;
};

SensorData    sensorData;
ActuatorState actuatorState = { false, false, false, false };
RelayTargets  desiredRelayState = { true, false, false, false };

ActuatorState relayAppliedState = { false, false, false, false };

// ─── millis() Timers (one per subsystem — fully non-blocking) ────
unsigned long sensorTimer    = 0;   // readAllSensors() + autonomous logic
unsigned long uploadTimer    = 0;   // postSensorData() to Flask
// controlTimer REMOVED: control polling is now handled by controlPollTask (FreeRTOS)
unsigned long lastManualControlMs = 0; // pause nonessential cloud work after a manual command
unsigned long reconnectTimer = 0;   // WiFi reconnect attempts
unsigned long plantTimer     = 0;   // fetchPlantConditions() refresh
unsigned long keepaliveTimer = 0;   // backend keepalive ping
unsigned long statusTimer    = 0;   // Serial status print
unsigned long ultrasonicTimer = 0;  // inter-ping delay (replaces vTaskDelay)

// ─── Ultrasonic non-blocking state machine ────────────────────────
// Takes one ping per loop pass; accumulates WATER_LEVEL_SAMPLES over time
int    ultrasonicSample   = 0;      // current sample index
float  ultrasonicSum      = 0.0f;   // running distance sum
int    ultrasonicValid    = 0;      // valid sample count
bool   ultrasonicPinging  = false;  // true while waiting for echo

bool wifiStarted = false;

// ─── Non-blocking Shed/Servo State Machine ────────────────────────
// FIX: setShed() previously had delay(SERVO_MOVE_MS=650ms) inside it.
// Called from the 5ms relay timer via applyDesiredRelayOutputs(), this
// blocked ALL actuator updates for 650ms on every shed position change.
// Solution: fire servo and return immediately; tickShed() stops it later.
enum ShedMoveState { SHED_IDLE, SHED_MOVING };
ShedMoveState shedMoveState  = SHED_IDLE;
unsigned long shedMoveStart  = 0;
bool          shedTargetOpen = false;

// ─── DS18B20 Async Conversion Flag ───────────────────────────────
// FIX: requestTemperatures() with default 12-bit resolution blocks ~750ms.
// Set async mode at init; request at end of each sensor cycle, read at start
// of the next cycle (2s later — conversion is done well within that window).
bool ds18b20ConversionPending = false;

ActuatorOverride pumpOverride  = { false, 0 };
ActuatorOverride lightOverride = { false, 0 };
ActuatorOverride mistOverride  = { false, 0 };
ActuatorOverride shedOverride  = { false, 0 };

bool systemAutoMode = true;

// ─── Reason strings ───────────────────────────────────────────────
char pumpReason[48]  = "Continuous — always on";
char lightReason[48] = "Initializing...";
char mistReason[48]  = "Initializing...";
char shedReason[64]  = "Initializing...";

bool ds18b20Found        = false;
int  ds18b20PinInUse     = ONE_WIRE_BUS;
int  ds18b20FailureStreak = 0;
unsigned long lastDs18b20Probe = 0;

// ─── Forward Declarations ─────────────────────────────────────────
int  httpGetWithRetry(HTTPClient& http, int maxRetries = HTTP_MAX_RETRIES, int firstTimeoutMs = HTTP_TIMEOUT_MS);
int  httpPostWithRetry(HTTPClient& http, const String& payload, int maxRetries = HTTP_MAX_RETRIES);
bool initializeDS18B20();
void initCloudClients();
bool connectWiFi();  // returns true when connected
int  relayPinLevelForLoadState(bool loadOn);
bool relayCoilEnergizedForLoadState(bool loadOn);
void runRelayDiagnosticTest();
void applyDesiredRelayOutputs();
void applyActuator(int pin, bool* state, bool value, const char* name, const char* reason);
void setShed(bool open, const char* reason);
void tickShed();          // FIX: non-blocking servo tick — call every loop from relay timer
// Named actuator helpers — always use these for relay control
void turnPumpOn(const char* reason);
void turnPumpOff(const char* reason);
void turnLightOn(const char* reason);
void turnLightOff(const char* reason);
void turnMistOn(const char* reason);
void turnMistOff(const char* reason);

// ─────────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────────
void setup() {

  Serial.begin(115200);
  delay(200);  // brief delay so Serial is ready before first print — unavoidable
  Serial.println("\n====================================================");
  Serial.println("[HYDRO] Smart Hydroponic System booting...");
  Serial.printf ("[RELAY] Polarity: ACTIVE_%s | Contact: %s\n",
                 RELAYS_ACTIVE_LOW ? "LOW" : "HIGH",
                 RELAY_CONTACT_NORMALLY_CLOSED ? "NC (inverted)" : "NO (normal)");
  Serial.println("====================================================");

  // ══════════════════════════════════════════════════════════════
  //  STEP 1 — RELAY PINS FIRST, before anything else.
  //  FIX: ESP32 GPIOs float during boot causing relay flicker.
  //  Drive all pins to safe OFF state IMMEDIATELY here.
  // ══════════════════════════════════════════════════════════════
  int offLevel = relayPinLevelForLoadState(false);  // compute OFF pin level
  pinMode(PUMP_RELAY_PIN,  OUTPUT); digitalWrite(PUMP_RELAY_PIN,  offLevel);
  pinMode(LIGHT_RELAY_PIN, OUTPUT); digitalWrite(LIGHT_RELAY_PIN, offLevel);
  pinMode(MIST_RELAY_PIN,  OUTPUT); digitalWrite(MIST_RELAY_PIN,  offLevel);
  Serial.printf("[RELAY] All pins driven to OFF level=%d at boot\n", offLevel);

  // ── LED ─────────────────────────────────────────────────────────
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // ── Servo / Shed (starts CLOSED for safety) ─────────────────────
  shedServo.attach(SERVO_PIN);
  shedServo.writeMicroseconds(SERVO_STOP_US);
  setShed(false, "Boot — closed");

  // ── Sensors init ────────────────────────────────────────────────
  dht.begin();
  ds18b20Found = initializeDS18B20();
  analogReadResolution(12);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // ══════════════════════════════════════════════════════════════
  //  RAW GPIO RELAY TEST — runs every boot before WiFi.
  //  Bypasses ALL abstraction. Watch Serial Monitor and listen
  //  for relay clicks to determine correct polarity and wiring.
  //
  //  WHAT TO LOOK FOR:
  //   - If relay clicks on LOW  → RELAYS_ACTIVE_LOW true  ✅ (current setting)
  //   - If relay clicks on HIGH → RELAYS_ACTIVE_LOW false  (change it)
  //   - If no click at all      → wrong GPIO pin / bad wire
  // ══════════════════════════════════════════════════════════════
  struct RawRelayTest { int pin; const char* name; };
  RawRelayTest rawTests[] = {
    { PUMP_RELAY_PIN,  "Pump  (GPIO " STRINGIFY(PUMP_RELAY_PIN)  ")" },
    { LIGHT_RELAY_PIN, "Light (GPIO " STRINGIFY(LIGHT_RELAY_PIN) ")" },
    { MIST_RELAY_PIN,  "Mist  (GPIO " STRINGIFY(MIST_RELAY_PIN)  ")" },
  };

  #ifdef RELAY_TEST_ONLY_MODE
  Serial.println("\n[RAW TEST] ====== RELAY GPIO DIAGNOSTIC ======");
  Serial.println("[RAW TEST] Each relay: LOW for 1.5s, then HIGH for 1.5s.");
  Serial.println("[RAW TEST] Listen for click. No abstraction layers.");

  for (auto& t : rawTests) {
    Serial.printf("\n[RAW TEST] %s — writing LOW  (should click if ACTIVE_LOW)...\n", t.name);
    digitalWrite(t.pin, LOW);
    delay(1500);
    Serial.printf("[RAW TEST] %s — writing HIGH (should click if ACTIVE_HIGH)...\n", t.name);
    digitalWrite(t.pin, HIGH);
    delay(1500);
  }

  // Restore OFF state after test
  for (auto& t : rawTests) digitalWrite(t.pin, relayPinLevelForLoadState(false));
  Serial.println("[RAW TEST] ====== TEST DONE — pins restored to OFF ======\n");

  Serial.println("[DIAG] RELAY_TEST_ONLY_MODE halting here. Re-comment and reflash.");
  while (true) { delay(1000); }
  #endif

  // ── Network (after relays are safe) ────────────────────────────
  connectWiFi();
  setupOTA();

  // ── Persistent HTTPS connections ───────────────────────────────
  initCloudClients();

  // ── Set desired boot state: pump ON, everything else OFF ───────
  turnPumpOn("Boot — continuous circulation");
  turnLightOff("Boot — awaiting auto logic");
  turnMistOff("Boot — awaiting auto logic");

  // CRITICAL FIX: actually write the GPIO pins now.
  // turnPump/Light/MistOn/Off only set desiredRelayState —
  // the GPIO doesn't get driven until applyDesiredRelayOutputs() runs.
  // Without this call, pins stay at offLevel until loop() starts,
  // which would delay the pump by hundreds of ms.
  applyDesiredRelayOutputs();

  // FIX: Start the high-priority relay task AFTER the boot GPIO state is set.
  // This task drives relays every 5ms independently of HTTP blocking calls.
  xTaskCreatePinnedToCore(
    relayControlTask,           // task function
    "RelayTask",                // debug name
    2048,                       // stack (relay task is tiny — no HTTP or JSON)
    NULL,                       // no args
    configMAX_PRIORITIES - 1,  // highest FreeRTOS priority → preempts HTTP stack
    &relayTaskHandle,           // handle (kept for future suspension if needed)
    1                           // Core 1 — same core as loop(), so it preempts HTTP
  );
  Serial.println("[RELAY] FreeRTOS relay task started (5ms, core 1, max priority)");

  xTaskCreatePinnedToCore(
    controlPollTask,            // task function
    "ControlTask",             // debug name
    4096,                       // HTTP + JSON needs a larger stack than relay task
    NULL,                       // no args
    configMAX_PRIORITIES - 2,   // below relay task, above loop work
    &controlTaskHandle,         // handle
    1                           // Core 1 — keeps control polling close to relay task
  );
  Serial.println("[CTRL] FreeRTOS control task started (polling dashboard independently)");

  // ── Stagger millis timers so all tasks don't fire at t=0 ───────
  unsigned long now = millis();
  sensorTimer    = now;
  uploadTimer    = now + 500;
  // controlTimer removed — control polling is handled by controlPollTask FreeRTOS task
  plantTimer     = now + 1500;
  keepaliveTimer = now + 2000;
  statusTimer    = now + 3000;

  Serial.println("[BOOT] Ready. Entering main loop.");
}


// ─────────────────────────────────────────────────────────────────
//  MAIN LOOP — millis() scheduler + FreeRTOS relay task
//
//  Relay GPIO is driven by relayControlTask (FreeRTOS, Core 1, max priority).
//  It runs every 5ms and preempts HTTP calls — actuators are NEVER blocked.
//
//  Main loop tasks (priority order):
//    1. refreshOverrideTimers — expire manual overrides
//    2. controlTimer (500 ms)  — receive dashboard commands
//    3. sensorTimer  (2 s)     — read sensors + run autonomous logic
//    4. uploadTimer  (2 s)     — POST data to Flask
//    5. plantTimer   (30 s)    — refresh plant thresholds
//    6. keepaliveTimer (5 min) — prevent Render cold-start
//    7. statusTimer  (10 s)    — Serial print
//    8. OTA          (always)  — wireless firmware updates
// ─────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── 1. RELAY / CONTROL TASKS — handled by FreeRTOS tasks on Core 1.
  //    The main loop now stays focused on sensor reads and background work.
  refreshOverrideTimers(now);

  // ── 2. SENSOR READ (2s) — DHT22, DS18B20, pH, TDS, LDR ───────────
  if (now - sensorTimer >= SENSOR_INTERVAL) {
    sensorTimer = now;
    readAllSensors();             // non-blocking analog reads + tickUltrasonic()
    runAutonomousControl(now);    // update desiredRelayState from sensor data
  }

  // ── 3. TELEMETRY UPLOAD (2s) — POST sensor+actuator JSON to Flask ───
  if (WiFi.status() == WL_CONNECTED) {
    if (now - uploadTimer >= TELEMETRY_INTERVAL && now - lastManualControlMs >= 1500UL) {
      uploadTimer = now;
      postSensorData();
    }
  }

  // ── 4. PLANT THRESHOLDS (30s) — fetch dynamic thresholds ──────────
  if (WiFi.status() == WL_CONNECTED) {
    if ((now - plantTimer >= PLANT_FETCH_INTERVAL || !plantCond.fetched) && now - lastManualControlMs >= 1500UL) {
      plantTimer = now;
      fetchPlantConditions();
    }
  }

  // ── 5. KEEPALIVE (5min) — prevent Render free-tier cold start ──────
  if (WiFi.status() == WL_CONNECTED) {
    if (now - keepaliveTimer >= KEEPALIVE_INTERVAL && now - lastManualControlMs >= 1500UL) {
      keepaliveTimer = now;
      sendKeepalivePing();
    }
  }

  // ── 6. SERIAL STATUS (10s) — debug print ────────────────────────
  if (now - statusTimer >= 10000UL) {
    statusTimer = now;
    printStatus();
  }

  // ── 7. WIFI RECONNECT (10s) — auto-heal lost connection ──────────
  if (WiFi.status() != WL_CONNECTED) {
    if (now - reconnectTimer >= 10000UL) {
      reconnectTimer = now;
      connectWiFi();  // uses WiFi.reconnect() for subsequent calls
    }
  }

  // ── OTA (always) ────────────────────────────────────────────────
  handleSerialDiagnostics();
  ArduinoOTA.handle();
}

// ─────────────────────────────────────────────────────────────────
//  AUTONOMOUS CONTROL LOGIC
// ─────────────────────────────────────────────────────────────────
void runAutonomousControl(unsigned long now) {
  if (!systemAutoMode) {
    snprintf(pumpReason,  sizeof(pumpReason),  "Manual mode active");
    snprintf(lightReason, sizeof(lightReason), "Manual mode active");
    snprintf(mistReason,  sizeof(mistReason),  "Manual mode active");
    snprintf(shedReason,  sizeof(shedReason),  "Manual mode active");
    return;
  }

  SensorData snapshot   = sensorData;
  PlantConditions plant = plantCond;

  if (!isOverrideActive(pumpOverride, now))  turnPumpOn("Continuous — always on");

  if (!isOverrideActive(shedOverride, now)) {
    if (!desiredRelayState.shed && snapshot.sunlight < plant.shedOpenThreshold) {
      desiredRelayState.shed = true;
      snprintf(shedReason, sizeof(shedReason), "Auto-opened: sun %d%%", snapshot.sunlight);
    } else if (desiredRelayState.shed && snapshot.sunlight >= plant.shedCloseThreshold) {
      desiredRelayState.shed = false;
      snprintf(shedReason, sizeof(shedReason), "Auto-closed: sun %d%%", snapshot.sunlight);
    }
  }

  if (!isOverrideActive(lightOverride, now)) {
    if (!desiredRelayState.light && snapshot.sunlight < plant.lightOnThreshold)       turnLightOn("Auto-on: low sunlight");
    else if (desiredRelayState.light && snapshot.sunlight >= plant.lightOffThreshold) turnLightOff("Auto-off: bright enough");
    else snprintf(lightReason, sizeof(lightReason), "Sun %d%% — light %s", snapshot.sunlight, desiredRelayState.light ? "ON" : "OFF");
  }

  if (!isOverrideActive(mistOverride, now)) {
    if (!desiredRelayState.mist && snapshot.airHumidity < plant.humidityMin)       turnMistOn("Auto-on: low humidity");
    else if (desiredRelayState.mist && snapshot.airHumidity >= plant.humidityMax)  turnMistOff("Auto-off: humidity ok");
    else snprintf(mistReason, sizeof(mistReason), "Hum %.0f%% — mist %s", snapshot.airHumidity, desiredRelayState.mist ? "ON" : "OFF");
  }
}

// ─────────────────────────────────────────────────────────────────
//  applyDesiredRelayOutputs — drives GPIO for all actuators.
//  Called every 5ms from loop() and immediately from setup()/commands.
//  Simple direct calls to applyActuator() — no structs, no batching.
// ─────────────────────────────────────────────────────────────────
void applyDesiredRelayOutputs() {
  applyActuator(PUMP_RELAY_PIN,  &actuatorState.pump,  desiredRelayState.pump,  "Pump",  pumpReason);
  applyActuator(LIGHT_RELAY_PIN, &actuatorState.light, desiredRelayState.light, "Light", lightReason);
  applyActuator(MIST_RELAY_PIN,  &actuatorState.mist,  desiredRelayState.mist,  "Mist",  mistReason);
  if (actuatorState.shed != desiredRelayState.shed) {
    setShed(desiredRelayState.shed, shedReason);
  }
}

void refreshOverrideTimers(unsigned long now) {
  isOverrideActive(pumpOverride,  now);
  isOverrideActive(lightOverride, now);
  isOverrideActive(mistOverride,  now);
  isOverrideActive(shedOverride,  now);
}

// ─────────────────────────────────────────────────────────────────
//  initCloudClients — set up persistent HTTPS connections once at boot
//  Reusing these objects eliminates the TLS handshake on every poll.
// ─────────────────────────────────────────────────────────────────
void initCloudClients() {
  clientControl.setInsecure();
  clientTelemetry.setInsecure();
  httpControl.setReuse(true);
  httpTelemetry.setReuse(true);
}


// ─────────────────────────────────────────────────────────────────
//  WiFi
// ─────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────
//  WiFi — first connect (called once from setup, blocks until connected or timeout)
// ─────────────────────────────────────────────────────────────────
bool connectWiFi() {
  if (wifiStarted) {
    // FIX: For reconnects, use reconnect() not begin() again.
    // Calling begin() repeatedly can corrupt the internal state machine.
    Serial.println("[WIFI] Reconnecting...");
    WiFi.reconnect();
    return false;  // non-blocking reconnect — result checked in loop()
  }

  // ── FIRST CONNECT — blocking with timeout (called only once from setup) ──
  Serial.printf("[WIFI] Connecting to '%s'...", WIFI_SSID);
  wifiStarted = true;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // disable power-save for stable RSSI
  WiFi.setAutoReconnect(true);   // let esp-idf reconnect automatically
  WiFi.persistent(false);        // don't write credentials to flash
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Wait up to 15 seconds — print a dot every 500ms so Serial Monitor shows activity
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected!  IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  } else {
    Serial.printf("[WIFI] Could not connect to '%s' within 15s.\n", WIFI_SSID);
    Serial.println("[WIFI] Check SSID/password and hotspot visibility.");
    Serial.println("[WIFI] Loop will retry every 10s automatically.");
    return false;
  }
}

// ─────────────────────────────────────────────────────────────────
//  OTA Setup
// ─────────────────────────────────────────────────────────────────
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("[OTA] Starting update of " + type);
    digitalWrite(LED_BUILTIN, HIGH);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Update complete! Rebooting…");
    digitalWrite(LED_BUILTIN, LOW);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int pct = progress / (total / 100);
    Serial.printf("[OTA] Progress: %u%%\r", pct);
    digitalWrite(LED_BUILTIN, (pct % 2 == 0) ? HIGH : LOW);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] ERROR[%u]: ", error);
    if      (error == OTA_AUTH_ERROR)    Serial.println("Auth failed");
    else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive failed");
    else if (error == OTA_END_ERROR)     Serial.println("End failed");
    digitalWrite(LED_BUILTIN, LOW);
  });

  ArduinoOTA.begin();
  Serial.println("[OTA] Ready!");
  Serial.printf("[OTA] Hostname: %s.local\n", OTA_HOSTNAME);
  Serial.printf("[OTA] IP      : %s\n", WiFi.localIP().toString().c_str());
}

// ─────────────────────────────────────────────────────────────────
//  Fetch Dynamic Plant Thresholds from Flask
//  Called on boot and every PLANT_FETCH_INTERVAL ms.
// ─────────────────────────────────────────────────────────────────
void fetchPlantConditions() {
  if (WiFi.status() != WL_CONNECTED) return;
  // FIX: reuse persistent clientTelemetry — creating a local WiFiClientSecure
  // here caused a full TLS handshake every 30 seconds unnecessarily.
  String url = String(FLASK_HOST) + "/current-plant";
  httpTelemetry.begin(clientTelemetry, url);
  int httpCode = httpGetWithRetry(httpTelemetry, 2);  // 2 retries for plant fetch

  if (httpCode == HTTP_CODE_OK) {
    String body = httpTelemetry.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    JsonObject cond = doc["conditions"].as<JsonObject>();
    if (!err && !cond.isNull()) {
      if (!cond["humidity_min"].isNull())    plantCond.humidityMin    = cond["humidity_min"].as<float>();
      if (!cond["humidity_max"].isNull())    plantCond.humidityMax    = cond["humidity_max"].as<float>();
      if (!cond["temperature_min"].isNull()) plantCond.temperatureMin = cond["temperature_min"].as<float>();
      if (!cond["temperature_max"].isNull()) plantCond.temperatureMax = cond["temperature_max"].as<float>();
      if (!cond["ph_min"].isNull())          plantCond.phMin          = cond["ph_min"].as<float>();
      if (!cond["ph_max"].isNull())          plantCond.phMax          = cond["ph_max"].as<float>();
      if (!cond["tds_min"].isNull())         plantCond.tdsMin         = cond["tds_min"].as<float>();
      if (!cond["tds_max"].isNull())         plantCond.tdsMax         = cond["tds_max"].as<float>();
      if (!cond["light_on_threshold"].isNull())   plantCond.lightOnThreshold  = cond["light_on_threshold"].as<int>();
      if (!cond["light_off_threshold"].isNull())  plantCond.lightOffThreshold = cond["light_off_threshold"].as<int>();
      if (!cond["shed_close_threshold"].isNull()) plantCond.shedCloseThreshold = cond["shed_close_threshold"].as<int>();
      if (!cond["shed_open_threshold"].isNull())  plantCond.shedOpenThreshold  = cond["shed_open_threshold"].as<int>();
      if (!cond["shed_closed_angle"].isNull())    plantCond.shedClosedAngle    = cond["shed_closed_angle"].as<int>();
      if (!cond["shed_open_angle"].isNull())      plantCond.shedOpenAngle      = cond["shed_open_angle"].as<int>();

      // Store plant name for Serial logging
      const char* name = doc["plant"] | "unknown";
      strncpy(plantCond.plantName, name, sizeof(plantCond.plantName) - 1);
      plantCond.plantName[sizeof(plantCond.plantName) - 1] = '\0';

      plantCond.fetched = true;
      Serial.printf("[PLANT] Thresholds updated for '%s'\n", plantCond.plantName);
    } else {
      Serial.println("[PLANT] JSON parse error");
    }
  } else {
    Serial.println("[PLANT] Fetch failed – using defaults");
  }
  httpTelemetry.end();
}

bool initializeDS18B20() {
  waterTempPrimary.begin();
  int primaryCount = waterTempPrimary.getDeviceCount();
  if (primaryCount > 0) {
    waterTempSensor = &waterTempPrimary;
    ds18b20PinInUse = ONE_WIRE_BUS;
    ds18b20FailureStreak = 0;
    lastDs18b20Probe = millis();
    // FIX: Use async (non-blocking) conversion mode.
    // requestTemperatures() normally BLOCKS ~750ms waiting for 12-bit ADC.
    // setWaitForConversion(false) makes it return immediately; we read the
    // result on the next sensor cycle (2s later — conversion finishes in ~375ms).
    // 11-bit resolution (0.125°C) is more than adequate for hydroponics.
    waterTempSensor->setWaitForConversion(false);
    waterTempSensor->setResolution(11);
    Serial.printf("[SENSOR] DS18B20 detected on GPIO %d (count=%d, async 11-bit)\n", ds18b20PinInUse, primaryCount);
    return true;
  }

  waterTempFallback.begin();
  int fallbackCount = waterTempFallback.getDeviceCount();
  if (fallbackCount > 0) {
    waterTempSensor = &waterTempFallback;
    ds18b20PinInUse = ONE_WIRE_BUS_FALLBACK;
    ds18b20FailureStreak = 0;
    lastDs18b20Probe = millis();
    waterTempSensor->setWaitForConversion(false);
    waterTempSensor->setResolution(11);
    Serial.printf("[SENSOR] DS18B20 detected on fallback GPIO %d (count=%d, async 11-bit)\n", ds18b20PinInUse, fallbackCount);
    return true;
  }

  waterTempSensor = nullptr;
  lastDs18b20Probe = millis();
  Serial.printf("[SENSOR] DS18B20 NOT detected on GPIO %d or fallback GPIO %d. Check DATA pin + 4.7k pull-up to 3.3V.\n",
                ONE_WIRE_BUS, ONE_WIRE_BUS_FALLBACK);
  return false;
}

// ─────────────────────────────────────────────────────────────────
//  Read All Sensors
// ─────────────────────────────────────────────────────────────────
void readAllSensors() {
  // ── DHT22 – Air Temperature & Humidity ───────────────────────
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    sensorData.airTemperature = t;
    sensorData.airHumidity    = h;
  } else {
    Serial.println("[SENSOR] DHT22 read failed – using last value");
  }

  // ── DS18B20 Water Temperature ─────────────────────────────────
  // FIX: async pattern — read FIRST (result ready from last cycle's request),
  // then request the NEXT conversion at the bottom of this function.
  // This eliminates the ~750ms blocking wait entirely.
  if (!ds18b20Found || waterTempSensor == nullptr) {
    if (millis() - lastDs18b20Probe >= DS18B20_REPROBE_INTERVAL_MS) {
      ds18b20Found = initializeDS18B20();
      ds18b20ConversionPending = false;
    }
  } else if (ds18b20ConversionPending) {
    float wtC = waterTempSensor->getTempCByIndex(0);
    if (wtC != DEVICE_DISCONNECTED_C && wtC > -100.0f && wtC < 125.0f && wtC >= WATER_TEMP_MIN_C && wtC <= WATER_TEMP_MAX_C) {
      ds18b20FailureStreak = 0;
      sensorData.waterTemperature = wtC;
    } else {
      ds18b20FailureStreak++;
      Serial.printf("[SENSOR] DS18B20 read failed on GPIO %d: %.2fC (streak=%d, keeping last %.2fC)\n",
                    ds18b20PinInUse, wtC, ds18b20FailureStreak, sensorData.waterTemperature);
      if (ds18b20FailureStreak >= DS18B20_REINIT_FAILURE_COUNT) {
        ds18b20FailureStreak = 0;
        ds18b20Found = initializeDS18B20();
        ds18b20ConversionPending = false;
      }
    }
  }

  // ── pH Sensor ────────────────────────────────────────────────
  int   phRaw  = analogRead(PH_PIN);
  float phVolt = (phRaw / ADC_RESOLUTION) * VREF;
  float phValue = constrain((-5.70f * phVolt) + 21.34f, 0.0f, 14.0f);

  // ── TDS Sensor ────────────────────────────────────────────────
  int   tdsRaw  = analogRead(TDS_PIN);
  float tdsVolt = (tdsRaw / ADC_RESOLUTION) * VREF;
  // Raw TDS calculation from sensor voltage (empirical polynomial)
  float tdsRawValue = (133.42f * pow(tdsVolt, 3) - 255.86f * pow(tdsVolt, 2) + 857.39f * tdsVolt) * TDS_FACTOR;
  // Apply temperature compensation to convert measured TDS to value at 25°C.
  float tempForComp = sensorData.waterTemperature;
  // If water temperature is out of expected range or not available, assume 25°C (no compensation).
  if (!(tempForComp >= WATER_TEMP_MIN_C && tempForComp <= WATER_TEMP_MAX_C)) {
    tempForComp = 25.0f;
  }
  float compensationFactor = 1.0f + TDS_TEMP_COEFFICIENT * (tempForComp - 25.0f);
  // Avoid division by zero or negative compensation
  if (compensationFactor <= 0.0f) compensationFactor = 1.0f;
  float tdsCompensated = tdsRawValue / compensationFactor;
  float tdsValue = max(0.0f, tdsCompensated);

  // ── Water Level (Ultrasonic) ─────────────────────────────────
  // [NON-BLOCKING] One ping per sensor cycle (2s interval is plenty).
  // delayMicroseconds(2/10) are hardware-required sub-ms pulses — unavoidable.
  // The old inter-ping vTaskDelay(40ms) is gone.
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT_US);
  if (duration > 0 && TANK_DEPTH_CM > TANK_FULL_CM) {
    float distanceCm = (duration * 0.0343f) / 2.0f;
    if (distanceCm > 0.5f && distanceCm < 400.0f) {
      float fillPct = ((TANK_DEPTH_CM - distanceCm) / (TANK_DEPTH_CM - TANK_FULL_CM)) * 100.0f;
      sensorData.waterLevel = constrain((int)roundf(fillPct), 0, 100);
    }
  }

  // ── Sunlight / LDR ───────────────────────────────────────────
  int ldrRaw = analogRead(LDR_PIN);
  int sunlightValue = constrain(map(ldrRaw, 4095, 0, 0, 100), 0, 100);

  // Single-loop: no mutex needed — write directly
  sensorData.pH      = phValue;
  sensorData.tds      = tdsValue;
  sensorData.sunlight = sunlightValue;

  // FIX: Issue DS18B20 conversion request NOW — it runs asynchronously in the
  // background. Result will be read at the TOP of the NEXT sensor cycle (2s later).
  // With setWaitForConversion(false) this returns immediately (no ~750ms block).
  if (ds18b20Found && waterTempSensor != nullptr) {
    waterTempSensor->requestTemperatures();
    ds18b20ConversionPending = true;
  }
}

// ─────────────────────────────────────────────────────────────────
//  Print Status to Serial
// ─────────────────────────────────────────────────────────────────
void printStatus() {
  Serial.println("════════════════════════════════════════");
  Serial.printf("[SENSOR]   Air Temp    : %.2f C\n",  sensorData.airTemperature);
  Serial.printf("[SENSOR]   Humidity    : %.2f %%\n", sensorData.airHumidity);
  Serial.printf("[SENSOR]   Water Temp  : %.2f C\n",  sensorData.waterTemperature);
  Serial.printf("[SENSOR]   pH          : %.2f\n",    sensorData.pH);
  Serial.printf("[SENSOR]   TDS         : %.1f ppm\n",sensorData.tds);
  Serial.printf("[SENSOR]   Water Level : %d %%\n",   sensorData.waterLevel);
  Serial.printf("[SENSOR]   Sunlight    : %d %%\n",   sensorData.sunlight);
  Serial.println("────────────────────────────────────────");
  Serial.printf("[ACTUATOR] Pump        : %s  -> %s\n", actuatorState.pump  ? "ON" : "OFF", pumpReason);
  Serial.printf("[ACTUATOR] Light       : %s  -> %s\n", actuatorState.light ? "ON" : "OFF", lightReason);
  Serial.printf("[ACTUATOR] Mist        : %s  -> %s\n", actuatorState.mist  ? "ON" : "OFF", mistReason);
  Serial.printf("[ACTUATOR] Shed        : %s  -> %s\n", actuatorState.shed  ? "OPEN" : "CLOSED", shedReason);
  Serial.println("════════════════════════════════════════");
}


// ─────────────────────────────────────────────────────────────────
//  HTTP Retry Helper (with exponential backoff & adaptive timeouts)
// ─────────────────────────────────────────────────────────────────
int httpPostWithRetry(HTTPClient& http, const String& payload, int maxRetries) {
  for (int attempt = 0; attempt < maxRetries; attempt++) {
    // Use longer timeout on first attempt (cold start), shorter on retries
    int timeout = (attempt == 0) ? HTTP_TIMEOUT_MS : HTTP_TIMEOUT_RETRY_MS;
    http.setTimeout(timeout);
    
    int httpCode = http.POST(payload);
    if (httpCode == HTTP_CODE_OK) {
      if (attempt > 0) {
        Serial.printf("[HTTP] POST succeeded on attempt %d\n", attempt + 1);
      } else {
        Serial.println("[HTTP] POST OK");
      }
      return httpCode;
    }
    
    if (attempt < maxRetries - 1) {
      // FIX: replaced delay(delayMs) with a relay-safe yield loop.
      // delay() blocked ALL actuator updates during HTTP retry backoff.
      // Now we service relays every 5ms while waiting between retries.
      unsigned long delayMs = HTTP_RETRY_DELAY_MS * (1UL << attempt);
      Serial.printf("[HTTP] POST Attempt %d failed (code=%d, timeout=%dms) – retrying in %lu ms...\n", 
                    attempt + 1, httpCode, timeout, delayMs);
      unsigned long retryStart = millis();
      while (millis() - retryStart < delayMs) {
        applyDesiredRelayOutputs();   // keep relay GPIOs driven during wait
        tickShed();
        yield();                      // feed WDT and let WiFi stack run
      }
    } else {
      Serial.printf("[HTTP] POST failed (%d) after %d attempts\n", httpCode, maxRetries);
    }
  }
  return -1;  // All retries exhausted
}

int httpGetWithRetry(HTTPClient& http, int maxRetries, int firstTimeoutMs) {
  for (int attempt = 0; attempt < maxRetries; attempt++) {
    // FIX: use caller-supplied firstTimeoutMs so pollControlCommands() can pass
    // CONTROL_HTTP_TIMEOUT_MS (4s) instead of the general HTTP_TIMEOUT_MS (8s).
    int timeout = (attempt == 0) ? firstTimeoutMs : HTTP_TIMEOUT_RETRY_MS;
    http.setTimeout(timeout);
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      if (attempt > 0) {
        Serial.printf("[HTTP] GET succeeded on attempt %d\n", attempt + 1);
      } else {
        Serial.println("[HTTP] GET OK");
      }
      return httpCode;
    }
    
    if (attempt < maxRetries - 1) {
      // FIX: replaced delay(delayMs) with a relay-safe yield loop (same as POST helper).
      unsigned long delayMs = HTTP_RETRY_DELAY_MS * (1UL << attempt);
      Serial.printf("[HTTP] GET Attempt %d failed (code=%d, timeout=%dms) – retrying in %lu ms...\n", 
                    attempt + 1, httpCode, timeout, delayMs);
      unsigned long retryStart = millis();
      while (millis() - retryStart < delayMs) {
        applyDesiredRelayOutputs();
        tickShed();
        yield();
      }
    } else {
      Serial.printf("[HTTP] GET failed (%d) after %d attempts\n", httpCode, maxRetries);
    }
  }
  return -1;  // All retries exhausted
}

// ─────────────────────────────────────────────────────────────────
//  Keepalive Ping (prevents Render free tier cold start)
// ─────────────────────────────────────────────────────────────────
void sendKeepalivePing() {
  if (WiFi.status() != WL_CONNECTED) return;
  // cloudInit handled once at boot in initCloudClients()
  String url = String(FLASK_HOST) + "/health";
  httpTelemetry.begin(clientTelemetry, url);
  httpTelemetry.setTimeout(HTTP_TIMEOUT_RETRY_MS);

  int httpCode = httpTelemetry.GET();
  if (httpCode == HTTP_CODE_OK) {
    Serial.println("[KEEPALIVE] Backend ping OK – staying warm");
  } else {
    Serial.printf("[KEEPALIVE] Ping failed (%d) – backend may be cold on next request\n", httpCode);
  }
  httpTelemetry.end();
}

// ─────────────────────────────────────────────────────────────────
//  POST Sensor + Actuator Data to Flask
// ─────────────────────────────────────────────────────────────────
void postSensorData() {
  if (WiFi.status() != WL_CONNECTED) return;
  // cloudInit handled once at boot in initCloudClients()
  String url = String(FLASK_HOST) + ENDPOINT_SENSOR;
  httpTelemetry.begin(clientTelemetry, url);
  httpTelemetry.addHeader("Content-Type", "application/json");
  httpTelemetry.addHeader("Connection", "keep-alive");
  httpTelemetry.setTimeout(HTTP_TIMEOUT_MS);

  JsonDocument doc;
  // Single-loop: no mutex needed
  doc["air_temperature"]   = round2(sensorData.airTemperature);
  doc["humidity"]          = round2(sensorData.airHumidity);
  doc["water_temperature"] = round2(sensorData.waterTemperature);
  doc["ph"]                = round2(sensorData.pH);
  doc["tds"]               = round1(sensorData.tds);
  doc["water_level"]       = sensorData.waterLevel;
  doc["sunlight"]          = sensorData.sunlight;
  // Actuator states
  doc["pump"]  = actuatorState.pump;
  doc["light"] = actuatorState.light;
  doc["mist"]  = actuatorState.mist;
  doc["shed"]  = actuatorState.shed;
  // Autonomous reason strings (shown on dashboard)
  doc["pump_reason"]  = pumpReason;
  doc["light_reason"] = lightReason;
  doc["mist_reason"]  = mistReason;
  doc["shed_reason"]  = shedReason;
  // Thresholds (so dashboard can display them)
  doc["shed_close_threshold"] = plantCond.shedCloseThreshold;
  doc["light_on_threshold"]   = plantCond.lightOnThreshold;
  doc["humidity_min"]         = plantCond.humidityMin;
  doc["humidity_max"]         = plantCond.humidityMax;
  // No stateUnlock() needed — single-loop, no mutex

  String payload;
  serializeJson(doc, payload);

  int httpCode = httpPostWithRetry(httpTelemetry, payload, HTTP_MAX_RETRIES);
  if (httpCode == HTTP_CODE_OK) {
    Serial.printf("[HTTP] Sensor POST OK\n");
  } else {
    Serial.printf("[HTTP] Sensor POST failed (%d)\n", httpCode);
  }
  httpTelemetry.end();
}

// ─────────────────────────────────────────────────────────────────
//  Poll Dashboard Manual Overrides (HIGH PRIORITY – retry 3 times)
//  Dashboard can still send commands; they last OVERRIDE_TIMEOUT_MS
//  then autonomous control resumes.
// ─────────────────────────────────────────────────────────────────
void pollControlCommands(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String(FLASK_HOST) + ENDPOINT_CONTROL + "?source=esp32";
  Serial.println("[CTRL] Polling dashboard commands...");
  httpControl.begin(clientControl, url);
  httpControl.addHeader("Connection", "keep-alive");
  
  // FIX: pass CONTROL_HTTP_TIMEOUT_MS (4s) — this endpoint must return fast so
  // manual dashboard commands reach the actuators without multi-second lag.
  // (Previously used HTTP_TIMEOUT_MS=30s which could freeze actuators for 30s.)
  int httpCode = httpGetWithRetry(httpControl, HTTP_MAX_RETRIES, CONTROL_HTTP_TIMEOUT_MS);
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = httpControl.getString();
    Serial.printf("[CTRL] Poll response: %u bytes\n", payload.length());
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print(F("[CLOUD] JSON Error: "));
      Serial.println(error.f_str());
      return;
    }

    bool anyCommand = false;
    int appliedCount = 0;

    if (doc["auto_mode"].is<bool>()) {
      bool newMode = doc["auto_mode"].as<bool>();
      if (newMode != systemAutoMode) {
        systemAutoMode = newMode;
        Serial.printf("[CTRL] System mode -> %s\n", systemAutoMode ? "AUTO" : "MANUAL");
        // FIX: Clear all manual overrides when switching TO auto mode.
        // Without this, overrides set during manual mode keep isOverrideActive()
        // returning true for up to OVERRIDE_TIMEOUT_MS (2 min), blocking auto logic.
        if (systemAutoMode) {
          pumpOverride  = { false, 0 };
          lightOverride = { false, 0 };
          mistOverride  = { false, 0 };
          shedOverride  = { false, 0 };
          Serial.println("[CTRL] All manual overrides cleared — autonomous control resumed immediately.");
        }
      }
    }

    // FIX: each named helper sets the desiredRelayState AND reason string cleanly
    if (doc["pump"].is<bool>()) {
      bool v = doc["pump"].as<bool>();
      pumpOverride = { true, now + OVERRIDE_TIMEOUT_MS };
      if (v) turnPumpOn("Manual override"); else turnPumpOff("Manual override");
      anyCommand = true;
      appliedCount++;
    }
    if (doc["light"].is<bool>()) {
      bool v = doc["light"].as<bool>();
      lightOverride = { true, now + OVERRIDE_TIMEOUT_MS };
      if (v) turnLightOn("Manual override"); else turnLightOff("Manual override");
      anyCommand = true;
      appliedCount++;
    }
    if (doc["mist"].is<bool>()) {
      bool v = doc["mist"].as<bool>();
      mistOverride = { true, now + OVERRIDE_TIMEOUT_MS };
      if (v) turnMistOn("Manual override"); else turnMistOff("Manual override");
      anyCommand = true;
      appliedCount++;
    }
    if (doc["shed"].is<bool>()) {
      bool v = doc["shed"].as<bool>();
      shedOverride = { true, now + OVERRIDE_TIMEOUT_MS };
      desiredRelayState.shed = v;
      snprintf(shedReason, sizeof(shedReason), "Manual override");
      anyCommand = true;
      appliedCount++;
    }

    if (anyCommand) {
      applyDesiredRelayOutputs();  // apply immediately; relay timer also re-applies every 5ms
      Serial.printf("[CTRL] Applied %d command(s) from dashboard\n", appliedCount);
      lastManualControlMs = now;
    } else {
      Serial.println("[CTRL] No actuator commands in poll response");
    }
  } else {
    Serial.printf("[CTRL] Poll failed with HTTP %d\n", httpCode);
  }
  httpControl.end();
}

// ─────────────────────────────────────────────────────────────────
//  Override Helper
// ─────────────────────────────────────────────────────────────────
bool isOverrideActive(ActuatorOverride& ov, unsigned long now) {
  if (!ov.active) return false;
  if (now >= ov.expiresAt) {
    ov.active = false;   // Override expired – autonomous control resumes
    Serial.println("[AUTO] Override expired – resuming autonomous control.");
    return false;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────
//  Actuator Helpers
// ─────────────────────────────────────────────────────────────────
void applyActuator(int pin, bool* state, bool value, const char* name, const char* reason) {
  if (*state == value) {
    return;  // FIX: removed Serial.printf here — called every 5ms × 3 relays = 600 printf/s → UART stall
  }
  *state = value;
  
  int desiredPin = relayPinLevelForLoadState(value);
  digitalWrite(pin, desiredPin);
  
  // Non-blocking verification readback.
  int readPin = digitalRead(pin);
  int expectedPin = desiredPin;
  bool coilEnergized = relayCoilEnergizedForLoadState(value);
  
  if (readPin == expectedPin) {
    Serial.printf("[ACTUATOR] %-6s → %s  (%s) [OK pin=%d coil=%s contact=%s]\n",
                  name,
                  value ? "ON" : "OFF",
                  reason,
                  readPin,
                  coilEnergized ? "ENERGIZED" : "DEENERGIZED",
                  RELAY_CONTACT_NORMALLY_CLOSED ? "NC" : "NO");
  } else {
    Serial.printf("[ACTUATOR] %-6s → %s  (%s) [WARN pin=%d expected=%d coil=%s contact=%s]\n",
                  name,
                  value ? "ON" : "OFF",
                  reason,
                  readPin,
                  expectedPin,
                  coilEnergized ? "ENERGIZED" : "DEENERGIZED",
                  RELAY_CONTACT_NORMALLY_CLOSED ? "NC" : "NO");
  }
}

bool relayCoilEnergizedForLoadState(bool loadOn) {
  return RELAY_CONTACT_NORMALLY_CLOSED ? !loadOn : loadOn;
}

int relayPinLevelForLoadState(bool loadOn) {
  bool coilEnergized = relayCoilEnergizedForLoadState(loadOn);
  if (RELAYS_ACTIVE_LOW) {
    return coilEnergized ? LOW : HIGH;
  }
  return coilEnergized ? HIGH : LOW;
}

void runRelayDiagnosticTest() {
  Serial.println("\n[DIAG] Relay test started.");
  Serial.printf("[DIAG] Polarity: RELAYS_ACTIVE_LOW=%s | Contact: %s\n",
                RELAYS_ACTIVE_LOW ? "true" : "false",
                RELAY_CONTACT_NORMALLY_CLOSED ? "NC" : "NO");
  Serial.println("[DIAG] Expect click on each ON/OFF transition.");

  struct RelayDiag {
    int pin;
    bool* state;
    const char* name;
  } relays[] = {
    { PUMP_RELAY_PIN,  &actuatorState.pump,  "Pump"  },
    { LIGHT_RELAY_PIN, &actuatorState.light, "Light" },
    { MIST_RELAY_PIN,  &actuatorState.mist,  "Mist"  }
  };

  for (auto &relay : relays) {
    applyActuator(relay.pin, relay.state, true, relay.name, "DIAG ON");
    delay(800);
    applyActuator(relay.pin, relay.state, false, relay.name, "DIAG OFF");

    delay(800);
  }


  // Test Shed Servo
  Serial.println("\n[DIAG] Testing Shed Servo...");
  Serial.printf("[DIAG] Servo moving OPEN with angle: %d deg\n", plantCond.shedOpenAngle);
  setShed(true, "DIAG OPEN");
  delay(1000);
  Serial.printf("[DIAG] Servo moving CLOSED with angle: %d deg\n", plantCond.shedClosedAngle);
  setShed(false, "DIAG CLOSED");
  delay(1000);

  Serial.println("[DIAG] Restoring desired actuator states...");
  applyDesiredRelayOutputs();
  Serial.println("[DIAG] Relay test done.\n");
}

void handleSerialDiagnostics() {
  if (!Serial.available()) return;
  char c = (char)Serial.read();
  if (c == 'T' || c == 't') {
    runRelayDiagnosticTest();
  }
}

void setShed(bool open, const char* reason) {
  // FIX: was delay(SERVO_MOVE_MS=650ms) — blocked the 5ms relay timer for 650ms on every shed change.
  // Now: fire servo and return immediately. tickShed() will issue the stop pulse after SERVO_MOVE_MS.
  if (actuatorState.shed == open && shedMoveState == SHED_IDLE) {
    return;
  }
  shedTargetOpen = open;
  int angle    = open ? plantCond.shedOpenAngle : plantCond.shedClosedAngle;
  int pulseUs  = servoAngleToPulseUs(angle);
  shedServo.writeMicroseconds(pulseUs);
  shedMoveStart = millis();
  shedMoveState = SHED_MOVING;
  snprintf(shedReason, sizeof(shedReason), "%s", reason);
  Serial.printf("[ACTUATOR] Shed   -> %s  angle:%d deg pulse:%d us  (%s) [moving...]\n",
                open ? "OPEN" : "CLOSED", angle, pulseUs, reason);
}

// tickShed — call every loop() from relay timer. Stops servo after SERVO_MOVE_MS.
void tickShed() {
  if (shedMoveState != SHED_MOVING) return;
  if (millis() - shedMoveStart >= SERVO_MOVE_MS) {
    shedServo.writeMicroseconds(SERVO_STOP_US);
    actuatorState.shed    = shedTargetOpen;
    desiredRelayState.shed = shedTargetOpen;  // keep desired in sync
    shedMoveState = SHED_IDLE;
    Serial.printf("[ACTUATOR] Shed   -> %s  [stop pulse sent]\n", shedTargetOpen ? "OPEN" : "CLOSED");
  }
}

// ─────────────────────────────────────────────────────────────────
//  Named Actuator Helpers — production-quality abstraction layer
//  All relay control MUST go through these functions.
//  They update desiredRelayState and reason strings together,
//  preventing the race condition where state and reason diverge.
// ─────────────────────────────────────────────────────────────────
void turnPumpOn(const char* reason) {
  desiredRelayState.pump = true;
  snprintf(pumpReason, sizeof(pumpReason), "%s", reason);
}
void turnPumpOff(const char* reason) {
  desiredRelayState.pump = false;
  snprintf(pumpReason, sizeof(pumpReason), "%s", reason);
}
void turnLightOn(const char* reason) {
  desiredRelayState.light = true;
  snprintf(lightReason, sizeof(lightReason), "%s", reason);
}
void turnLightOff(const char* reason) {
  desiredRelayState.light = false;
  snprintf(lightReason, sizeof(lightReason), "%s", reason);
}
void turnMistOn(const char* reason) {
  desiredRelayState.mist = true;
  snprintf(mistReason, sizeof(mistReason), "%s", reason);
}
void turnMistOff(const char* reason) {
  desiredRelayState.mist = false;
  snprintf(mistReason, sizeof(mistReason), "%s", reason);
}

// ─────────────────────────────────────────────────────────────────
//  Utility: Rounding
// ─────────────────────────────────────────────────────────────────
float round2(float val) { return roundf(val * 100.0f) / 100.0f; }
float round1(float val) { return roundf(val * 10.0f)  / 10.0f;  }