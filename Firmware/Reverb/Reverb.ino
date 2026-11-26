#include "driver/i2s.h"
#include "esp_timer.h"
#include <Wire.h>
#include <SD.h>
#include <SPI.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include <Stepper.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include "RevEng_PAJ7620.h"

// ------------------- PIN DEFINITIONS -------------------
#define SD_CS       38
#define SD_MOSI     11
#define SD_MISO     45
#define SD_SCK      12

#define RFID_CS     2
#define RFID_MOSI   40
#define RFID_MISO   13
#define RFID_SCK    10
#define RFID_RST    1

#define I2S_DOUT    5
#define I2S_BCLK    6
#define I2S_LRC     7

#define SDA_PIN     8
#define SCL_PIN     9

#define VSENSE_PIN  14
#define IN1         18
#define IN2         17
#define IN3         16
#define IN4         15
#define HALL_PIN    21
#define LED_PIN     48
#define LED_COUNT   24

#define MAPPINGS_FILE "/mappings.json"
#define WIFI_FILE     "/wifi.json"
#define SETTINGS_FILE "/settings.json"

#define VBAT_RATIO  2.0f
#define ADC_REF     3.3f
#define ADC_MAX     4095.0f
#define BAT_MAX     4.2f
#define BAT_MIN     3.0f

#define SAMPLE_RATE   44100
#define I2S_BUF_SIZE  2048

// 1 = wave drive (one coil at a time, ~half the power)
// 0 = full-step (two coils, more torque). Switch to 0 if the disk stalls.
#define STEPPER_WAVE_DRIVE 1

// ------------------- SPI BUSES -------------------
SPIClass spiSD(FSPI);
SPIClass spiRFID(HSPI);

// ------------------- RFID -------------------
MFRC522DriverPinSimple ss_pin(RFID_CS);
MFRC522DriverSPI       driver{ ss_pin, spiRFID };
MFRC522                mfrc522{ driver };

// ------------------- NEOPIXEL -------------------
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
bool          neoEnabled    = true;
int           neoBrightness = 80;
int           fadeBrightness = 80;
bool          isFading      = false;
unsigned long lastFadeTime  = 0;

// ------------------- STEPPER -------------------
const int           stepsPerRevolution = 2048;
Stepper             myStepper(stepsPerRevolution, IN1, IN3, IN2, IN4); // only used for boot beeps
int                 motorSpeed         = 5;   // RPM

// Bit order of masks: bit0=IN1, bit1=IN2, bit2=IN3, bit3=IN4
#if STEPPER_WAVE_DRIVE
const uint8_t stepMasks[4] = { 0b0010, 0b0100, 0b1000, 0b0001 };  // IN2, IN3, IN4, IN1
#else
const uint8_t stepMasks[4] = { 0b0011, 0b0110, 0b1100, 0b1001 };  // IN1+2, IN2+3, IN3+4, IN4+1
#endif

volatile bool       stepperRun   = false;
uint8_t             stepPhase    = 0;
esp_timer_handle_t  stepperTimer = nullptr;

// ------------------- AUDIO STATE -------------------
File  audioFile;
bool  isPlaying    = false;
int   volume       = 15;
int   audioLevel   = 0;
int   dynamicMax   = 1000;
unsigned long lastVisFrame = 0;
#define NUM_BANDS 8
const float bandFreq[NUM_BANDS] = {60, 150, 300, 600, 1200, 2400, 4800, 9600};
float bandCoeff[NUM_BANDS];
float bandPeak[NUM_BANDS];
float bandLevel[NUM_BANDS];  // 0..1, read by the visualizer
float visualizerLevels[8] = {0};
bool songFinished = false;

// Tuning
const float ATTACK_SPEED = 0.65f;
const float DECAY_SPEED  = 0.12f;
float smoothedBattery = -1;
const float BATTERY_SMOOTHING = 0.15f;

void resetBands() {
  for (int b = 0; b < NUM_BANDS; b++) {
    bandLevel[b] = 0;
    bandPeak[b] = 0.05f;
  }
  for (int i = 0; i < NUM_BANDS; i++) {
    visualizerLevels[i] = 0;
  }
}

void initBands() {
  for (int b = 0; b < NUM_BANDS; b++) {
    bandCoeff[b] = 2.0f * cosf(2.0f * PI * bandFreq[b] / SAMPLE_RATE);
  }
  resetBands();
}

// buf = interleaved 16-bit stereo (raw, before volume scaling)
void analyzeBands(const int16_t* buf, int frames) {
  if (frames < 64) return;

  float s1[NUM_BANDS] = {0};
  float s2[NUM_BANDS] = {0};

  for (int n = 0; n < frames; n++) {
    float x = ((int)buf[2 * n] + (int)buf[2 * n + 1]) * (1.0f / 65536.0f);
    for (int b = 0; b < NUM_BANDS; b++) {
      float s0 = x + bandCoeff[b] * s1[b] - s2[b];
      s2[b] = s1[b];
      s1[b] = s0;
    }
  }

  for (int b = 0; b < NUM_BANDS; b++) {
    float p = s1[b] * s1[b] + s2[b] * s2[b] - bandCoeff[b] * s1[b] * s2[b];
    float mag = sqrtf(p > 0 ? p : 0) / frames;

    bandPeak[b] *= 0.995f;
    if (mag > bandPeak[b]) bandPeak[b] = mag;
    if (bandPeak[b] < 0.01f) bandPeak[b] = 0.01f;

    float lv = mag / bandPeak[b];
    if (lv > 1.0f) lv = 1.0f;
    bandLevel[b] = (lv > bandLevel[b]) ? lv : bandLevel[b] * 0.8f;
  }
}

// ------------------- STATE -------------------
String currentUID    = "";
int    currentTrackIndex = 0;   // which track of an album is playing (0 for single disks)
bool   sdReady       = false;
bool   lastHallState = false;
bool   diskPresent   = false;

// RFID scan state machine
enum ScanState { IDLE, WAITING_FOR_CARD, CARD_READ };
ScanState scanState      = IDLE;
String    lastScannedUID = "";
bool      scanRequested  = false;

// ------------------- WEB SERVER -------------------
WebServer server(80);
DynamicJsonDocument mappingsDoc(8192);

// ------------------- GESTURE SENSOR -------------------
RevEng_PAJ7620 sensor = RevEng_PAJ7620();

// ------------------- FORWARD DECLARATIONS -------------------
void handleHallSensor();
void handleRFIDStateMachine();
void handleGestures();
void handleStepper();
void handleFade();
void updateVisualizer();
void smoothVisualizer();
void loadMappings();
void saveMappings();
void loadSettings();
void saveSettings();
void setupWebServer();
void setupI2S();
void stopAudio();
void playFile(const String& path, const String& uid, int trackIndex = 0);
void onTrackFinished();
void skipTrack(int dir);
int getTrackCount(const String& uid);
String getTrackPath(const String& uid, int index);
void compactMappings();
void handleAudioLoop();
void stepperSignal(int pulses);
void releaseStepper();
void setupStepperTimer();
void startRFIDScan();
void initRFID();
bool rfidHealthy();
bool loadWiFiCredentials(String& ssid, String& password);
String getFileForUID(const String& uid);
float getBatteryPercent();
int getSmoothedLevel();

// ======================================================
//  STEPPER
// ======================================================

void releaseStepper() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

// Boot beeps (blocking, setup only)
void stepperSignal(int pulses) {
  for (int i = 0; i < pulses; i++) {
    for (int s = 0; s < 50; s++) myStepper.step(1);
    delay(300);
  }
  releaseStepper();  // don't leave coils energized after the beeps
}

// Runs in the esp_timer task, independent of loop() timing
void stepperTick(void*) {
  static bool energized = false;
  if (!stepperRun) {
    if (energized) { releaseStepper(); energized = false; }
    return;
  }
  stepPhase = (stepPhase + 1) & 3;
  uint8_t m = stepMasks[stepPhase];
  digitalWrite(IN1, (m >> 0) & 1);
  digitalWrite(IN2, (m >> 1) & 1);
  digitalWrite(IN3, (m >> 2) & 1);
  digitalWrite(IN4, (m >> 3) & 1);
  energized = true;
}

void setupStepperTimer() {
  uint64_t intervalUs = 60000000ULL / ((uint64_t)stepsPerRevolution * motorSpeed);
  esp_timer_create_args_t args = {};
  args.callback        = &stepperTick;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name            = "stepper";
  esp_timer_create(&args, &stepperTimer);
  esp_timer_start_periodic(stepperTimer, intervalUs);
}

// ======================================================
//  I2S AUDIO
// ======================================================

void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 128
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_BCLK,
    .ws_io_num    = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

void stopAudio() {
  if (audioFile) audioFile.close();
  isPlaying = false;
  songFinished = true;
  resetBands();
  i2s_zero_dma_buffer(I2S_NUM_0);
  // Note: currentUID is NOT cleared here so now playing stays visible
}

void playFile(const String& path, const String& uid, int trackIndex) {
  // Stop without clearing UID
  if (audioFile) audioFile.close();
  isPlaying = false;
  resetBands();
  i2s_zero_dma_buffer(I2S_NUM_0);

  audioFile = SD.open(path.c_str());
  if (!audioFile) { Serial.println("Failed to open: " + path); return; }
  audioFile.seek(44);  // skip WAV header
  isPlaying  = true;
  currentUID = uid;    // set after successful open
  currentTrackIndex = trackIndex;
  Serial.println("Playing: " + path + " UID: " + uid + " track " + String(trackIndex + 1));
}

// End of a track: move on to the next one in the album, otherwise stop.
void onTrackFinished() {
  String uid = currentUID;
  int next = currentTrackIndex + 1;
  if (uid.length() && next < getTrackCount(uid)) {
    String path = getTrackPath(uid, next);
    if (path.length()) {
      playFile(path, uid, next);
      if (isPlaying) return;   // next track opened fine
    }
  }
  stopAudio();                 // last track done (or next one failed to open)
}

// Web "skip": dir = +1 next track, -1 previous track.
// Next on the last track does nothing; previous on the first track restarts it.
void skipTrack(int dir) {
  if (!isPlaying || !currentUID.length()) return;
  int count  = getTrackCount(currentUID);
  int target = currentTrackIndex + dir;
  if (target >= count) return;
  if (target < 0) target = 0;
  String uid  = currentUID;
  String path = getTrackPath(uid, target);
  if (path.length()) playFile(path, uid, target);
  if (!isPlaying) stopAudio();   // target failed to open
}

void handleAudioLoop() {
  if (!isPlaying || !audioFile) return;
  if (!audioFile.available()) { onTrackFinished(); return; }

  int16_t buffer[I2S_BUF_SIZE];
  float volScale = (float)volume / 21.0f;
  size_t bytesRead = audioFile.read((uint8_t*)buffer, sizeof(buffer));
  int samples = bytesRead / 2;
  analyzeBands(buffer, bytesRead / 4);

  // Calculate peak from RAW samples BEFORE volume scaling
  int peak = 0;
  for (int i = 0; i < samples; i++) {
    int val = abs(buffer[i]);  // raw unscaled value
    if (val > peak) peak = val;
  }
  audioLevel = (audioLevel * 11 + peak) / 12;

  // Apply volume scaling AFTER peak calculation
  for (int i = 0; i < samples; i++) {
    buffer[i] = (int16_t)(buffer[i] * volScale);
  }

  size_t bytesWritten;
  i2s_write(I2S_NUM_0, buffer, bytesRead, &bytesWritten, 100 / portTICK_PERIOD_MS);
}

// ======================================================
//  BATTERY
// ======================================================
float getBatteryPercent() {
  const int NUM_SAMPLES = 30;
  uint32_t totalMilliVolts = 0;

  for (int i = 0; i < NUM_SAMPLES; i++) {
    totalMilliVolts += analogReadMilliVolts(VSENSE_PIN);
    delayMicroseconds(500);
  }

  // Average voltage at the ADC pin
  float pinVoltage = (totalMilliVolts / (float)NUM_SAMPLES) / 1000.0f;

  // Convert to actual battery voltage
  float batteryVoltage = pinVoltage * VBAT_RATIO;

  // Convert voltage to percentage
  float pct = (batteryVoltage - BAT_MIN) /
              (BAT_MAX - BAT_MIN) * 100.0f;

  pct = constrain(pct, 0.0f, 100.0f);

  // Smooth changes between web requests
  if (smoothedBattery < 0) {
    smoothedBattery = pct;
  } else {
    smoothedBattery +=
        (pct - smoothedBattery) * BATTERY_SMOOTHING;
  }

  return smoothedBattery;
}

// ======================================================
//  WIFI CREDENTIALS FROM SD
// ======================================================

bool loadWiFiCredentials(String& ssid, String& password) {
  if (!SD.exists(WIFI_FILE)) return false;
  File f = SD.open(WIFI_FILE, FILE_READ);
  if (!f) return false;
  DynamicJsonDocument doc(256);
  auto err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  ssid     = doc["ssid"].as<String>();
  password = doc["password"].as<String>();
  return ssid.length() > 0;
}

// ======================================================
//  MAPPING HELPERS
// ======================================================

void loadMappings() {
  if (!SD.exists(MAPPINGS_FILE)) return;
  File f = SD.open(MAPPINGS_FILE, FILE_READ);
  if (!f) return;
  auto err = deserializeJson(mappingsDoc, f);
  f.close();
  if (err) Serial.println("mappings.json parse error");
}

void saveMappings() {
  File f = SD.open(MAPPINGS_FILE, FILE_WRITE);
  if (!f) return;
  serializeJson(mappingsDoc, f);
  f.close();
}

// Number of tracks on a disk: album length, 1 for a single-file disk, 0 if unmapped.
int getTrackCount(const String& uid) {
  if (!mappingsDoc.containsKey(uid)) return 0;
  JsonArray t = mappingsDoc[uid]["tracks"].as<JsonArray>();
  if (!t.isNull() && t.size() > 0) return (int)t.size();
  return mappingsDoc[uid]["file"].as<String>().length() ? 1 : 0;
}

String getTrackPath(const String& uid, int index) {
  if (index < 0 || !mappingsDoc.containsKey(uid)) return "";
  JsonArray t = mappingsDoc[uid]["tracks"].as<JsonArray>();
  if (!t.isNull() && t.size() > 0)
    return (index < (int)t.size()) ? t[index].as<String>() : String("");
  return (index == 0) ? mappingsDoc[uid]["file"].as<String>() : String("");
}

// ArduinoJson doesn't free memory on remove(); rebuilding the doc reclaims it.
void compactMappings() {
  String tmp;
  serializeJson(mappingsDoc, tmp);
  mappingsDoc.clear();
  deserializeJson(mappingsDoc, tmp);
}

String getFileForUID(const String& uid) {
  return getTrackPath(uid, 0);   // first track (the only one for single disks)
}

// ======================================================
//  RFID STATE MACHINE
// ======================================================

void startRFIDScan() {
  initRFID();  // also turns the antenna back on
  if (scanState != IDLE) return;
  lastScannedUID = "";
  scanState = WAITING_FOR_CARD;
}

void handleRFIDStateMachine() {
  if (scanState != WAITING_FOR_CARD) return;
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 1000) {
    lastCheck = millis();
    if (!rfidHealthy()) { Serial.println("RFID unhealthy -> re-init"); initRFID(); }
  }
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  String newUID = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) newUID += "0";
    newUID += String(mfrc522.uid.uidByte[i], HEX);
    if (i < mfrc522.uid.size - 1) newUID += ":";
  }
  lastScannedUID = newUID;
  mfrc522.PICC_HaltA();
  mfrc522.PCD_AntennaOff();  // save power + RF noise; startRFIDScan() turns it back on
  scanState = CARD_READ;
  Serial.println("Card read");

  if (scanRequested) {
    scanRequested = false;
    scanState = IDLE;
    return;
  }

  if (newUID != currentUID) {
    String filename = getFileForUID(newUID);
    if (filename.length() && sdReady) {
      playFile(filename, newUID);
    }
  }
  scanState = IDLE;
}

// ======================================================
//  WEB SERVER ROUTES
// ======================================================

void handleRoot() {
  File f = SD.open("/index.html", FILE_READ);
  if (f) { server.streamFile(f, "text/html"); f.close(); return; }
  server.send(500, "text/plain", "index.html not found on SD");
}

void handleGetStatus() {
  float  bat  = getBatteryPercent();
  bool   hall = digitalRead(HALL_PIN);
  String label = "";
  if (currentUID.length() && mappingsDoc.containsKey(currentUID))
    label = mappingsDoc[currentUID]["label"].as<String>();

  StaticJsonDocument<512> doc;
  doc["ip"]            = WiFi.localIP().toString();
  doc["sd"]            = sdReady;
  doc["hall"]          = hall;
  doc["battery"]       = (int)bat;
  doc["playing"]       = isPlaying;
  doc["label"]         = label;
  doc["uid"]           = currentUID;
  doc["track"]         = isPlaying ? currentTrackIndex + 1 : 0;
  doc["tracks"]        = currentUID.length() ? getTrackCount(currentUID) : 0;
  doc["volume"]        = volume;
  doc["neoOn"]         = neoEnabled;
  doc["neoBrightness"] = neoBrightness;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleGetMappings() {
  String out;
  serializeJson(mappingsDoc, out);
  server.send(200, "application/json", out);
}

void handlePostMapping() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  DynamicJsonDocument body(2048);
  if (deserializeJson(body, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }
  String uid   = body["uid"].as<String>();
  String label = body["label"].as<String>();
  String file  = body["file"].as<String>();
  JsonArray tracksIn = body["tracks"].as<JsonArray>();
  if (!file.length() && !tracksIn.isNull() && tracksIn.size() > 0) file = tracksIn[0].as<String>();
  if (!uid.length() || !file.length()) { server.send(400, "text/plain", "Missing fields"); return; }

  compactMappings();
  mappingsDoc[uid]["label"] = label;
  mappingsDoc[uid]["file"]  = file;             // first track, so single-track code paths still work
  mappingsDoc[uid].remove("tracks");
  if (!tracksIn.isNull() && tracksIn.size() > 1) {
    JsonArray out = mappingsDoc[uid].createNestedArray("tracks");
    for (JsonVariant t : tracksIn) out.add(t.as<String>());
  }

  if (mappingsDoc.overflowed()) {                // album too big for the in-memory doc
    mappingsDoc.clear();
    loadMappings();                              // roll back to what's on the SD card
    server.send(507, "text/plain", "Too many tracks");
    return;
  }
  saveMappings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleDeleteMapping() {
  String uri = server.uri();
  String uid = uri.substring(uri.lastIndexOf('/') + 1);
  uid.replace("%3A", ":");
  uid.replace("%3a", ":");
  if (mappingsDoc.containsKey(uid)) mappingsDoc.remove(uid);
  saveMappings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleGetFiles() {
  if (!sdReady) { server.send(503, "application/json", "[]"); return; }
  String json = "[";
  File root = SD.open("/");
  bool first = true;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    entry.close();
    if (name.endsWith(".wav") || name.endsWith(".WAV")) {
      if (!first) json += ",";
      json += "\"/" + name + "\"";
      first = false;
    }
  }
  root.close();
  json += "]";
  server.send(200, "application/json", json);
}

void handleScanStart() {
  scanRequested = true;
  startRFIDScan();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleScanResult() {
  StaticJsonDocument<128> doc;
  doc["uid"]      = lastScannedUID;
  doc["scanning"] = (scanState == WAITING_FOR_CARD);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleSetVolume() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  DynamicJsonDocument body(64);
  if (deserializeJson(body, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }
  volume = constrain(body["volume"].as<int>(), 0, 21);
  saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetNeopixel() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  DynamicJsonDocument body(128);
  if (deserializeJson(body, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }
  bool newState  = body["on"].as<bool>();
  neoBrightness  = constrain(body["brightness"].as<int>(), 0, 255);

  if (!newState && neoEnabled) {
    // Turning off — start fade from current brightness
    isFading      = true;
    fadeBrightness = neoBrightness;
  } else if (newState) {
    // Turning on — instant
    isFading       = false;
    fadeBrightness = neoBrightness;
    strip.setBrightness(neoBrightness);
  } else {
    // Staying off — just update brightness for when it turns back on
    strip.setBrightness(neoBrightness);
  }
  neoEnabled = newState;
  saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleNextTrack() {
  skipTrack(+1);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handlePrevTrack() {
  skipTrack(-1);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSaveWifi() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  DynamicJsonDocument body(256);
  if (deserializeJson(body, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }
  String ssid     = body["ssid"].as<String>();
  String password = body["password"].as<String>();
  if (!ssid.length()) { server.send(400, "text/plain", "SSID required"); return; }
  File f = SD.open(WIFI_FILE, FILE_WRITE);
  if (!f) { server.send(500, "text/plain", "Failed to write wifi.json"); return; }
  serializeJson(body, f);
  f.close();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Saved. Restart to connect.\"}");
}

void setupWebServer() {
  server.on("/",                HTTP_GET,  handleRoot);
  server.on("/api/status",      HTTP_GET,  handleGetStatus);
  server.on("/api/mappings",    HTTP_GET,  handleGetMappings);
  server.on("/api/mappings",    HTTP_POST, handlePostMapping);
  server.on("/api/files",       HTTP_GET,  handleGetFiles);
  server.on("/api/scan/start",  HTTP_POST, handleScanStart);
  server.on("/api/scan/result", HTTP_GET,  handleScanResult);
  server.on("/api/volume",      HTTP_POST, handleSetVolume);
  server.on("/api/neopixel",    HTTP_POST, handleSetNeopixel);
  server.on("/api/wifi",        HTTP_POST, handleSaveWifi);
  server.on("/api/next",        HTTP_POST, handleNextTrack);
  server.on("/api/prev",        HTTP_POST, handlePrevTrack);

  server.onNotFound([]() {
    if (server.method() == HTTP_DELETE && server.uri().startsWith("/api/mappings/")) {
      handleDeleteMapping();
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });

  server.begin();
}

// ======================================================
//  SETUP
// ======================================================

void setup() {
  setCpuFrequencyMhz(160);  // plenty for audio + visualizer, saves tens of mA

  Serial.begin(115200);
  delay(200); // give USB-CDC a moment to enumerate before the first print

  pinMode(RFID_RST, OUTPUT);
  digitalWrite(RFID_RST, HIGH);
  delay(50);

  // Print WHY we rebooted before anything else runs
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Reset reason: ");
  switch (reason) {
    case ESP_RST_POWERON:   Serial.println("Power-on"); break;
    case ESP_RST_SW:        Serial.println("Software reset"); break;
    case ESP_RST_PANIC:     Serial.println("Exception/panic"); break;
    case ESP_RST_INT_WDT:   Serial.println("Interrupt watchdog"); break;
    case ESP_RST_TASK_WDT:  Serial.println("Task watchdog"); break;
    case ESP_RST_WDT:       Serial.println("Other watchdog"); break;
    case ESP_RST_BROWNOUT:  Serial.println("Brownout!"); break;
    default:                Serial.printf("Other (%d)\n", reason); break;
  }

  Serial.println("[setup] pins/motor init");
  pinMode(IN1, OUTPUT); digitalWrite(IN1, LOW);
  pinMode(IN2, OUTPUT); digitalWrite(IN2, LOW);
  pinMode(IN3, OUTPUT); digitalWrite(IN3, LOW);
  pinMode(IN4, OUTPUT); digitalWrite(IN4, LOW);
  Wire.begin(SDA_PIN, SCL_PIN);

  //Serial.println("[setup] gesture sensor begin");
  //sensor.begin();
  //Serial.println("[setup] gesture sensor OK");

  myStepper.setSpeed(motorSpeed);
  pinMode(HALL_PIN, INPUT);
  pinMode(VSENSE_PIN, INPUT);
  analogReadResolution(12);

  Serial.println("[setup] neopixel init");
  strip.begin();
  strip.show();
  strip.setBrightness(neoBrightness);
  initBands();

  // SD
  Serial.println("[setup] SD.begin...");
  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, spiSD, 4000000)) {
    Serial.println("[setup] SD FAILED");
    stepperSignal(5);
  } else {
    Serial.println("[setup] SD OK");
    sdReady = true;
    stepperSignal(2);
    loadMappings();
    Serial.println("[setup] mappings loaded");
    loadSettings();
    Serial.println("[setup] settings loaded");
  }

  // I2S
  Serial.println("[setup] I2S init...");
  setupI2S();
  Serial.println("[setup] I2S OK");

  // RFID
  Serial.println("[setup] RFID SPI begin...");
  spiRFID.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_CS);
  Serial.println("[setup] RFID PCD_Init...");
  initRFID();
  byte v = driver.PCD_ReadRegister(MFRC522::PCD_Register::VersionReg);
  byte t = driver.PCD_ReadRegister(MFRC522::PCD_Register::TxControlReg);
  Serial.printf("[setup] RFID baseline ver=0x%02X tx=0x%02X\n", v, t);
  mfrc522.PCD_AntennaOff();  // stay off until a disk is placed
  Serial.println("[setup] RFID OK");

  // WiFi
  Serial.println("[setup] loading WiFi creds...");
  String ssid, password;
  if (sdReady && loadWiFiCredentials(ssid, password)) {
    Serial.println("[setup] WiFi creds from SD: " + ssid);
  } else {
    Serial.println("[setup] no WiFi creds on SD, using defaults");
    ssid     = "YOUR_SSID";
    password = "YOUR_PASSWORD";
  }
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.println("[setup] WiFi.begin called, waiting...");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); attempts++;
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[setup] WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    WiFi.setTxPower(WIFI_POWER_11dBm);  // lower TX bursts; fine at normal router range
    MDNS.begin("reverb");
    setupWebServer();
    Serial.println("[setup] web server started");
    stepperSignal(3);
  } else {
    Serial.println("[setup] WiFi FAILED, starting AP mode");
    stepperSignal(1);
    WiFi.softAP("Reverb OS", "reverb123");
    WiFi.setTxPower(WIFI_POWER_11dBm);
    Serial.print("[setup] AP IP: ");
    Serial.println(WiFi.softAPIP());
    setupWebServer();
    Serial.println("[setup] web server started (AP mode)");
  }

  strip.setBrightness(neoBrightness);
  fadeBrightness = neoBrightness;

  Serial.printf("[setup] NeoPixel brightness: %d\n", neoBrightness);

  setupStepperTimer();  // from here on the timer drives the motor

  Serial.println("[setup] COMPLETE");
}

// ======================================================
//  LOOP
// ======================================================

void loop() {
  handleAudioLoop();
  server.handleClient();
  handleHallSensor();
  handleRFIDStateMachine();
  //handleGestures();
  handleStepper();
  handleFade();
  updateVisualizer();

  if (!isPlaying) delay(2);  // let the CPU idle when there's no audio to feed
}

// ======================================================
//  HANDLERS
// ======================================================

void handleHallSensor() {
  bool hallState = !digitalRead(HALL_PIN);
  diskPresent = hallState;

  // Rising edge: magnet detected → disk placed → start stepper + RFID scan
  if (hallState && !lastHallState) {
    songFinished = false;
    Serial.println("Disk placed — starting stepper and RFID scan");
    if (scanState == IDLE) startRFIDScan();
  }

  // Falling edge: magnet gone → disk removed → stop everything
  if (!hallState && lastHallState) {
    Serial.println("Disk removed — stopping");
    stopAudio();
    currentUID = "";  // clear UID only on disk removal
    scanState  = IDLE;
    mfrc522.PCD_AntennaOff();
  }

  lastHallState = hallState;
}

// Motor is driven by the timer (stepperTick); this just says whether it should run.
// It spins while a disk is present and the song hasn't finished.
void handleStepper() {
  stepperRun = diskPresent && !songFinished;
}

void handleFade() {
  if (!isFading) return;
  unsigned long now = millis();
  if (now - lastFadeTime < 16) return;
  lastFadeTime = now;

  fadeBrightness -= 2;
  if (fadeBrightness <= 0) {
    fadeBrightness = 0;
    isFading = false;
    strip.clear();
    strip.show();
    return;
  }

  // Scale each pixel color directly instead of setBrightness
  float scale = fadeBrightness / (float)neoBrightness;
  for (int i = 0; i < LED_COUNT; i++) {
    uint32_t c = strip.getPixelColor(i);
    uint8_t r = ((c >> 16) & 0xFF) * scale;
    uint8_t g = ((c >> 8)  & 0xFF) * scale;
    uint8_t b = (c         & 0xFF) * scale;
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

void handleGestures() {
  Gesture gesture = sensor.readGesture();

  if (gesture == GES_FORWARD  && volume < 21) volume++;
  if (gesture == GES_BACKWARD && volume > 0)  volume--;

  if (audioFile && isPlaying) {
    const long skipBytes = 44100L * 2 * 2 * 10;
    long currentPos = audioFile.position();
    long fileSize   = audioFile.size();
    if (gesture == GES_RIGHT) {
      long newPos = min(currentPos + skipBytes, fileSize - 1);
      audioFile.seek(newPos);
    }
    if (gesture == GES_LEFT) {
      long newPos = max(currentPos - skipBytes, 44L);
      audioFile.seek(newPos);
    }
  }
}

int getSmoothedLevel() {
  return audioLevel;  // already smoothed in handleAudioLoop
}

// Convert a visualizer coordinate to the physical NeoPixel index.
// x = frequency column (left to right), y = height (bottom to top).
// Physical wiring:
//   bottom: LED 8 ... LED 1   (left to right)
//   middle: LED 9 ... LED 16  (left to right)
//   top:    LED 24 ... LED 17 (left to right)
int visualizerPixelIndex(int x, int y) {
  if (y == 0) return 7 - x;  // Bottom: right to left
  if (y == 1) return 15 - x; // Middle: right to left
  if (y == 2) return 23 - x; // Top: right to left

  return -1;
}

void updateVisualizer() {
  static bool outputIsClear = true;
  static uint32_t lastSig = 0xFFFFFFFF;

  if (isFading) {
    lastSig = 0xFFFFFFFF;
    return;
  }

  if (!isPlaying || !neoEnabled) {
    if (!outputIsClear) {
      strip.clear();
      strip.show();
      outputIsClear = true;
      lastSig = 0xFFFFFFFF;
    }
    return;
  }

  outputIsClear = false;

  unsigned long now = millis();
  if (now - lastVisFrame < 40) return;
  lastVisFrame = now;

  uint8_t barHeight[8];
  uint32_t sig = 0;

  // Center-out frequency arrangement:
  // Physical columns left to right:
  // 3, 4, 2, 5, 1, 6, 0, 7
  const int bandOrder[8] = {
  7, 6, 1, 4, 3, 2, 5, 0
  };
  smoothVisualizer();
  for (int x = 0; x < 8; x++) {
    float level = visualizerLevels[bandOrder[x]];

    barHeight[x] =
      level > 0.70f ? 3 :
      level > 0.40f ? 2 :
      level > 0.12f ? 1 : 0;

    sig = (sig << 2) | barHeight[x];
  }

  if (sig == lastSig) return;
  lastSig = sig;

  strip.clear();

  for (int x = 0; x < 8; x++) {
    // Build vertical bars from bottom upward
    for (int y = 0; y < barHeight[x]; y++) {

      uint8_t brightness;

      if (y == 0) {
        brightness = 255;  // Bottom: brightest
      } else if (y == 1) {
        brightness = 110;  // Middle: medium
      } else {
        brightness = 30;   // Top: dimmest
      }

      int pixelIndex = visualizerPixelIndex(x, y);

      if (pixelIndex >= 0) {
        strip.setPixelColor(
          pixelIndex,
          strip.Color(brightness, brightness, brightness)
        );
      }
    }
  }

  strip.show();
}

void smoothVisualizer() {
  for (int i = 0; i < NUM_BANDS; i++) {

    float target = bandLevel[i];

    if (target > visualizerLevels[i]) {
      // Fast attack
      visualizerLevels[i] +=
        (target - visualizerLevels[i]) * ATTACK_SPEED;
    } else {
      // Slow decay
      visualizerLevels[i] +=
        (target - visualizerLevels[i]) * DECAY_SPEED;
    }

    visualizerLevels[i] = constrain(
      visualizerLevels[i], 0.0f, 1.0f
    );
  }
}

void saveSettings() {
  File f = SD.open(SETTINGS_FILE, FILE_WRITE);
  if (!f) return;
  StaticJsonDocument<128> doc;
  doc["neoEnabled"]    = neoEnabled;
  doc["neoBrightness"] = neoBrightness;
  doc["volume"]        = volume;
  serializeJson(doc, f);
  f.close();
}

void loadSettings() {
  if (!SD.exists(SETTINGS_FILE)) return;
  File f = SD.open(SETTINGS_FILE, FILE_READ);
  if (!f) return;
  StaticJsonDocument<128> doc;
  auto err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  neoEnabled    = doc["neoEnabled"]    | true;   // default true if missing
  neoBrightness = doc["neoBrightness"] | 80;
  volume        = doc["volume"]        | 15;
  fadeBrightness = neoBrightness;
}

void initRFID() {
  bool ok = mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(MFRC522::PCD_RxGain::RxGain_max);
  Serial.printf("[rfid] PCD_Init returned %d\n", ok);
}

bool rfidHealthy() {
  byte ver = driver.PCD_ReadRegister(MFRC522::PCD_Register::VersionReg);
  byte tx  = driver.PCD_ReadRegister(MFRC522::PCD_Register::TxControlReg);
  bool ok = ver != 0x00 && ver != 0xFF && (tx & 0x03) == 0x03;
  if (!ok) Serial.printf("[rfid] UNHEALTHY ver=0x%02X tx=0x%02X\n", ver, tx);
  return ok;
}
