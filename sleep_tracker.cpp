/*
  Arduino UNO R4 WiFi - Sleep Tracker -> ThingSpeak

  Sensors:
   - Pulse Sensor (BPM)      : A0
   - KY-037 Mic (noise RMS)  : A1 (analog out)
   - DHT11 (temp/humidity)   : D2
   - Light sensor (analog)   : A2
  Time:
   - NTP epoch (Unix time)

  ThingSpeak fields:
   Field 1 = BPM (filtered)
   Field 2 = Noise RMS
   Field 3 = Temperature (C)
   Field 4 = Humidity (%)
   Field 5 = Light (analog)
   Field 6 = Epoch (seconds)
   Field 7 = SleepState (0/1)

  ThingSpeak free tier: >= 15s update interval (using 20s)
*/

#include <WiFiS3.h>
#include <ThingSpeak.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

#include <DHT.h>
#include <PulseSensorPlayground.h>

// -------------------- USER CONFIG --------------------
const char* WIFI_SSID = "Yi";
const char* WIFI_PASS = "yodaniel04";

// ThingSpeak
unsigned long THINGSPEAK_CHANNEL_ID = 3204174;
const char* THINGSPEAK_WRITE_API_KEY = "N4X3LKJL3LU6NWXH";

// -------------------- PIN CONFIG ---------------------
static const int PIN_PULSE = A0;
static const int PIN_MIC   = A1;
static const int PIN_LIGHT = A2;
static const int PIN_DHT   = 2;

#define DHTTYPE DHT11

// -------------------- SLEEP STATE THRESHOLDS ---------
// Tune these with your real data:
const int   LIGHT_SLEEP_TH = 180;   // lower = darker
const float NOISE_SLEEP_TH = 2.5;   // lower = quieter (your RMS scale)
const int   BPM_SLEEP_MAX  = 65;    // resting bpm upper bound

// -------------------- OBJECTS ------------------------
WiFiClient client;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0 /*UTC offset seconds*/, 60 * 1000);

DHT dht(PIN_DHT, DHTTYPE);
PulseSensorPlayground pulseSensor;

// -------------------- TIMING -------------------------
const unsigned long UPLOAD_INTERVAL_MS = 20000; // 20s
unsigned long lastUploadMs = 0;

// -------------------- HELPERS ------------------------
float readNoiseLevelRMS(int analogPin, int samples = 100, int delayUs = 150) {
  long sum = 0;
  long sumSq = 0;

  for (int i = 0; i < samples; i++) {
    int v = analogRead(analogPin);
    sum += v;
    delayMicroseconds(delayUs);
  }
  int mean = (int)(sum / samples);

  for (int i = 0; i < samples; i++) {
    int v = analogRead(analogPin);
    int centered = v - mean;
    sumSq += (long)centered * (long)centered;
    delayMicroseconds(delayUs);
  }

  float meanSq = (float)sumSq / (float)samples;
  return sqrt(meanSq);
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nWiFi connect timeout.");
      return false;
    }
  }

  Serial.println("\nWiFi connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

unsigned long getEpochTimeSafe() {
  if (!timeClient.isTimeSet()) return 0;
  return timeClient.getEpochTime();
}

// -------------------- SETUP --------------------------
void setup() {
  Serial.begin(115200);
  delay(1500);

  dht.begin();

  // Pulse sensor
  pulseSensor.analogInput(PIN_PULSE);

  // Start here, tune if needed:
  pulseSensor.setThreshold(600);

  if (!pulseSensor.begin()) {
    Serial.println("PulseSensor init failed. Check wiring.");
  }

  if (connectWiFi()) {
    ThingSpeak.begin(client);
    timeClient.begin();
    timeClient.update();
  }
}

// -------------------- LOOP ---------------------------
void loop() {
  // Keep WiFi alive
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Keep NTP time fresh
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
  }

  // ---- BPM: process continuously ----
  static int bpmStable = 0;
  static unsigned long lastBeatMs = 0;

  int bpmNow = pulseSensor.getBeatsPerMinute();
  if (pulseSensor.sawStartOfBeat()) {
    unsigned long now = millis();

    // Ignore beats too close together (>200 BPM) + clamp to human range
    if ((now - lastBeatMs) >= 300 && bpmNow >= 40 && bpmNow <= 180) {
      bpmStable = bpmNow;
      lastBeatMs = now;
    }
  }

  // Only upload every 20 seconds
  if (millis() - lastUploadMs < UPLOAD_INTERVAL_MS) {
    delay(10);
    return;
  }
  lastUploadMs = millis();

  // ---- Noise (RMS) ----
  float noiseLevel = readNoiseLevelRMS(PIN_MIC);

  // ---- DHT (with retry) ----
  float humidity = dht.readHumidity();
  float temperatureC = dht.readTemperature();

  if (isnan(humidity) || isnan(temperatureC)) {
    delay(2000); // DHT11 needs time
    humidity = dht.readHumidity();
    temperatureC = dht.readTemperature();
  }
  bool dhtOk = !(isnan(humidity) || isnan(temperatureC));

  // ---- Light ----
  int lightAnalog = analogRead(PIN_LIGHT);

  // ---- Time ----
  unsigned long epoch = getEpochTimeSafe();

  // ---- SleepState (simple rule) ----
  int sleepState = 0;
  if (bpmStable > 0 &&
      bpmStable <= BPM_SLEEP_MAX &&
      noiseLevel <= NOISE_SLEEP_TH &&
      lightAnalog <= LIGHT_SLEEP_TH) {
    sleepState = 1;
  }

  // Debug print
  Serial.println("---- Sensor Readings ----");
  Serial.print("BPM (stable): "); Serial.println(bpmStable);
  Serial.print("Noise RMS: "); Serial.println(noiseLevel, 2);
  Serial.print("Temp C: "); Serial.println(dhtOk ? temperatureC : -999);
  Serial.print("Humidity: "); Serial.println(dhtOk ? humidity : -999);
  Serial.print("Light A: "); Serial.println(lightAnalog);
  Serial.print("Epoch: "); Serial.println(epoch);
  Serial.print("SleepState: "); Serial.println(sleepState);

  // Upload
  if (WiFi.status() == WL_CONNECTED) {
    ThingSpeak.setField(1, bpmStable);
    ThingSpeak.setField(2, noiseLevel);
    ThingSpeak.setField(3, dhtOk ? temperatureC : -999);
    ThingSpeak.setField(4, dhtOk ? humidity : -999);
    ThingSpeak.setField(5, lightAnalog);
    ThingSpeak.setField(6, (long)epoch);
    ThingSpeak.setField(7, sleepState);

    int httpCode = ThingSpeak.writeFields(THINGSPEAK_CHANNEL_ID, THINGSPEAK_WRITE_API_KEY);
    if (httpCode == 200) Serial.println("ThingSpeak update OK.");
    else {
      Serial.print("ThingSpeak update failed, HTTP code: ");
      Serial.println(httpCode);
    }
  } else {
    Serial.println("Skipping upload: WiFi not connected.");
  }
}