#include <Audio.h>
#include <Wire.h>
#include <SD.h>
#include <SPI.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include "paj7620.h"
#include <Stepper.h>
#include <Adafruit_NeoPixel.h>


// ------------------- PIN DEFINITIONS -------------------
#define HALL_PIN      21

#define SD_CS         38
#define SPI_MOSI      11 
#define SPI_MISO      13
#define SPI_SCK       12

#define I2S_DOUT      5
#define I2S_BCLK      6
#define I2S_LRC       7

#define IN1 18
#define IN2 17
#define IN3 16
#define IN4 15

#define LED_PIN   48
#define LED_COUNT 24
#define AUDIO_ADC 4

// ------------------- RFID -------------------
MFRC522DriverPinSimple ss_pin(5);
MFRC522DriverSPI driver{ss_pin};
MFRC522 mfrc522{driver};

// ------------------- AUDIO -------------------
Audio audio;
int volume = 5;
int smoothedLevel = 0;

// NeoPixel
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ------------------ STEPPER ------------------
const int stepsPerRevolution = 2048;  
Stepper myStepper(stepsPerRevolution, IN1, IN3, IN2, IN4);
int speed = 5;
unsigned long lastStepTime = 0;
const unsigned long stepIntervalMs = 5;
// stepIntervalMs = map(volume, 0, 21, 10, 3);

// ------------------- STATE -------------------
String currentUID = "";
bool isPlaying = false;

// ------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Stepper
  myStepper.setSpeed(speed);

  // Hall Effect
  pinMode(HALL_PIN, INPUT);

  // NeoPixel
  strip.begin();
  strip.show();
  strip.setBrightness(80);
  pinMode(AUDIO_ADC, INPUT);

  // SD
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  if (!SD.begin(SD_CS)) {
    Serial.println("SD card failed!");
    while (true);
  }

  // Audio
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(volume);

  // Gesture
  paj7620Init();

  // RFID
  mfrc522.PCD_Init();
  MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);

  Serial.println("Ready.");
}

// ------------------------------------------------------

void loop() {
  handleHallSensor();
  handleRFID();
  handleGestures();
  handleStepper();
  updateVisualizer();


  audio.loop();
}

// ------------------------------------------------------

void handleHallSensor() {
  bool recordPresent = digitalRead(HALL_PIN);

  if (!recordPresent && isPlaying) {
    Serial.println("Record removed → stop");
    audio.stopSong();
    isPlaying = false;
    currentUID = "";
  }
}

// ------------------------------------------------------

void handleRFID() {
  if (!digitalRead(HALL_PIN)) return; // no record, ignore RFID

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  String newUID = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) newUID += "0";
    newUID += String(mfrc522.uid.uidByte[i], HEX);
  }

  if (newUID == currentUID) return;

  Serial.print("New RFID: ");
  Serial.println(newUID);

  currentUID = newUID;

  // TODO: map UID → filename
  audio.stopSong();
  audio.connecttoFS(SD, "/MYMUSIC.mp3");
  isPlaying = true;


  mfrc522.PICC_HaltA();
}

// ------------------------------------------------------

void handleGestures() {
  uint8_t data = 0;
  paj7620ReadReg(0x43, 1, &data);

  if (data == GES_UP_FLAG && volume < 21) {
    volume++;
    audio.setVolume(volume);
  }

  if (data == GES_DOWN_FLAG && volume > 0) {
    volume--;
    audio.setVolume(volume);
  }
}

void handleStepper() {
  if (!isPlaying) return;

  unsigned long now = millis();
  if (now - lastStepTime >= stepIntervalMs) {
    myStepper.step(1);   // single step only
    lastStepTime = now;
  }
}

int readAudioLevel() {
  int raw = analogRead(AUDIO_ADC);   // 0–4095
  raw = abs(raw - 2048);              // center around mid psychologist
  return raw;
}

int getSmoothedLevel() {
  int level = readAudioLevel();
  smoothedLevel = (smoothedLevel * 7 + level) / 8;
  return smoothedLevel;
}

void updateVisualizer() {
  if (!isPlaying) {
    strip.clear();
    strip.show();
    return;
  }

  int level = getSmoothedLevel();
  int bars = map(level, 0, 1500, 0, 8);
  bars = constrain(bars, 0, 8);

  strip.clear();

  for (int col = 0; col < 3; col++) {
    for (int row = 0; row < bars; row++) {
      int index = col * 8 + row;

      uint32_t color;
      if (row < 5) color = strip.Color(0, 255, 0);
      else if (row < 7) color = strip.Color(255, 150, 0);
      else color = strip.Color(255, 0, 0);

      strip.setPixelColor(index, color);
    }
  }

  strip.show();
}
