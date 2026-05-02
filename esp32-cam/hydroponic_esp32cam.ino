/*
 * =====================================================================
 *  Smart Hydroponic Farming System – ESP32-CAM Image Upload Node
 *  Author  : Hydro-AI System
 *  Purpose : Capture plant images every 10–15 s, POST to Flask for
 *            Gemini Vision disease detection
 *  Board   : AI Thinker ESP32-CAM (OV2640)
 *
 *  OTA UPDATE: After first flash via serial, all future updates
 *  are wireless via Arduino IDE → Tools → Port → Network → hydro-cam
 *  OTA Password: see OTA_PASSWORD below
 *
 *  PARTITION: MUST use "Huge APP (3MB No OTA/1MB SPIFFS)"
 *  This leaves ~1.28 MB per OTA slot, enough for the camera sketch.
 * =====================================================================
 * =====================================================================
 *
 *  WIRING REMINDER:
 *    GPIO0  → GND  (for flash; remove for normal run)
 *    FTDI   → 5 V, GND, TX→U0R, RX→U0T
 *
 *  Board: "AI Thinker ESP32-CAM" in Arduino IDE
 *  Partition: "Huge APP (3MB No OTA/1MB SPIFFS)"
 * =====================================================================
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ─── OTA Libraries (built-in with ESP32 board package) ─────────────
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>

// ─── Camera Pin Map (AI Thinker ESP32-CAM) ───────────────────────
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ─── WiFi Credentials ─────────────────────────────────────────────
const char* WIFI_SSID     = "Galaxy M3285D4";
const char* WIFI_PASSWORD = "987654321";

// ─── Flask Server ─────────────────────────────────────────────────
const char* FLASK_HOST         = "https://hydrophonics-server-1.onrender.com";  // ← Change to your Flask IP
const char* ENDPOINT_UPLOAD    = "/upload-image";

// ─── OTA Configuration ────────────────────────────────────────────
const char* OTA_HOSTNAME = "hydro-cam";         // Visible in Arduino IDE as network port
const char* OTA_PASSWORD = "hydro1234";         // ← Change this to your OTA password

// ─── Capture Schedule ────────────────────────────────────────────
//  3 photos per day → one every 8 hours
#define PHOTOS_PER_DAY        3
#define CAPTURE_INTERVAL_MS   (8UL * 60UL * 60UL * 1000UL)   // 8 hours in ms
#define DAY_RESET_MS          (24UL * 60UL * 60UL * 1000UL)  // 24 hours in ms

// ─── Flash LED (GPIO 4 on AI Thinker) ────────────────────────────
#define FLASH_GPIO 4

unsigned long lastCapture   = -(CAPTURE_INTERVAL_MS);  // Fire immediately at boot
unsigned long dayStartMs    = 0;    // millis() when the current 24-h window began
int           photosToday   = 0;    // how many captures have been taken today

// ─────────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[CAM] ESP32-CAM Plant Monitor – Booting…");

  // Flash LED pin
  pinMode(FLASH_GPIO, OUTPUT);
  digitalWrite(FLASH_GPIO, LOW);

  // ── Camera configuration ──────────────────────────────────────
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // Quality settings based on PSRAM availability
  if (psramFound()) {
    config.frame_size    = FRAMESIZE_VGA;   // 640×480 – good balance of detail vs. size
    config.jpeg_quality  = 12;              // 0–63; lower = better quality
    config.fb_count      = 2;
    Serial.println("[CAM] PSRAM found – using VGA, quality 12");
  } else {
    config.frame_size    = FRAMESIZE_QVGA;  // 320×240 fallback
    config.jpeg_quality  = 20;
    config.fb_count      = 1;
    Serial.println("[CAM] No PSRAM – using QVGA, quality 20");
  }

  // Init camera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Camera init FAILED: 0x%x\n", err);
    Serial.println("[CAM] Rebooting in 5 s…");
    delay(5000);
    ESP.restart();
  }
  Serial.println("[CAM] Camera initialized successfully");

  // Tune sensor settings for plant imaging
  sensor_t* s = esp_camera_sensor_get();
  s->set_brightness(s, 0);      // -2 to 2
  s->set_contrast(s, 1);        // -2 to 2 (slight boost for leaf detail)
  s->set_saturation(s, 1);      // -2 to 2 (enhance green signature)
  s->set_sharpness(s, 1);       // sharpen leaf edges
  s->set_awb_gain(s, 1);        // auto white balance gain
  s->set_wb_mode(s, 0);         // 0 = auto
  s->set_exposure_ctrl(s, 1);   // auto exposure
  s->set_gain_ctrl(s, 1);       // auto gain

  // ── WiFi ──────────────────────────────────────────────────────
  connectWiFi();

  // ── OTA Setup ─────────────────────────────────────────────────
  setupOTA();
}

// ─────────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────────
void loop() {
  // ── OTA handler – keeps wireless update listening active ──────
  ArduinoOTA.handle();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Lost connection – reconnecting…");
    connectWiFi();
  }

  unsigned long now = millis();

  // ── Reset daily counter every 24 hours ──────────────────────────
  if (now - dayStartMs >= DAY_RESET_MS) {
    dayStartMs  = now;
    photosToday = 0;
    Serial.println("[CAM] New 24-hour window — photo counter reset.");
  }

  // ── Capture: max 3 photos per day, spaced 8 hours apart ─────────
  if (photosToday < PHOTOS_PER_DAY && (now - lastCapture >= CAPTURE_INTERVAL_MS)) {
    lastCapture = now;
    photosToday++;
    Serial.printf("[CAM] Photo %d/%d for today\n", photosToday, PHOTOS_PER_DAY);
    captureAndUpload();

    unsigned long nextMs = (photosToday < PHOTOS_PER_DAY)
                           ? CAPTURE_INTERVAL_MS / 3600000UL  // hours
                           : 0;
    if (photosToday < PHOTOS_PER_DAY) {
      Serial.printf("[CAM] Next photo in %lu hour(s).\n",
                    CAPTURE_INTERVAL_MS / 3600000UL);
    } else {
      Serial.println("[CAM] All 3 photos taken. Sleeping until tomorrow.");
    }
  }

  delay(1000);  // Yield — 1 s tick is fine given 8-hour intervals
}

// ─────────────────────────────────────────────────────────────────
//  WiFi Connection
// ─────────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 40) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WIFI] Connection failed. Will retry in next loop.");
  }
}

// ─────────────────────────────────────────────────────────────────
//  OTA Setup (Wireless update capability)
// ─────────────────────────────────────────────────────────────────
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    // IMPORTANT: stop camera grab during OTA to free memory
    esp_camera_deinit();
    Serial.println("[OTA] Starting firmware update…");
    // Flash LED solid ON
    digitalWrite(FLASH_GPIO, HIGH);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Done! Rebooting…");
    digitalWrite(FLASH_GPIO, LOW);
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int pct = progress / (total / 100);
    Serial.printf("[OTA] %u%%\r", pct);
    // Blink flash LED as progress indicator
    digitalWrite(FLASH_GPIO, (pct % 4 < 2) ? HIGH : LOW);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if      (error == OTA_AUTH_ERROR)    Serial.println("Wrong password!");
    else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin failed – check partition!");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive failed");
    else if (error == OTA_END_ERROR)     Serial.println("End failed");
    digitalWrite(FLASH_GPIO, LOW);
  });

  ArduinoOTA.begin();

  Serial.println("[OTA] Wireless update ready!");
  Serial.printf("[OTA] Hostname : %s.local\n", OTA_HOSTNAME);
  Serial.printf("[OTA] IP       : %s\n", WiFi.localIP().toString().c_str());
  Serial.println("[OTA] Arduino IDE: Tools → Port → Network Ports → hydro-cam");
}

// ─────────────────────────────────────────────────────────────────
//  Capture Image & Upload to Flask
// ─────────────────────────────────────────────────────────────────
void captureAndUpload() {
  Serial.println("[CAM] Capturing frame…");

  // Brief flash for consistent lighting (optional)
  digitalWrite(FLASH_GPIO, HIGH);
  delay(150);

  camera_fb_t* fb = esp_camera_fb_get();

  digitalWrite(FLASH_GPIO, LOW);

  if (!fb) {
    Serial.println("[CAM] Frame capture FAILED");
    return;
  }

  Serial.printf("[CAM] Frame captured: %zu bytes (format: %d)\n", fb->len, fb->format);

  // ── Upload via HTTP POST (binary JPEG) ───────────────────────
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    String url = String(FLASK_HOST) + ENDPOINT_UPLOAD;
    http.begin(client, url);
    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader("X-Device",     "esp32-cam");
    http.setTimeout(15000);  // 15 s timeout for large images

    int httpCode = http.POST(fb->buf, fb->len);

    if (httpCode == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.printf("[HTTP] Upload OK (%d) – Response: %s\n", httpCode, response.c_str());
    } else {
      Serial.printf("[HTTP] Upload FAILED: %d – %s\n", httpCode, http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    Serial.println("[HTTP] No WiFi – skipping upload");
  }

  // Return frame buffer to driver
  esp_camera_fb_return(fb);
}
