/*
 * =====================================================================
 *  Smart Hydroponic Farming System – ESP32 Autonomous Control Node
 *  Author  : Hydro-AI System
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
 *  Override is respected for OVERRIDE_TIMEOUT_MS, then auto resumes.
 *
 *  OTA UPDATE: After first flash via USB, all future updates are
 *  done wirelessly via Arduino IDE → Ports → (device hostname)
 *  OTA Password: see OTA_PASSWORD below
 * =====================================================================
 */

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

#define ONE_WIRE_BUS    5       // DS18B20 data pin (legacy wiring)

#define PH_PIN          39      // pH sensor analog input (VN)
#define TDS_PIN         35      // TDS sensor analog input
#define TRIG_PIN        26      // Ultrasonic sensor Trigger pin
#define ECHO_PIN        18      // Ultrasonic sensor Echo pin
#define LDR_PIN         33      // LDR (sunlight) analog input

#define PUMP_RELAY_PIN  13      // Pump relay  (Active LOW)
#define LIGHT_RELAY_PIN 12      // LED grow light relay (Active LOW)
#define MIST_RELAY_PIN  14      // Mist maker relay (Active LOW)
#define SERVO_PIN       27      // Shed servo motor

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
#define WATER_LEVEL_SAMPLES       5
#define ULTRASONIC_TIMEOUT_US     30000UL
#define ULTRASONIC_INTERPING_MS   25

// ═══════════════════════════════════════════════════════════════════
//  AUTONOMOUS THRESHOLDS – NOW DYNAMIC
//  Thresholds are no longer hardcoded here. They are stored in the
//  PlantConditions struct below and fetched from Flask /current-plant
//  every PLANT_FETCH_INTERVAL ms. Default values are set in the struct.
// ═══════════════════════════════════════════════════════════════════

// ─── Timing ───────────────────────────────────────────────────────
#define SENSOR_INTERVAL      2000    // ms between sensor reads & POST
#define CONTROL_INTERVAL     4000    // ms between control polling
#define OVERRIDE_TIMEOUT_MS  30000   // ms manual override lasts (30 s)
#define PLANT_FETCH_INTERVAL 30000   // ms between /current-plant refreshes

// ─── Objects ──────────────────────────────────────────────────────
DHT dht(DHTPIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature waterTempSensor(&oneWire);
Servo shedServo;

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

// ─── Override Tracking ────────────────────────────────────────────
struct Override {
  bool     active;
  unsigned long expiresAt;
};

SensorData    sensorData;
ActuatorState actuatorState = { false, false, false, false };

Override pumpOverride  = { false, 0 };
Override lightOverride = { false, 0 };
Override mistOverride  = { false, 0 };
Override shedOverride  = { false, 0 };

// ─── Reason strings (sent to dashboard) ──────────────────────────
char pumpReason[48]  = "Continuous — always on";
char lightReason[48] = "Initializing…";
char mistReason[48]  = "Initializing…";
char shedReason[64]  = "Initializing…";

unsigned long lastSensorSend  = 0;
unsigned long lastControlPoll = 0;
unsigned long lastPlantFetch  = 0;
bool ds18b20Found = false;

// ─────────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[HYDRO] Smart Hydroponic System – Autonomous Node Booting…");

  // Actuator pins – HIGH = relay OFF (active-low)
  pinMode(PUMP_RELAY_PIN,  OUTPUT); digitalWrite(PUMP_RELAY_PIN,  HIGH);
  pinMode(LIGHT_RELAY_PIN, OUTPUT); digitalWrite(LIGHT_RELAY_PIN, HIGH);
  pinMode(MIST_RELAY_PIN,  OUTPUT); digitalWrite(MIST_RELAY_PIN,  HIGH);

  // Built-in LED for OTA feedback
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Servo for shed
  shedServo.attach(SERVO_PIN);
  setShed(false, "Boot default – closed");  // Start closed for safety

  // Sensor init
  dht.begin();
  waterTempSensor.begin();
  ds18b20Found = (waterTempSensor.getDeviceCount() > 0);
  if (ds18b20Found) {
    Serial.printf("[SENSOR] DS18B20 detected on GPIO %d (count=%d)\n", ONE_WIRE_BUS, waterTempSensor.getDeviceCount());
  } else {
    Serial.printf("[SENSOR] DS18B20 NOT detected on GPIO %d. Check DATA pin + 4.7k pull-up to 3.3V.\n", ONE_WIRE_BUS);
  }

  // ADC setup for analog sensors.
  // ESP32 defaults can clip around ~1.1V; 11dB allows full 0-3.3V range.
  analogReadResolution(12);
  
  // Ultrasonic Sensor Init
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // WiFi
  connectWiFi();

  // OTA
  setupOTA();

  // ── Pump runs IMMEDIATELY and CONTINUOUSLY ────────────────────
  applyActuator(PUMP_RELAY_PIN, &actuatorState.pump, true, "Pump", pumpReason);
  Serial.println("[AUTO] Pump started — will run continuously.");
}

// ─────────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();  // MUST be first in loop

  unsigned long now = millis();

  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Connection lost – reconnecting…");
    connectWiFi();
  }

  // ── Read sensors & run autonomous logic ───────────────────────
  if (now - lastSensorSend >= SENSOR_INTERVAL) {
    lastSensorSend = now;
    readAllSensors();
    runAutonomousControl(now);
    printStatus();
    postSensorData();
  }

  // ── Poll for dashboard manual overrides ────────────────────────
  if (now - lastControlPoll >= CONTROL_INTERVAL) {
    lastControlPoll = now;
    pollControlCommands(now);
  }

  // ── Refresh plant thresholds from Flask every 30 s ─────────────
  if (!plantCond.fetched || (now - lastPlantFetch >= PLANT_FETCH_INTERVAL)) {
    lastPlantFetch = now;
    fetchPlantConditions();
  }
}

// ─────────────────────────────────────────────────────────────────
//  AUTONOMOUS CONTROL LOGIC
//  Called every SENSOR_INTERVAL. Checks overrides before acting.
// ─────────────────────────────────────────────────────────────────
void runAutonomousControl(unsigned long now) {

  // ── PUMP: always ON (no logic needed, just enforce) ───────────
  if (!actuatorState.pump) {
    applyActuator(PUMP_RELAY_PIN, &actuatorState.pump, true, "Pump", pumpReason);
    Serial.println("[AUTO] Pump was OFF – re-enabling (must stay ON).");
  }

  // ── SHED: sunlight intensity → protect plants ─────────────────
  if (!isOverrideActive(shedOverride, now)) {
    bool currentShed = actuatorState.shed;
    bool newShed     = currentShed;

    if (!currentShed && sensorData.sunlight < plantCond.shedOpenThreshold) {
      newShed = true;
      snprintf(shedReason, sizeof(shedReason),
               "Auto-opened: sunlight %d%% < %d%% threshold",
               sensorData.sunlight, plantCond.shedOpenThreshold);
    } else if (currentShed && sensorData.sunlight >= plantCond.shedCloseThreshold) {
      newShed = false;
      snprintf(shedReason, sizeof(shedReason),
               "Auto-closed: sunlight %d%% ≥ %d%% threshold",
               sensorData.sunlight, plantCond.shedCloseThreshold);
    } else {
      snprintf(shedReason, sizeof(shedReason),
               "Sunlight %d%% in safe range [%d–%d]%%",
               sensorData.sunlight, plantCond.shedOpenThreshold, plantCond.shedCloseThreshold);
    }

    if (newShed != currentShed) {
      setShed(newShed, shedReason);
    }
  } else {
    snprintf(shedReason, sizeof(shedReason), "Manual override active");
  }

  // ── GROW LIGHT: supplement when sun is low ────────────────────
  if (!isOverrideActive(lightOverride, now)) {
    bool newLight = actuatorState.light;

    if (!actuatorState.light && sensorData.sunlight < plantCond.lightOnThreshold) {
      newLight = true;
      snprintf(lightReason, sizeof(lightReason),
               "Auto-on: sunlight %d%% < %d%%",
               sensorData.sunlight, plantCond.lightOnThreshold);
    } else if (actuatorState.light && sensorData.sunlight >= plantCond.lightOffThreshold) {
      newLight = false;
      snprintf(lightReason, sizeof(lightReason),
               "Auto-off: sunlight %d%% ≥ %d%%",
               sensorData.sunlight, plantCond.lightOffThreshold);
    } else {
      snprintf(lightReason, sizeof(lightReason),
               "Sun %d%% — light %s",
               sensorData.sunlight,
               actuatorState.light ? "ON" : "OFF");
    }

    if (newLight != actuatorState.light) {
      applyActuator(LIGHT_RELAY_PIN, &actuatorState.light, newLight, "Light", lightReason);
    }
  } else {
    snprintf(lightReason, sizeof(lightReason), "Manual override active");
  }

  // ── MIST MAKER: humidity management ──────────────────────────
  if (!isOverrideActive(mistOverride, now)) {
    bool newMist = actuatorState.mist;

    if (!actuatorState.mist && sensorData.airHumidity < plantCond.humidityMin) {
      newMist = true;
      snprintf(mistReason, sizeof(mistReason),
               "Auto-on: humidity %.0f%% < %.0f%%",
               sensorData.airHumidity, plantCond.humidityMin);
    } else if (actuatorState.mist && sensorData.airHumidity >= plantCond.humidityMax) {
      newMist = false;
      snprintf(mistReason, sizeof(mistReason),
               "Auto-off: humidity %.0f%% ≥ %.0f%%",
               sensorData.airHumidity, plantCond.humidityMax);
    } else {
      snprintf(mistReason, sizeof(mistReason),
               "Humidity %.0f%% — mist %s",
               sensorData.airHumidity,
               actuatorState.mist ? "ON" : "OFF");
    }

    if (newMist != actuatorState.mist) {
      applyActuator(MIST_RELAY_PIN, &actuatorState.mist, newMist, "Mist", mistReason);
    }
  } else {
    snprintf(mistReason, sizeof(mistReason), "Manual override active");
  }
}

// ─────────────────────────────────────────────────────────────────
//  WiFi
// ─────────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WIFI] Failed. Will retry in loop.");
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

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  String url = String(FLASK_HOST) + "/current-plant";
  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String body = http.getString();
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

      // Store plant name for Serial logging
      const char* name = doc["plant"] | "unknown";
      strncpy(plantCond.plantName, name, sizeof(plantCond.plantName) - 1);
      plantCond.plantName[sizeof(plantCond.plantName) - 1] = '\0';

      plantCond.fetched = true;
      Serial.printf("[PLANT] Thresholds updated for '%s':\n", plantCond.plantName);
      Serial.printf("        Temp: %.1f–%.1f°C | Hum: %.0f–%.0f%% | pH: %.1f–%.1f | TDS: %.0f–%.0f ppm\n",
                    plantCond.temperatureMin, plantCond.temperatureMax,
                    plantCond.humidityMin, plantCond.humidityMax,
                    plantCond.phMin, plantCond.phMax,
                    plantCond.tdsMin, plantCond.tdsMax);
    } else {
      Serial.println("[PLANT] JSON parse error for /current-plant");
    }
  } else {
    Serial.printf("[PLANT] /current-plant GET FAILED: %d\n", httpCode);
  }
  http.end();
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
  waterTempSensor.requestTemperatures();
  float wtC = waterTempSensor.getTempCByIndex(0);
  if (wtC != DEVICE_DISCONNECTED_C && wtC > -100.0f && wtC < 125.0f && wtC >= WATER_TEMP_MIN_C && wtC <= WATER_TEMP_MAX_C) {
    sensorData.waterTemperature = wtC;
  } else {
    Serial.printf("[SENSOR] DS18B20 read failed on GPIO %d: %.2fC (keeping last %.2fC)\n",
                  ONE_WIRE_BUS, wtC, sensorData.waterTemperature);
  }

  // ── pH Sensor ────────────────────────────────────────────────
  int   phRaw  = analogRead(PH_PIN);
  float phVolt = (phRaw / ADC_RESOLUTION) * VREF;
  sensorData.pH = constrain((-5.70f * phVolt) + 21.34f, 0.0f, 14.0f);

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
  sensorData.tds = max(0.0f, tdsCompensated);

  // ── Water Level (Ultrasonic) ──────────────────────────────────
  // Take multiple pings and average valid echoes for stable readings.
  float distanceSumCm = 0.0f;
  int validPings = 0;

  for (int i = 0; i < WATER_LEVEL_SAMPLES; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    unsigned long duration = pulseIn(ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT_US);
    if (duration > 0) {
      float distanceCm = (duration * 0.0343f) / 2.0f;
      if (distanceCm > 0.5f && distanceCm < 400.0f) {
        distanceSumCm += distanceCm;
        validPings++;
      }
    }

    delay(ULTRASONIC_INTERPING_MS);
  }

  if (validPings > 0 && TANK_DEPTH_CM > TANK_FULL_CM) {
    float distanceAvgCm = distanceSumCm / validPings;
    float fillPct = ((TANK_DEPTH_CM - distanceAvgCm) / (TANK_DEPTH_CM - TANK_FULL_CM)) * 100.0f;
    sensorData.waterLevel = constrain((int)roundf(fillPct), 0, 100);
  } else {
    Serial.printf("[SENSOR] Ultrasonic read failed (%d/%d valid) - keeping last water level %d%%\n",
                  validPings, WATER_LEVEL_SAMPLES, sensorData.waterLevel);
  }

  // ── Sunlight / LDR (inverted: higher LDR ADC = less light) ───
  int ldrRaw = analogRead(LDR_PIN);
  sensorData.sunlight = constrain(map(ldrRaw, 4095, 0, 0, 100), 0, 100);
}

// ─────────────────────────────────────────────────────────────────
//  Print Status to Serial
// ─────────────────────────────────────────────────────────────────
void printStatus() {
  Serial.println("════════════════════════════════════════");
  Serial.printf("[SENSOR]   Air Temp    : %.2f °C\n",  sensorData.airTemperature);
  Serial.printf("[SENSOR]   Humidity    : %.2f %%\n",  sensorData.airHumidity);
  Serial.printf("[SENSOR]   Water Temp  : %.2f °C\n",  sensorData.waterTemperature);
  Serial.printf("[SENSOR]   pH          : %.2f\n",     sensorData.pH);
  Serial.printf("[SENSOR]   TDS         : %.1f ppm\n", sensorData.tds);
  Serial.printf("[SENSOR]   Water Level : %d %%\n",    sensorData.waterLevel);
  Serial.printf("[SENSOR]   Sunlight    : %d %%\n",    sensorData.sunlight);
  Serial.println("────────────────────────────────────────");
  Serial.printf("[ACTUATOR] Pump        : %s  → %s\n", actuatorState.pump  ? "ON" : "OFF", pumpReason);
  Serial.printf("[ACTUATOR] Light       : %s  → %s\n", actuatorState.light ? "ON" : "OFF", lightReason);
  Serial.printf("[ACTUATOR] Mist        : %s  → %s\n", actuatorState.mist  ? "ON" : "OFF", mistReason);
  Serial.printf("[ACTUATOR] Shed        : %s  → %s\n", actuatorState.shed  ? "OPEN" : "CLOSED", shedReason);
  Serial.println("════════════════════════════════════════");
}

// ─────────────────────────────────────────────────────────────────
//  POST Sensor + Actuator Data to Flask
// ─────────────────────────────────────────────────────────────────
void postSensorData() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  String url = String(FLASK_HOST) + ENDPOINT_SENSOR;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  // Sensors
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

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  if (httpCode == HTTP_CODE_OK) {
    Serial.printf("[HTTP] Sensor POST OK (%d)\n", httpCode);
  } else {
    Serial.printf("[HTTP] Sensor POST FAILED: %d – %s\n", httpCode, http.errorToString(httpCode).c_str());
  }
  http.end();
}

// ─────────────────────────────────────────────────────────────────
//  Poll Dashboard Manual Overrides
//  Dashboard can still send commands; they last OVERRIDE_TIMEOUT_MS
//  then autonomous control resumes.
// ─────────────────────────────────────────────────────────────────
void pollControlCommands(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  String url = String(FLASK_HOST) + ENDPOINT_CONTROL;
  http.begin(client, url);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    Serial.printf("[HTTP] Control response: %s\n", response.c_str());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response);
    if (!err) {
      // Pump override: dashboard cannot permanently turn pump off,
      // but we respect the override for OVERRIDE_TIMEOUT_MS
      if (doc["pump"].is<bool>()) {
        bool val = doc["pump"].as<bool>();
        pumpOverride = { true, now + OVERRIDE_TIMEOUT_MS };
        applyActuator(PUMP_RELAY_PIN, &actuatorState.pump, val, "Pump", "Manual override");
        snprintf(pumpReason, sizeof(pumpReason), "Manual override (auto resumes in 30s)");
      }
      if (doc["light"].is<bool>()) {
        bool val = doc["light"].as<bool>();
        lightOverride = { true, now + OVERRIDE_TIMEOUT_MS };
        applyActuator(LIGHT_RELAY_PIN, &actuatorState.light, val, "Light", "Manual override");
        snprintf(lightReason, sizeof(lightReason), "Manual override (auto resumes in 30s)");
      }
      if (doc["mist"].is<bool>()) {
        bool val = doc["mist"].as<bool>();
        mistOverride = { true, now + OVERRIDE_TIMEOUT_MS };
        applyActuator(MIST_RELAY_PIN, &actuatorState.mist, val, "Mist", "Manual override");
        snprintf(mistReason, sizeof(mistReason), "Manual override (auto resumes in 30s)");
      }
      if (doc["shed"].is<bool>()) {
        bool val = doc["shed"].as<bool>();
        shedOverride = { true, now + OVERRIDE_TIMEOUT_MS };
        setShed(val, "Manual override");
        snprintf(shedReason, sizeof(shedReason), "Manual override (auto resumes in 30s)");
      }
    }
  } else {
    Serial.printf("[HTTP] Control GET FAILED: %d\n", httpCode);
  }
  http.end();
}

// ─────────────────────────────────────────────────────────────────
//  Override Helper
// ─────────────────────────────────────────────────────────────────
bool isOverrideActive(Override& ov, unsigned long now) {
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
  *state = value;
  // Active-LOW relay: LOW = ON, HIGH = OFF
  digitalWrite(pin, value ? LOW : HIGH);
  Serial.printf("[ACTUATOR] %-6s → %s  (%s)\n", name, value ? "ON" : "OFF", reason);
}

void setShed(bool open, const char* reason) {
  actuatorState.shed = open;
  int angle = open ? 90 : 0;   // 90° = open (plants exposed), 0° = closed (plants protected)
  shedServo.write(angle);
  Serial.printf("[ACTUATOR] Shed   → %s  angle:%d°  (%s)\n",
                open ? "OPEN" : "CLOSED", angle, reason);
  snprintf(shedReason, sizeof(shedReason), "%s", reason);
}

// ─────────────────────────────────────────────────────────────────
//  Utility: Rounding
// ─────────────────────────────────────────────────────────────────
float round2(float val) { return roundf(val * 100.0f) / 100.0f; }
float round1(float val) { return roundf(val * 10.0f)  / 10.0f;  }