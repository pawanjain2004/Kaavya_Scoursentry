/*
  ============================================================================
   SCOUR SENTRY  -  Bridge pier scour early-warning node (ESP32)
  ============================================================================
   Hardware
     MPU6050   : SDA 21, SCL 22
     SD card   : CS 5, MOSI 23, CLK 18, MISO 19 (3V3)
     LEDs      : RED 25, YELLOW 26, GREEN 27 (active HIGH)
     Buzzer    : S 14 (active HIGH)

   MPU outputs (same calibrated algorithm as Mpu test code/sketch_nov4a)
     - Angle (deg)      complementary filter + zero/scale calibration
     - Frequency (Hz)   FFT dominant peak every ~5.12 s
     - Vibration (mg)   RMS + peak from the same window

   Alarms use those three values vs thresholds -> SAFE / WARNING / DANGER
   LEDs + buzzer follow the level. SD logs continuously; when the card
   is full (or the log is too large) the oldest file is rotated away.

   Serial (115200)
     Z : set ZERO (flat)          N : lock +90 and save MPU cal
     C : clear MPU calibration    F : save current freq as baseline
     X : clear baseline frequency M : mute / unmute buzzer
     P : status                   D : raw debug   H : help
  ============================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <string.h>

#include "secrets.h"

// Forward decls (Arduino auto-prototypes run before struct bodies)
struct LiveState;
struct EventRec;

// ============================================================================
//  USER CONFIG
// ============================================================================
#define DEVICE_ID       "pier-01"
#define GMT_OFFSET_SEC  19800

// Pins
#define MPU_ADDR    0x68
#define I2C_SDA     21
#define I2C_SCL     22
#define SD_CS       5
#define SD_MOSI     23
#define SD_SCK      18
#define SD_MISO     19
#define LED_RED     25
#define LED_YELLOW  26
#define LED_GREEN   27
#define BUZZER_PIN  14
#define BUZZER_ON   HIGH
#define BUZZER_OFF  LOW

// MPU / FFT (same as calibrated sketch)
#define FILTER_ALPHA    0.96f
#define SAMPLE_RATE_HZ  200
#define FFT_SIZE        1024
#define FREQ_MIN_HZ     0.5f
#define FREQ_MAX_HZ     25.0f
#define MIN_PEAK_MG     0.15f   // was 0.5 — lab peaks are often weaker
#define MIN_PEAK_SNR    1.8f    // was 4.0 — still require peak above band noise
#define SPECTRUM_POINTS 64

// Alarm thresholds (final criteria)
// Angle: SAFE 80–100°, WARNING 70–80 & 100–110°, DANGER ≤70° or ≥110°
#define ANGLE_SAFE_LO          80.0f
#define ANGLE_SAFE_HI          100.0f
#define ANGLE_WARN_LO          70.0f
#define ANGLE_WARN_HI         110.0f
// Frequency (Hz): ≤20 SAFE, ≤50 WARNING, >50 DANGER
#define FREQ_WARN_HZ           20.0f
#define FREQ_DANGER_HZ         50.0f
// Vibration (mg): <200 SAFE, <500 WARNING, ≥500 DANGER
#define VIB_WARN_MG           200.0f
#define VIB_DANGER_MG         500.0f
#define ALARM_CONFIRM_COUNT     3

static int angleLevel(float a) {
  if (!isfinite(a)) return 0;
  if (a <= ANGLE_WARN_LO || a >= ANGLE_WARN_HI) return 2;                 // danger
  if (a < ANGLE_SAFE_LO || a > ANGLE_SAFE_HI) return 1;                   // warning bands
  return 0;                                                              // 80–100 safe
}

static int freqLevel(float f, bool ready) {
  if (!ready || !isfinite(f)) return 0;
  if (f > FREQ_DANGER_HZ) return 2;
  if (f > FREQ_WARN_HZ) return 1;
  return 0;
}

static int vibLevel(float v) {
  if (!isfinite(v)) return 0;
  if (v >= VIB_DANGER_MG) return 2;
  if (v >= VIB_WARN_MG) return 1;
  return 0;
}

// Timing / SD / Firebase retention
#define LIVE_UPLOAD_MS     2000
#define HISTORY_UPLOAD_MS  10000
#define FB_KEEP_MS         (5UL * 60UL * 1000UL)     // keep only last 5 minutes on Firebase
#define FB_HISTORY_MAX     36                        // 5 min @ 10 s
#define FB_EVENTS_MAX      20
#define WIFI_RETRY_MS      15000
#define SD_RETRY_MS        10000
#define STATUS_PRINT_MS    500
#define EVAL_MS            1000
#define MAX_LOG_BYTES      (2UL * 1024UL * 1024UL)   // rotate after ~2 MB
#define MIN_SD_FREE_BYTES  (64UL * 1024UL)            // rotate if free < 64 KB

#define LOG_FILE     "/scour_log.csv"
#define LOG_OLD_FILE "/scour_log_old.csv"
#define EVENT_FILE   "/scour_events.csv"

// ============================================================================
//  STATE
// ============================================================================
struct LiveState {
  float angle, freq, vibRms, vibPeak, baselineFreq, freqChangePct;
  bool  freqValid, mpuOk, sdOk, angleCal, freqCal, calibrating;
  bool  buzzer, muted;
  int   level;
  char  alerts[40];
  char  dataNote[96];
  float sampleRate;
  float spectrum[SPECTRUM_POINTS];
  float spectrumDf;
  int   spectrumGroup;
  bool  spectrumReady;
  uint32_t uptimeS;
};

struct EventRec {
  int level;
  char alerts[40];
  float angle, freq, vibRms;
  uint32_t epoch;       // unix seconds when known, else 0
  uint32_t tsMs;        // epoch ms used as Firebase key / prune age
};

struct HistRec {
  uint32_t tsMs;
  float angle, freq, vibRms;
  int level;
};

static Preferences prefs;
static SemaphoreHandle_t dataMutex;
static QueueHandle_t eventQueue;

static LiveState gLive;
static volatile bool wifiOk = false;

// --- calibrated MPU state (from sketch_nov4a) ---
static float zeroAngle = 0;
static float scaleFactor = 1.0f;
static float gyroBiasX = 0;
static float filteredAngle = 0;
static float deviation = 0;
static bool filterStarted = false;
static bool angleCalSaved = false;
static bool showDebug = false;

static float dominantFreqHz = 0;
static float vibRmsMg = 0;
static float vibPeakMg = 0;
static float freqPeakAmpMg = 0;   // amplitude of chosen FFT bin (mg)
static bool freqValid = false;    // peak clear enough for alarms / baseline
static bool freqReady = false;    // at least one FFT window finished
static float lastFs = SAMPLE_RATE_HZ;

static float sampleBuf[3][FFT_SIZE];
static int sampleIdx = 0;
static uint32_t windowStartUs = 0;
static uint32_t nextSampleUs = 0;

static float fftRe[FFT_SIZE], fftIm[FFT_SIZE], hannWin[FFT_SIZE];
static float ampSq[FFT_SIZE / 2 + 1];

static float baselineFreq = NAN;
static bool freqCal = false;

static bool sensorOk = false;
static bool sdOk = false;
static uint32_t lastSdRetry = 0;

static int alarmLevel = 0;
static int pendingLevel = 0, pendingCount = 0;
static char alertStr[40] = "";
static bool buzzerMuted = false;
static bool buzzerActive = false;

static float spectrumOut[SPECTRUM_POINTS];
static float spectrumDf = 0;
static int spectrumGroup = 1;
static bool spectrumFresh = false;

// Rolling Firebase windows (last 5 minutes only)
static HistRec histBuf[FB_HISTORY_MAX];
static int histCount = 0;
static EventRec eventBuf[FB_EVENTS_MAX];
static int eventCount = 0;

static uint32_t fbNowMs() {
  if (timeSynced()) return (uint32_t)((uint64_t)time(nullptr) * 1000ULL);
  // Before NTP: synthetic timeline so pruning still works within a boot
  return 1700000000000UL + millis();
}

static void histPruneAndPush(const LiveState &s) {
  uint32_t now = fbNowMs();
  uint32_t cut = (now > FB_KEEP_MS) ? (now - FB_KEEP_MS) : 0;
  int w = 0;
  for (int i = 0; i < histCount; i++) {
    if (histBuf[i].tsMs >= cut) histBuf[w++] = histBuf[i];
  }
  histCount = w;
  if (histCount == FB_HISTORY_MAX) {
    memmove(&histBuf[0], &histBuf[1], sizeof(HistRec) * (FB_HISTORY_MAX - 1));
    histCount = FB_HISTORY_MAX - 1;
  }
  HistRec h;
  h.tsMs = now;
  h.angle = s.angle;
  h.freq = s.freq;
  h.vibRms = s.vibRms;
  h.level = s.level;
  histBuf[histCount++] = h;
}

static void eventPruneAndPush(const EventRec &ev) {
  uint32_t now = ev.tsMs ? ev.tsMs : fbNowMs();
  uint32_t cut = (now > FB_KEEP_MS) ? (now - FB_KEEP_MS) : 0;
  int w = 0;
  for (int i = 0; i < eventCount; i++) {
    if (eventBuf[i].tsMs >= cut) eventBuf[w++] = eventBuf[i];
  }
  eventCount = w;
  if (eventCount == FB_EVENTS_MAX) {
    memmove(&eventBuf[0], &eventBuf[1], sizeof(EventRec) * (FB_EVENTS_MAX - 1));
    eventCount = FB_EVENTS_MAX - 1;
  }
  EventRec e = ev;
  e.tsMs = now;
  eventBuf[eventCount++] = e;
}

// ============================================================================
//  HELPERS
// ============================================================================
static float wrapAngle(float angle) {
  while (angle > 180) angle -= 360;
  while (angle < -180) angle += 360;
  return angle;
}

static bool timeSynced() { return time(nullptr) > 1700000000; }
static bool ntpStarted = false;

static void startNtpIfNeeded() {
  if (ntpStarted) return;
  if (!cloudConfigured()) return;
  configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
  ntpStarted = true;
  Serial.println("[TIME] NTP started (IST UTC+5:30). Waiting for sync...");
}

// Always fills date + time for SD CSV (real clock after NTP, else UNSYNCED + uptime clock)
static void dateTimeStrings(char *dateOut, size_t dateLen, char *timeOut, size_t timeLen) {
  if (timeSynced()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(dateOut, dateLen, "%Y-%m-%d", &t);
    strftime(timeOut, timeLen, "%H:%M:%S", &t);
  } else {
    uint32_t s = millis() / 1000UL;
    snprintf(dateOut, dateLen, "UNSYNCED");
    snprintf(timeOut, timeLen, "%02lu:%02lu:%02lu",
             (unsigned long)((s / 3600UL) % 100UL),
             (unsigned long)((s / 60UL) % 60UL),
             (unsigned long)(s % 60UL));
  }
}

static void timeString(char *out, size_t len) {
  char d[16], t[16];
  dateTimeStrings(d, sizeof(d), t, sizeof(t));
  snprintf(out, len, "%s %s", d, t);
}

static void jAddNum(String &j, const char *key, float v, int dec) {
  if (!isfinite(v)) return;
  j += ",\""; j += key; j += "\":"; j += String(v, dec);
}
static void jAddBool(String &j, const char *key, bool v) {
  j += ",\""; j += key; j += "\":"; j += v ? "true" : "false";
}
static void jAddStr(String &j, const char *key, const char *v) {
  j += ",\""; j += key; j += "\":\""; j += v; j += "\"";
}

static const char *levelName(int l) {
  return l >= 2 ? "DANGER" : (l == 1 ? "WARNING" : "SAFE");
}

// ============================================================================
//  MPU (same as calibrated sketch)
// ============================================================================
static bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool readAccelG(float &ax, float &ay, float &az) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)6) != 6) return false;
  uint8_t b0 = Wire.read(), b1 = Wire.read();
  uint8_t b2 = Wire.read(), b3 = Wire.read();
  uint8_t b4 = Wire.read(), b5 = Wire.read();
  ax = (int16_t)((b0 << 8) | b1) / 16384.0f;
  ay = (int16_t)((b2 << 8) | b3) / 16384.0f;
  az = (int16_t)((b4 << 8) | b5) / 16384.0f;
  return true;
}

static bool readGyroDps(float &gx, float &gy, float &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x43);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)6) != 6) return false;
  uint8_t b0 = Wire.read(), b1 = Wire.read();
  uint8_t b2 = Wire.read(), b3 = Wire.read();
  uint8_t b4 = Wire.read(), b5 = Wire.read();
  gx = (int16_t)((b0 << 8) | b1) / 131.0f;
  gy = (int16_t)((b2 << 8) | b3) / 131.0f;
  gz = (int16_t)((b4 << 8) | b5) / 131.0f;
  return true;
}

static float accelAngleX() {
  float sx = 0, sy = 0, sz = 0;
  int count = 0;
  for (int i = 0; i < 40; i++) {
    float ax, ay, az;
    if (readAccelG(ax, ay, az)) {
      sx += ax; sy += ay; sz += az; count++;
    }
    delay(5);
  }
  if (count == 0) return NAN;
  return atan2(-sy / count, sz / count) * 180.0f / PI;
}

static bool mpuInit() {
  Wire.beginTransmission(MPU_ADDR);
  if (Wire.endTransmission() != 0) return false;
  mpuWrite(0x6B, 0x00);   // wake
  mpuWrite(0x19, 0x04);   // 200 Hz
  mpuWrite(0x1A, 0x03);   // DLPF ~44 Hz
  mpuWrite(0x1C, 0x00);   // +/-2g
  delay(50);
  return true;
}

static void saveAngleCal() {
  prefs.begin("mpu6050", false);
  prefs.putFloat("zero", zeroAngle);
  prefs.putFloat("scale", scaleFactor);
  prefs.putFloat("biasX", gyroBiasX);
  prefs.putBool("valid", true);
  prefs.end();
  angleCalSaved = true;
  Serial.println("[CAL] Angle calibration SAVED to flash.");
}

static bool loadAngleCal() {
  prefs.begin("mpu6050", true);
  bool valid = prefs.getBool("valid", false);
  if (valid) {
    zeroAngle = prefs.getFloat("zero", 0);
    scaleFactor = prefs.getFloat("scale", 1.0f);
    gyroBiasX = prefs.getFloat("biasX", 0);
  }
  prefs.end();
  if (!valid) return false;
  if (fabsf(scaleFactor - 1.0f) < 0.0001f) return false;
  angleCalSaved = true;
  filteredAngle = zeroAngle;
  filterStarted = true;
  Serial.printf("[CAL] Loaded angle cal  zero=%.2f  scale=%.4f  biasX=%.2f\n",
                zeroAngle, scaleFactor, gyroBiasX);
  return true;
}

static void clearAngleCal() {
  prefs.begin("mpu6050", false);
  prefs.clear();
  prefs.end();
  zeroAngle = 0;
  scaleFactor = 1.0f;
  gyroBiasX = 0;
  angleCalSaved = false;
  Serial.println("[CAL] Angle calibration CLEARED. Do Z then N again.");
}

static void saveBaselineFreq() {
  prefs.begin("sentry", false);
  prefs.putBool("hasFreq", true);
  prefs.putFloat("f0", baselineFreq);
  prefs.end();
  freqCal = true;
}

static void loadBaselineFreq() {
  prefs.begin("sentry", true);
  freqCal = prefs.getBool("hasFreq", false);
  baselineFreq = prefs.getFloat("f0", NAN);
  prefs.end();
  if (!isfinite(baselineFreq)) freqCal = false;
  if (freqCal) Serial.printf("[CAL] Baseline frequency loaded: %.3f Hz\n", baselineFreq);
}

static void clearBaselineFreq() {
  prefs.begin("sentry", false);
  prefs.remove("hasFreq");
  prefs.remove("f0");
  prefs.end();
  freqCal = false;
  baselineFreq = NAN;
  Serial.println("[CAL] Baseline frequency cleared.");
}

static void calibrateGyroBias() {
  float sx = 0;
  int count = 0;
  for (int i = 0; i < 100; i++) {
    float gx, gy, gz;
    if (readGyroDps(gx, gy, gz)) { sx += gx; count++; }
    delay(10);
  }
  if (count == 0) {
    Serial.println("[CAL] Gyro bias FAILED");
    return;
  }
  gyroBiasX = sx / count;
  Serial.printf("[CAL] Gyro bias X: %.2f deg/s\n", gyroBiasX);
}

static void calibrateZero() {
  Serial.println("\n[CAL] Keep sensor FLAT (0 deg), still...");
  delay(1500);
  calibrateGyroBias();
  float angle = accelAngleX();
  if (isnan(angle)) {
    Serial.println("[CAL] Zero calibration FAILED");
    return;
  }
  zeroAngle = angle;
  filteredAngle = angle;
  filterStarted = true;
  scaleFactor = 1.0f;
  angleCalSaved = false;
  Serial.println("[CAL] ZERO set.");
  Serial.printf("[CAL] Reference: %.2f deg\n", zeroAngle);
  Serial.println("[CAL] Hold +90 and send N to finish & save.");
}

static void calibrateNinety() {
  float measured = wrapAngle(filteredAngle - zeroAngle);
  if (fabsf(measured) < 45.0f) {
    Serial.printf("\n[CAL] NOW reading only %.2f deg — tilt closer to +90, then send N.\n",
                  measured);
    return;
  }
  scaleFactor = 90.0f / measured;
  Serial.println("\n========== 90 DEG LOCKED ==========");
  Serial.printf("Was reading: %.2f deg\n", measured);
  Serial.printf("Scale set to: %.4f\n", scaleFactor);
  Serial.println("===================================\n");
  saveAngleCal();
}

// ---------- FFT (same as sketch_nov4a) ----------
static void fftInPlace(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    int half = len >> 1;
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < half; k++) {
        int a = i + k, b = a + half;
        float tr = re[b] * cr - im[b] * ci;
        float ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr;  im[b] = im[a] - ti;
        re[a] += tr;         im[a] += ti;
        float ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

static void initHann() {
  for (int i = 0; i < FFT_SIZE; i++)
    hannWin[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_SIZE - 1)));
}

static void analyzeFrequency(float fs) {
  const int N = FFT_SIZE;
  const int H = N / 2;
  if (fs < SAMPLE_RATE_HZ * 0.8f || fs > SAMPLE_RATE_HZ * 1.2f) fs = SAMPLE_RATE_HZ;
  float df = fs / N;
  lastFs = fs;

  memset(ampSq, 0, sizeof(ampSq));
  float mean[3];
  double varSum = 0;

  for (int ax = 0; ax < 3; ax++) {
    double m = 0;
    for (int i = 0; i < N; i++) m += sampleBuf[ax][i];
    mean[ax] = m / N;
    for (int i = 0; i < N; i++) {
      float d = sampleBuf[ax][i] - mean[ax];
      varSum += (double)d * d;
      fftRe[i] = d * hannWin[i];
      fftIm[i] = 0;
    }
    fftInPlace(fftRe, fftIm, N);
    const float scale = 16.0f / ((float)N * N);
    for (int k = 1; k <= H; k++)
      ampSq[k] += (fftRe[k] * fftRe[k] + fftIm[k] * fftIm[k]) * scale;
  }

  float peak = 0;
  for (int i = 0; i < N; i++) {
    float dx = sampleBuf[0][i] - mean[0];
    float dy = sampleBuf[1][i] - mean[1];
    float dz = sampleBuf[2][i] - mean[2];
    float mag = sqrtf(dx * dx + dy * dy + dz * dz);
    if (mag > peak) peak = mag;
  }

  for (int k = 0; k <= H; k++) ampSq[k] = sqrtf(ampSq[k]) * 1000.0f;
  ampSq[0] = 0;

  int kMin = max(1, (int)ceilf(FREQ_MIN_HZ / df));
  int kMax = min(H - 1, (int)floorf(FREQ_MAX_HZ / df));
  int kPk = kMin;
  double bandSum = 0;
  for (int k = kMin; k <= kMax; k++) {
    bandSum += ampSq[k];
    if (ampSq[k] > ampSq[kPk]) kPk = k;
  }
  float bandAvg = bandSum / (kMax - kMin + 1);

  float a = ampSq[kPk - 1], c = ampSq[kPk], e = ampSq[kPk + 1];
  float denom = a - 2 * c + e;
  float delta = (fabsf(denom) > 1e-9f) ? 0.5f * (a - e) / denom : 0;
  if (delta > 0.5f) delta = 0.5f;
  if (delta < -0.5f) delta = -0.5f;

  dominantFreqHz = (kPk + delta) * df;
  vibRmsMg = sqrt(varSum / N) * 1000.0f;
  vibPeakMg = peak * 1000.0f;
  freqPeakAmpMg = c;
  freqReady = true;
  // Accept peak if strong enough vs noise; always keep dominantFreqHz for display
  freqValid = (c >= MIN_PEAK_MG) && (bandAvg <= 1e-6f || c >= MIN_PEAK_SNR * bandAvg);

  int group = (kMax + SPECTRUM_POINTS) / SPECTRUM_POINTS;
  spectrumGroup = group;
  spectrumDf = df;
  for (int p = 0; p < SPECTRUM_POINTS; p++) {
    float m = 0;
    for (int k = p * group; k < (p + 1) * group && k <= H; k++)
      if (ampSq[k] > m) m = ampSq[k];
    spectrumOut[p] = m;
  }
  spectrumFresh = true;
}

// ============================================================================
//  SD (circular: rotate when full / too large)
// ============================================================================
static void sdWriteHeader(File &f) {
  f.println("date,time,uptime_s,angle_deg,freq_hz,freq_valid,baseline_hz,freq_change_pct,vib_rms_mg,vib_peak_mg,level,alerts");
}

static bool sdEnsureLog() {
  if (!SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (!f) return false;
    sdWriteHeader(f);
    f.close();
  }
  if (!SD.exists(EVENT_FILE)) {
    File f = SD.open(EVENT_FILE, FILE_WRITE);
    if (f) {
      f.println("date,time,uptime_s,level,alerts,angle_deg,freq_hz,vib_rms_mg");
      f.close();
    }
  }
  return true;
}

static void sdRotateIfNeeded() {
  if (!sdOk) return;
  uint64_t total = SD.totalBytes();
  uint64_t used = SD.usedBytes();
  uint64_t freeB = (total > used) ? (total - used) : 0;

  File f = SD.open(LOG_FILE, FILE_READ);
  size_t sz = f ? f.size() : 0;
  if (f) f.close();

  bool needRotate = (sz >= MAX_LOG_BYTES) || (freeB < MIN_SD_FREE_BYTES);
  if (!needRotate) return;

  Serial.printf("[SD] Rotating log (size=%u free=%llu). Oldest backup removed.\n",
                (unsigned)sz, (unsigned long long)freeB);
  SD.remove(LOG_OLD_FILE);
  SD.rename(LOG_FILE, LOG_OLD_FILE);
  File nf = SD.open(LOG_FILE, FILE_WRITE);
  if (nf) {
    sdWriteHeader(nf);
    nf.close();
  }
}

static bool sdInit() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 4000000)) return false;
  return sdEnsureLog();
}

static void sdMaintain() {
  if (sdOk || millis() - lastSdRetry < SD_RETRY_MS) return;
  lastSdRetry = millis();
  SD.end();
  sdOk = sdInit();
  if (sdOk) Serial.println("[SD] Card mounted.");
}

static void sdLogRow(float angle, float freq, bool fv, float vibRms, float vibPeak, float freqChange) {
  if (!sdOk) return;
  sdRotateIfNeeded();
  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[SD] Write failed — rotating / remounting...");
    sdOk = false;
    lastSdRetry = 0;
    sdMaintain();
    if (!sdOk) return;
    f = SD.open(LOG_FILE, FILE_APPEND);
    if (!f) return;
  }
  char dateBuf[16], timeBuf[16];
  dateTimeStrings(dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));
  f.printf("%s,%s,%lu,%.3f,%.3f,%d,%.3f,%.2f,%.2f,%.2f,%s,%s\n",
           dateBuf, timeBuf, millis() / 1000, angle, freq, fv ? 1 : 0,
           freqCal ? baselineFreq : NAN, freqChange,
           vibRms, vibPeak, levelName(alarmLevel), alertStr);
  f.close();
}

// ============================================================================
//  ALARM + OUTPUTS
// ============================================================================
static void publishLive() {
  LiveState s;
  memset(&s, 0, sizeof(s));
  s.angle = sensorOk ? deviation : NAN;
  s.freq = (sensorOk && freqReady) ? dominantFreqHz : NAN;
  s.freqValid = freqValid && sensorOk && freqReady;
  s.vibRms = sensorOk ? vibRmsMg : NAN;
  s.vibPeak = sensorOk ? vibPeakMg : NAN;
  s.baselineFreq = freqCal ? baselineFreq : NAN;
  s.freqChangePct = NAN;  // alarms use absolute Hz, not % drop
  s.mpuOk = sensorOk;
  s.sdOk = sdOk;
  s.angleCal = angleCalSaved;
  s.freqCal = freqCal;
  s.calibrating = !angleCalSaved;
  s.buzzer = buzzerActive;
  s.muted = buzzerMuted;
  s.level = alarmLevel;
  strncpy(s.alerts, alertStr, sizeof(s.alerts) - 1);
  if (!sensorOk)
    strncpy(s.dataNote, "MPU6050 NOT CONNECTED - no angle/frequency data", sizeof(s.dataNote) - 1);
  else if (!angleCalSaved)
    strncpy(s.dataNote, "Angle calibration incomplete - do Z then N", sizeof(s.dataNote) - 1);
  else
    strncpy(s.dataNote, "Live calibrated MPU6050 readings", sizeof(s.dataNote) - 1);
  s.sampleRate = lastFs;
  s.uptimeS = millis() / 1000;
  memcpy(s.spectrum, spectrumOut, sizeof(spectrumOut));
  s.spectrumDf = spectrumDf;
  s.spectrumGroup = spectrumGroup;
  s.spectrumReady = spectrumFresh;

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  gLive = s;
  xSemaphoreGive(dataMutex);
}

static void evaluateAlarm() {
  int al = 0, fl = 0, vl = 0;

  if (sensorOk) {
    al = angleLevel(deviation);
    fl = freqLevel(dominantFreqHz, freqReady);
    vl = vibLevel(vibRmsMg);
  }

  int raw = max(al, max(fl, vl));
  char buf[40] = "";
  if (al) strcat(buf, "ANGLE,");
  if (fl) strcat(buf, "FREQ,");
  if (vl) strcat(buf, "VIB,");
  if (!sensorOk) { strcat(buf, "SENSOR,"); raw = max(raw, 1); }
  size_t n = strlen(buf);
  if (n) buf[n - 1] = 0;

  if (raw == pendingLevel) pendingCount++;
  else { pendingLevel = raw; pendingCount = 1; }

  int prev = alarmLevel;
  if (pendingCount >= ALARM_CONFIRM_COUNT) alarmLevel = pendingLevel;

  if (alarmLevel > 0) strncpy(alertStr, buf, sizeof(alertStr));
  else alertStr[0] = 0;

  if (alarmLevel != prev) {
    buzzerMuted = false;
    Serial.printf("[ALARM] %s -> %s (%s)\n", levelName(prev), levelName(alarmLevel), alertStr);
    EventRec ev;
    memset(&ev, 0, sizeof(ev));
    ev.level = alarmLevel;
    strncpy(ev.alerts, alertStr, sizeof(ev.alerts) - 1);
    ev.angle = deviation;
    ev.freq = freqReady ? dominantFreqHz : NAN;
    ev.vibRms = vibRmsMg;
    ev.epoch = timeSynced() ? (uint32_t)time(nullptr) : 0;
    ev.tsMs = fbNowMs();
    xQueueSend(eventQueue, &ev, 0);

    if (sdOk) {
      File f = SD.open(EVENT_FILE, FILE_APPEND);
      if (f) {
        char dateBuf[16], timeBuf[16];
        dateTimeStrings(dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));
        f.printf("%s,%s,%lu,%s,%s,%.3f,%.3f,%.2f\n",
                 dateBuf, timeBuf, millis() / 1000, levelName(alarmLevel), alertStr,
                 ev.angle, ev.freq, ev.vibRms);
        f.close();
      }
    }
  }

  publishLive();
}

static void updateOutputs() {
  uint32_t ms = millis();
  bool r = false, y = false, g = false, buzz = false;

  if (!sensorOk) {
    y = (ms / 500) % 2;                          // sensor fault: blink yellow
  } else if (alarmLevel >= 2) {
    r = true;                                    // DANGER: red + continuous beep
    buzz = (ms % 400) < 200;
  } else if (alarmLevel == 1) {
    y = true;                                    // WARNING: yellow + two beeps
    uint32_t t = ms % 3000;
    buzz = (t < 120) || (t >= 250 && t < 370);   // beep-beep, then pause
  } else {
    g = true;                                    // SAFE: green, silent
  }
  if (!angleCalSaved) g = (ms / 250) % 2;

  buzzerActive = alarmLevel > 0 && !buzzerMuted;
  if (buzzerMuted) buzz = false;

  digitalWrite(LED_RED, r);
  digitalWrite(LED_YELLOW, y);
  digitalWrite(LED_GREEN, g);
  digitalWrite(BUZZER_PIN, buzz ? BUZZER_ON : BUZZER_OFF);
}

// ============================================================================
//  FIREBASE
// ============================================================================
static WiFiClientSecure tlsClient;
static HTTPClient http;

static bool cloudConfigured() {
  return strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0 &&
         strstr(FIREBASE_DB_URL, "YOUR-PROJECT") == nullptr;
}

static bool fbSend(const char *method, const char *path, const String &body) {
  String url = String(FIREBASE_DB_URL) + "/scourSentry/" + DEVICE_ID + path + ".json";
  if (strlen(FIREBASE_AUTH)) url += String("?auth=") + FIREBASE_AUTH;
  if (!http.begin(tlsClient, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, body);
  if (code != 200) {
    Serial.printf("[FB] FAIL %s %s -> %d\n", method, path, code);
  }
  http.end();
  return code == 200;
}

static String buildLiveJson(const LiveState &s) {
  // Only fields the website needs
  String j;
  j.reserve(700);
  j += "{\"ts\":{\".sv\":\"timestamp\"}";
  j += ",\"uptime\":" + String(s.uptimeS);
  jAddBool(j, "mpuConnected", s.mpuOk);
  jAddBool(j, "sensorOk", s.mpuOk);
  jAddBool(j, "sdConnected", s.sdOk);
  jAddBool(j, "sdOk", s.sdOk);
  jAddBool(j, "hasReadings", s.mpuOk && isfinite(s.angle));
  jAddStr(j, "dataNote", s.dataNote);
  jAddNum(j, "angle", s.angle, 2);
  jAddNum(j, "freq", s.freq, 2);
  jAddBool(j, "freqValid", s.freqValid);
  jAddNum(j, "vibRms", s.vibRms, 1);
  jAddNum(j, "vibPeak", s.vibPeak, 1);
  jAddNum(j, "sampleRate", s.sampleRate, 1);
  j += ",\"level\":" + String(s.level);
  jAddStr(j, "status", levelName(s.level));
  jAddStr(j, "alerts", s.alerts);
  jAddBool(j, "buzzer", s.buzzer);
  jAddBool(j, "muted", s.muted);
  jAddBool(j, "angleCal", s.angleCal);
  jAddBool(j, "tiltCal", s.angleCal);
  jAddBool(j, "calibrating", s.calibrating);
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"thresholds\":{";
  j += "\"angleSafeLo\":" + String(ANGLE_SAFE_LO, 1);
  j += ",\"angleSafeHi\":" + String(ANGLE_SAFE_HI, 1);
  j += ",\"angleWarnLo\":" + String(ANGLE_WARN_LO, 1);
  j += ",\"angleWarnHi\":" + String(ANGLE_WARN_HI, 1);
  j += ",\"freqWarn\":" + String(FREQ_WARN_HZ, 1);
  j += ",\"freqDanger\":" + String(FREQ_DANGER_HZ, 1);
  j += ",\"vibWarn\":" + String(VIB_WARN_MG, 1);
  j += ",\"vibDanger\":" + String(VIB_DANGER_MG, 1);
  j += "}}";
  return j;
}

// Replace entire /history node with only the last 5 minutes
static String buildHistoryBundle() {
  String j = "{";
  for (int i = 0; i < histCount; i++) {
    if (i) j += ",";
    const HistRec &h = histBuf[i];
    j += "\"";
    j += String(h.tsMs);
    j += "\":{\"ts\":";
    j += String(h.tsMs);
    jAddNum(j, "angle", h.angle, 2);
    jAddNum(j, "freq", h.freq, 2);
    jAddNum(j, "vibRms", h.vibRms, 1);
    j += ",\"level\":";
    j += String(h.level);
    j += "}";
  }
  j += "}";
  return j;
}

static String buildSpectrumJson(const LiveState &s) {
  String j;
  j.reserve(64 + SPECTRUM_POINTS * 8);
  j += "{\"ts\":{\".sv\":\"timestamp\"}";
  j += ",\"df\":" + String(s.spectrumDf, 5);
  j += ",\"group\":" + String(s.spectrumGroup);
  jAddNum(j, "peakFreq", s.freq, 2);
  jAddBool(j, "valid", s.freqValid);
  j += ",\"amp\":[";
  for (int i = 0; i < SPECTRUM_POINTS; i++) {
    if (i) j += ",";
    j += String(s.spectrum[i], 2);
  }
  j += "]}";
  return j;
}

// Replace entire /events node with only the last 5 minutes
static String buildEventsBundle() {
  String j = "{";
  for (int i = 0; i < eventCount; i++) {
    if (i) j += ",";
    const EventRec &e = eventBuf[i];
    j += "\"";
    j += String(e.tsMs);
    j += "\":{\"ts\":";
    j += String(e.tsMs);
    j += ",\"level\":";
    j += String(e.level);
    jAddStr(j, "status", levelName(e.level));
    jAddStr(j, "alerts", e.alerts);
    jAddNum(j, "angle", e.angle, 2);
    jAddNum(j, "freq", e.freq, 2);
    jAddNum(j, "vibRms", e.vibRms, 1);
    j += "}";
  }
  j += "}";
  return j;
}

static void cloudTask(void *) {
  if (!cloudConfigured()) {
    Serial.println("[NET] WiFi/Firebase not configured — offline mode.");
    vTaskDelete(nullptr);
  }

  tlsClient.setInsecure();
  http.setReuse(true);
  http.setTimeout(8000);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[NET] Connecting to %s...\n", WIFI_SSID);

  uint32_t lastWifiTry = millis(), lastLive = 0, lastHist = 0;
  bool announcedNtp = false;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiOk) Serial.println("[NET] WiFi lost.");
      wifiOk = false;
      if (millis() - lastWifiTry > WIFI_RETRY_MS) {
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        lastWifiTry = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!wifiOk) {
      wifiOk = true;
      Serial.printf("[NET] WiFi OK IP %s RSSI %d\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      startNtpIfNeeded();
    }
    if (timeSynced() && !announcedNtp) {
      announcedNtp = true;
      char d[16], t[16];
      dateTimeStrings(d, sizeof(d), t, sizeof(t));
      Serial.printf("[TIME] Clock synced: %s %s (IST)\n", d, t);
    }

    // Drain alarm events into the 5-minute rolling buffer
    EventRec ev;
    bool eventsDirty = false;
    while (xQueueReceive(eventQueue, &ev, 0) == pdTRUE) {
      eventPruneAndPush(ev);
      eventsDirty = true;
    }
    if (eventsDirty) {
      if (fbSend("PUT", "/events", buildEventsBundle()))
        Serial.printf("[FB] EVENTS updated (%d in last 5 min)\n", eventCount);
    }

    uint32_t now = millis();
    if (now - lastLive >= LIVE_UPLOAD_MS) {
      lastLive = now;
      LiveState s;
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      s = gLive;
      bool doSpec = s.spectrumReady;
      if (doSpec) gLive.spectrumReady = false;
      xSemaphoreGive(dataMutex);

      if (fbSend("PUT", "/live", buildLiveJson(s))) {
        Serial.printf("[FB] LIVE | MPU:%s angle:%.2f freq:%.2f%s vib:%.1f status:%s\n",
                      s.mpuOk ? "OK" : "NO",
                      s.angle,
                      isfinite(s.freq) ? s.freq : 0.0f,
                      s.freqValid ? "" : "?",
                      s.vibRms,
                      levelName(s.level));
      }

      // History: keep only last 5 minutes, replace whole node (old points deleted)
      if (now - lastHist >= HISTORY_UPLOAD_MS) {
        lastHist = now;
        histPruneAndPush(s);
        if (fbSend("PUT", "/history", buildHistoryBundle()))
          Serial.printf("[FB] HISTORY updated (%d pts, last 5 min)\n", histCount);
      }

      if (doSpec && s.mpuOk) {
        if (fbSend("PUT", "/spectrum", buildSpectrumJson(s)))
          Serial.println("[FB] SPECTRUM sent");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================================================================
//  SERIAL
// ============================================================================
static void printHelp() {
  Serial.println("Commands: Z=zero  N=+90 save  C=clear angle cal  F=baseline freq");
  Serial.println("          X=clear baseline  M=mute  P=status  D=debug  H=help");
}

static void printStatus() {
  Serial.println("------------------------------------------------------------");
  Serial.printf("Status : %s %s\n", levelName(alarmLevel), alertStr);
  Serial.printf("Angle  : %.2f deg  cal:%s\n", deviation, angleCalSaved ? "yes" : "NO");
  Serial.printf("Freq   : %.2f Hz valid:%d  baseline %.2f\n",
                dominantFreqHz, freqValid, baselineFreq);
  Serial.printf("Vib    : RMS %.1f mg  peak %.1f mg\n", vibRmsMg, vibPeakMg);
  Serial.printf("MPU:%s  SD:%s  WiFi:%s  muted:%s\n",
                sensorOk ? "OK" : "NO", sdOk ? "OK" : "NO",
                wifiOk ? "OK" : "NO", buzzerMuted ? "yes" : "no");
  Serial.println("------------------------------------------------------------");
}

static void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r' || c == ' ') continue;
    c = toupper(c);
    switch (c) {
      case 'Z':
        Serial.println("\n>>> Z — calibrating ZERO...");
        calibrateZero();
        break;
      case 'N':
      case '9':
        Serial.println("\n>>> N — locking +90...");
        calibrateNinety();
        break;
      case 'C':
        clearAngleCal();
        break;
      case 'F':
        if (freqReady && freqPeakAmpMg >= 0.05f) {
          baselineFreq = dominantFreqHz;
          saveBaselineFreq();
          freqValid = true;  // accept this peak as the reference
          Serial.printf("[CAL] Baseline frequency saved: %.3f Hz (peak amp %.2f mg)%s\n",
                        baselineFreq, freqPeakAmpMg,
                        freqPeakAmpMg < MIN_PEAK_MG ? " [weak peak — ok for lab]" : "");
        } else {
          Serial.println("[CAL] Wait for one FFT window (~5 s), then send F again.");
        }
        break;
      case 'X':
        clearBaselineFreq();
        break;
      case 'M':
        buzzerMuted = !buzzerMuted;
        Serial.printf("[BUZZER] %s\n", buzzerMuted ? "muted" : "unmuted");
        break;
      case 'P':
        printStatus();
        break;
      case 'D':
        showDebug = !showDebug;
        Serial.println(showDebug ? "Debug ON" : "Debug OFF");
        break;
      case 'H':
      case '?':
        printHelp();
        break;
      default:
        Serial.printf("Unknown '%c'. Send H for help.\n", c);
        break;
    }
  }
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n==== SCOUR SENTRY ====");

  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_OFF);

  int leds[3] = {LED_RED, LED_YELLOW, LED_GREEN};
  for (int i = 0; i < 3; i++) {
    digitalWrite(leds[i], HIGH); delay(200); digitalWrite(leds[i], LOW);
  }
  digitalWrite(BUZZER_PIN, BUZZER_ON); delay(80); digitalWrite(BUZZER_PIN, BUZZER_OFF);

  dataMutex = xSemaphoreCreateMutex();
  eventQueue = xQueueCreate(16, sizeof(EventRec));
  memset(&gLive, 0, sizeof(gLive));
  gLive.angle = gLive.freq = gLive.vibRms = gLive.vibPeak = gLive.baselineFreq = NAN;

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  if (mpuInit()) {
    sensorOk = true;
    Serial.println("[MPU] Ready (200 Hz, complementary filter + FFT).");
  } else {
    Serial.println("[MPU] NOT DETECTED — will keep retrying.");
  }

  initHann();
  loadBaselineFreq();
  if (loadAngleCal()) {
    Serial.println("[CAL] Angle calibration loaded.");
  } else if (sensorOk) {
    Serial.println("[CAL] No saved angle cal — do Z (flat) then N (+90).");
    calibrateZero();
  }

  sdOk = sdInit();
  lastSdRetry = millis();
  Serial.println(sdOk ? "[SD] Card mounted." : "[SD] Card NOT found — will retry.");

  xTaskCreatePinnedToCore(cloudTask, "cloud", 10240, nullptr, 1, nullptr, 0);

  sampleIdx = 0;
  windowStartUs = micros();
  nextSampleUs = micros();
  printHelp();
}

void loop() {
  handleSerial();
  updateOutputs();
  sdMaintain();

  // Retry MPU if missing
  static uint32_t lastMpuTry = 0;
  if (!sensorOk && millis() - lastMpuTry > 2000) {
    lastMpuTry = millis();
    if (mpuInit()) {
      sensorOk = true;
      Serial.println("[MPU] Connected.");
      sampleIdx = 0;
      windowStartUs = micros();
      nextSampleUs = micros();
      filterStarted = false;
    }
  }

  if (!sensorOk) {
    static uint32_t lastEval = 0;
    if (millis() - lastEval > EVAL_MS) {
      lastEval = millis();
      evaluateAlarm();
    }
    delay(5);
    return;
  }

  // Fixed-rate 200 Hz sampling (same as calibrated sketch)
  uint32_t nowUs = micros();
  if ((int32_t)(nowUs - nextSampleUs) < 0) {
    delay(1);
    return;
  }
  nextSampleUs += 1000000UL / SAMPLE_RATE_HZ;

  float ax, ay, az, gx, gy, gz;
  if (!readAccelG(ax, ay, az) || !readGyroDps(gx, gy, gz)) {
    static int fails = 0;
    if (++fails >= 20) {
      sensorOk = false;
      fails = 0;
      Serial.println("[MPU] Read failures — marked disconnected.");
      evaluateAlarm();
    }
    return;
  }

  // --- Angle ---
  float dt = 1.0f / SAMPLE_RATE_HZ;
  float accelAngle = atan2(-ay, az) * 180.0f / PI;
  float rateX = gx - gyroBiasX;
  if (!filterStarted) {
    filteredAngle = accelAngle;
    filterStarted = true;
  } else {
    float gyroAngle = filteredAngle + rateX * dt;
    filteredAngle = FILTER_ALPHA * gyroAngle + (1.0f - FILTER_ALPHA) * accelAngle;
  }
  deviation = wrapAngle(filteredAngle - zeroAngle) * scaleFactor;

  // --- FFT buffer ---
  sampleBuf[0][sampleIdx] = ax;
  sampleBuf[1][sampleIdx] = ay;
  sampleBuf[2][sampleIdx] = az;
  sampleIdx++;

  bool newFft = false;
  if (sampleIdx >= FFT_SIZE) {
    uint32_t t1 = micros();
    float fs = FFT_SIZE * 1e6f / (float)(t1 - windowStartUs);
    windowStartUs = t1;
    sampleIdx = 0;
    analyzeFrequency(fs);
    newFft = true;
  }

  static uint32_t lastEvalMs = 0, lastPrintMs = 0, lastLogMs = 0;
  uint32_t nowMs = millis();

  if (newFft || nowMs - lastEvalMs >= EVAL_MS) {
    lastEvalMs = nowMs;
    evaluateAlarm();
  }

  if (nowMs - lastLogMs >= 1000) {
    lastLogMs = nowMs;
    sdLogRow(deviation, dominantFreqHz, freqReady, vibRmsMg, vibPeakMg, NAN);
  }

  if (nowMs - lastPrintMs >= STATUS_PRINT_MS) {
    lastPrintMs = nowMs;
    Serial.printf("%-7s Angle: %.2f deg | Freq: ", levelName(alarmLevel), deviation);
    if (freqReady) {
      Serial.printf("%.2f Hz%s", dominantFreqHz, freqValid ? "" : "?");
    } else {
      Serial.print("waiting...");
    }
    Serial.printf(" | Vib: %.1f mg\n", vibRmsMg);

    if (!angleCalSaved && fabsf(deviation) > 80.0f)
      Serial.println(">>> Near 90? Type N to lock & save.");

    if (showDebug) {
      Serial.printf("  ACCEL ax:%.3f ay:%.3f az:%.3f\n", ax, ay, az);
      Serial.printf("  GYRO  gx:%.2f gy:%.2f gz:%.2f\n", gx, gy, gz);
    }
  }
}
