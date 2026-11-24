/*
  ============================================================================
   SCOUR SENTRY  -  Bridge pier scour early-warning node (ESP32)
  ============================================================================
   Hardware
     MPU6050   : SDA 21, SCL 22
     SD card   : CS 5, MOSI 23, CLK 18, MISO 19 (3V3)
     LEDs      : RED 25, YELLOW 26, GREEN 27 (active HIGH)
     Buzzer    : S 14 (active HIGH)

   What it does
     - Samples the accelerometer at 200 Hz.
     - Every ~5 s runs an FFT on the window and finds the dominant
       (natural) frequency of the pier, plus vibration RMS / peak.
     - Every 1 s computes the tilt of the pier relative to a stored
       zero reference (roll, pitch and total tilt).
     - Compares tilt, natural-frequency drop and vibration against
       thresholds -> SAFE / WARNING / DANGER.
     - Drives LEDs + buzzer, logs every second to the SD card,
       and uploads live data / history / spectrum / events to the
       Firebase Realtime Database for the web dashboard.

   Serial commands (115200 baud)
     Z : full calibration (tilt zero + baseline frequency)
     T : tilt zero only            F : baseline frequency only
     X : clear stored calibration  M : mute / unmute buzzer
     P : print status              H : help

   Calibration is stored in flash (NVS) and survives reboots. On the very
   first boot (nothing stored) the node calibrates itself automatically.
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

// Private WiFi + Firebase secret — copy secrets.h.example -> secrets.h (gitignored)
#include "secrets.h"

// ============================================================================
//  USER CONFIGURATION
// ============================================================================
#define DEVICE_ID        "pier-01"

#define GMT_OFFSET_SEC   19800        // IST (UTC+5:30)

// ============================================================================
//  PINS
// ============================================================================
#define MPU_ADDR     0x68
#define I2C_SDA      21
#define I2C_SCL      22

#define SD_CS        5
#define SD_MOSI      23
#define SD_SCK       18
#define SD_MISO      19

#define LED_RED      25
#define LED_YELLOW   26
#define LED_GREEN    27

#define BUZZER_PIN   14
#define BUZZER_ON    HIGH
#define BUZZER_OFF   LOW

// ============================================================================
//  MEASUREMENT SETTINGS
// ============================================================================
#define SAMPLE_RATE_HZ     200
#define FFT_SIZE           1024        // 1024 / 200 Hz = 5.12 s window, 0.195 Hz resolution
#define FREQ_MIN_HZ        0.5f        // search band for the natural frequency
#define FREQ_MAX_HZ        25.0f
#define MIN_PEAK_MG        0.5f        // spectral peak must exceed this to be trusted
#define MIN_PEAK_SNR       4.0f        // ... and be this many times the band average
#define SPECTRUM_POINTS    64          // points sent to the dashboard

// ============================================================================
//  ALARM THRESHOLDS  (placeholders - replace with calibrated values)
// ============================================================================
#define TILT_WARN_DEG          0.5f
#define TILT_DANGER_DEG        1.0f
#define FREQ_DROP_WARN_PCT     10.0f    // % drop from baseline natural frequency
#define FREQ_DROP_DANGER_PCT   20.0f
#define VIB_WARN_MG            50.0f    // vibration RMS in milli-g
#define VIB_DANGER_MG          150.0f
#define ALARM_CONFIRM_COUNT    3        // consecutive 1 s evaluations before a level change

// Calibration
#define CAL_TILT_BLOCKS        5        // seconds averaged for tilt zero
#define CAL_FREQ_WINDOWS       3        // valid FFT windows averaged for baseline
#define CAL_FREQ_MAX_WINDOWS   12       // give up after this many windows
#define CAL_MAX_VIB_MG         VIB_WARN_MG

// Timing
#define LIVE_UPLOAD_MS         2000
#define HISTORY_UPLOAD_MS      10000
#define WIFI_RETRY_MS          15000
#define SD_RETRY_MS            10000
#define SENSOR_FAIL_LIMIT      20       // consecutive failed reads -> sensor fault

#define LOG_FILE    "/scour_log.csv"
#define EVENT_FILE  "/scour_events.csv"

// ============================================================================
//  SHARED DATA
// ============================================================================
struct TiltBlock {
  float gx, gy, gz;     // mean acceleration over 1 s (g)
  float rmsMg;          // dynamic RMS over the block (mg)
};

struct Analysis {
  float freq;           // dominant frequency (Hz)
  bool  freqValid;
  float peakMg;         // amplitude of the spectral peak (mg)
  float vibRms;         // vibration RMS (mg)
  float vibPeak;        // vibration peak (mg)
  float fs;             // measured sample rate (Hz)
  float df;             // FFT bin width (Hz)
  int   group;          // FFT bins merged per spectrum point
  float spectrum[SPECTRUM_POINTS];
};

struct LiveState {
  float roll, pitch, tilt;          // deviation from zero reference (deg)
  float rollRaw, pitchRaw;          // absolute orientation (deg)
  float freq, freqPeakMg, baselineFreq, freqChangePct;
  bool  freqValid;
  float vibRms, vibPeak, fs;
  int   level;
  char  alerts[40];
  bool  buzzer, muted, sensorOk, sdOk, tiltCal, freqCal, calibrating;
  uint32_t uptimeS;
};

struct EventRec {
  int   level;
  char  alerts[40];
  float tilt, freq, vibRms;
  uint32_t epoch;
};

static SemaphoreHandle_t dataMutex;
static QueueHandle_t     eventQueue;
static TaskHandle_t      analysisTaskHandle;

static TiltBlock gTilt;
static uint32_t  gTiltSeq = 0;
static Analysis  gAna;
static uint32_t  gAnaSeq = 0;
static LiveState gLive;

static volatile bool sensorOk = false;
static volatile bool wifiOk   = false;

// Sample buffers (double buffered: one filling, one being analysed)
static float sampleBuf[2][3][FFT_SIZE];
static float windowFs[2];
static volatile int readyBuf = -1;

static float fftRe[FFT_SIZE], fftIm[FFT_SIZE], hannWin[FFT_SIZE];
static float ampSq[FFT_SIZE / 2 + 1];

// ============================================================================
//  CALIBRATION STATE (owned by loop)
// ============================================================================
static Preferences prefs;

static bool  tiltCal = false, freqCal = false;
static float refG[3] = {0, 0, 1};
static float refRoll = 0, refPitch = 0;
static float baselineFreq = NAN;

static bool   calTiltRunning = false, calFreqRunning = false;
static int    calTiltCount = 0;
static double calSum[3];
static int    calFreqCount = 0, calFreqSeen = 0;
static float  calFreqSum = 0;

// ============================================================================
//  RUNTIME STATE (owned by loop)
// ============================================================================
static float curRoll = NAN, curPitch = NAN, curTilt = NAN;
static float curRollRaw = NAN, curPitchRaw = NAN;
static Analysis lastAna;
static bool   haveAna = false;
static uint32_t lastAnaMs = 0;

static int  alarmLevel = 0;
static int  pendingLevel = 0, pendingCount = 0;
static char alertStr[40] = "";
static bool buzzerMuted = false;
static bool buzzerActive = false;

static bool sdOk = false;
static uint32_t lastSdRetry = 0;

// ============================================================================
//  MPU6050
// ============================================================================
static bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static int mpuReadReg(uint8_t reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((uint8_t)MPU_ADDR, (size_t)1, true) != 1) return -1;
  return Wire.read();
}

static bool mpuInit() {
  Wire.beginTransmission(MPU_ADDR);
  if (Wire.endTransmission() != 0) return false;

  bool ok = mpuWrite(0x6B, 0x80);           // reset
  delay(100);
  ok &= mpuWrite(0x6B, 0x01);               // wake, PLL clock from gyro X
  ok &= mpuWrite(0x19, 0x04);               // sample rate 1 kHz / (1+4) = 200 Hz
  ok &= mpuWrite(0x1A, 0x03);               // DLPF ~44 Hz (anti-alias for 200 Hz)
  ok &= mpuWrite(0x1C, 0x00);               // accel +-2 g
  ok &= mpuWrite(0x1B, 0x00);               // gyro +-250 dps
  delay(50);

  int who = mpuReadReg(0x75);
  if (who != 0x68) {
    Serial.printf("[MPU] WHO_AM_I = 0x%02X (expected 0x68, clone chip?)\n", who);
  }
  return ok;
}

static bool mpuReadAccel(float &ax, float &ay, float &az) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MPU_ADDR, (size_t)6, true) != 6) return false;

  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();

  ax = (int16_t)((b[0] << 8) | b[1]) / 16384.0f;
  ay = (int16_t)((b[2] << 8) | b[3]) / 16384.0f;
  az = (int16_t)((b[4] << 8) | b[5]) / 16384.0f;
  return true;
}

// ============================================================================
//  FFT (in-place, radix-2)
// ============================================================================
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
    double ang = -2.0 * M_PI / len;
    float wr = cos(ang), wi = sin(ang);
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

// ============================================================================
//  TASK: sensor sampling (200 Hz)
// ============================================================================
static void sensorTask(void *) {
  int fillBuf = 0, idx = 0;
  uint32_t windowStartUs = micros();

  double sx = 0, sy = 0, sz = 0, sxx = 0, syy = 0, szz = 0;
  int blockN = 0;
  int failStreak = 0;
  float ax = 0, ay = 0, az = 1;

  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / SAMPLE_RATE_HZ);

  for (;;) {
    vTaskDelayUntil(&lastWake, period);

    float x, y, z;
    if (mpuReadAccel(x, y, z)) {
      if (!sensorOk) {
        idx = 0; blockN = 0;
        sx = sy = sz = sxx = syy = szz = 0;
        windowStartUs = micros();
      }
      ax = x; ay = y; az = z;
      failStreak = 0;
      sensorOk = true;
    } else {
      failStreak++;
      if (failStreak >= SENSOR_FAIL_LIMIT) {
        sensorOk = false;
        if (failStreak % SAMPLE_RATE_HZ == 0) mpuInit();   // retry once a second
        continue;
      }
      // short glitch: repeat the previous sample to keep timing intact
    }

    sampleBuf[fillBuf][0][idx] = ax;
    sampleBuf[fillBuf][1][idx] = ay;
    sampleBuf[fillBuf][2][idx] = az;
    idx++;

    if (idx >= FFT_SIZE) {
      uint32_t now = micros();
      windowFs[fillBuf] = FFT_SIZE * 1e6f / (float)(now - windowStartUs);
      windowStartUs = now;
      readyBuf = fillBuf;
      xTaskNotifyGive(analysisTaskHandle);
      fillBuf ^= 1;
      idx = 0;
    }

    sx += ax; sy += ay; sz += az;
    sxx += (double)ax * ax; syy += (double)ay * ay; szz += (double)az * az;
    if (++blockN >= SAMPLE_RATE_HZ) {
      TiltBlock tb;
      tb.gx = sx / blockN; tb.gy = sy / blockN; tb.gz = sz / blockN;
      double var = (sxx / blockN - (double)tb.gx * tb.gx)
                 + (syy / blockN - (double)tb.gy * tb.gy)
                 + (szz / blockN - (double)tb.gz * tb.gz);
      tb.rmsMg = var > 0 ? sqrt(var) * 1000.0f : 0;

      xSemaphoreTake(dataMutex, portMAX_DELAY);
      gTilt = tb;
      gTiltSeq++;
      xSemaphoreGive(dataMutex);

      sx = sy = sz = sxx = syy = szz = 0;
      blockN = 0;
    }
  }
}

// ============================================================================
//  TASK: FFT analysis (natural frequency, vibration)
// ============================================================================
static void analysisTask(void *) {
  for (int i = 0; i < FFT_SIZE; i++)
    hannWin[i] = 0.5f * (1.0f - cos(2.0 * M_PI * i / (FFT_SIZE - 1)));

  static Analysis res;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int b = readyBuf;
    if (b < 0) continue;

    const int N = FFT_SIZE;
    const int H = N / 2;
    float fs = windowFs[b];
    if (fs < SAMPLE_RATE_HZ * 0.8f || fs > SAMPLE_RATE_HZ * 1.2f) fs = SAMPLE_RATE_HZ;
    float df = fs / N;

    memset(ampSq, 0, sizeof(ampSq));
    float mean[3];
    double varSum = 0;

    for (int ax = 0; ax < 3; ax++) {
      const float *s = sampleBuf[b][ax];
      double m = 0;
      for (int i = 0; i < N; i++) m += s[i];
      mean[ax] = m / N;

      for (int i = 0; i < N; i++) {
        float d = s[i] - mean[ax];
        varSum += (double)d * d;
        fftRe[i] = d * hannWin[i];
        fftIm[i] = 0;
      }
      fftInPlace(fftRe, fftIm, N);

      // Hann window: single-sided amplitude = 4 * |X| / N
      const float scale = 16.0f / ((float)N * N);
      for (int k = 1; k <= H; k++)
        ampSq[k] += (fftRe[k] * fftRe[k] + fftIm[k] * fftIm[k]) * scale;
    }

    float peak = 0;
    for (int i = 0; i < N; i++) {
      float dx = sampleBuf[b][0][i] - mean[0];
      float dy = sampleBuf[b][1][i] - mean[1];
      float dz = sampleBuf[b][2][i] - mean[2];
      float m = sqrtf(dx * dx + dy * dy + dz * dz);
      if (m > peak) peak = m;
    }

    // amplitude per bin in mg
    for (int k = 0; k <= H; k++) ampSq[k] = sqrtf(ampSq[k]) * 1000.0f;
    float *amp = ampSq;
    amp[0] = 0;

    int kMin = max(1, (int)ceilf(FREQ_MIN_HZ / df));
    int kMax = min(H - 1, (int)floorf(FREQ_MAX_HZ / df));
    int kPk = kMin;
    double bandSum = 0;
    for (int k = kMin; k <= kMax; k++) {
      bandSum += amp[k];
      if (amp[k] > amp[kPk]) kPk = k;
    }
    float bandAvg = bandSum / (kMax - kMin + 1);

    float a = amp[kPk - 1], c = amp[kPk], e = amp[kPk + 1];
    float denom = a - 2 * c + e;
    float delta = (fabsf(denom) > 1e-9f) ? 0.5f * (a - e) / denom : 0;
    if (delta > 0.5f) delta = 0.5f;
    if (delta < -0.5f) delta = -0.5f;

    res.freq      = (kPk + delta) * df;
    res.peakMg    = c;
    res.freqValid = (c >= MIN_PEAK_MG) && (bandAvg <= 0 || c >= MIN_PEAK_SNR * bandAvg);
    res.vibRms    = sqrt(varSum / N) * 1000.0f;
    res.vibPeak   = peak * 1000.0f;
    res.fs        = fs;
    res.df        = df;

    int group = (kMax + SPECTRUM_POINTS) / SPECTRUM_POINTS;
    res.group = group;
    for (int p = 0; p < SPECTRUM_POINTS; p++) {
      float m = 0;
      for (int k = p * group; k < (p + 1) * group && k <= H; k++)
        if (amp[k] > m) m = amp[k];
      res.spectrum[p] = m;
    }

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    gAna = res;
    gAnaSeq++;
    xSemaphoreGive(dataMutex);
  }
}

// ============================================================================
//  HELPERS
// ============================================================================
static float wrap180(float a) {
  while (a > 180) a -= 360;
  while (a < -180) a += 360;
  return a;
}

static bool timeSynced() { return time(nullptr) > 1700000000; }

static void timeString(char *out, size_t len) {
  if (!timeSynced()) { out[0] = 0; return; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  strftime(out, len, "%Y-%m-%d %H:%M:%S", &t);
}

// Firebase REST deletes keys set to null - so only append finite numbers.
static void jAddNum(String &j, const char *key, float v, int dec) {
  if (!isfinite(v)) return;
  j += ",\"";
  j += key;
  j += "\":";
  j += String(v, dec);
}

static void jAddBool(String &j, const char *key, bool v) {
  j += ",\"";
  j += key;
  j += "\":";
  j += v ? "true" : "false";
}

static void jAddStr(String &j, const char *key, const char *v) {
  j += ",\"";
  j += key;
  j += "\":\"";
  j += v;
  j += "\"";
}

static const char *levelName(int l) {
  return l >= 2 ? "DANGER" : (l == 1 ? "WARNING" : "SAFE");
}

// ============================================================================
//  CALIBRATION
// ============================================================================
static void loadCalibration() {
  prefs.begin("sentry", true);
  tiltCal = prefs.getBool("hasTilt", false);
  refG[0] = prefs.getFloat("gx", 0);
  refG[1] = prefs.getFloat("gy", 0);
  refG[2] = prefs.getFloat("gz", 1);
  freqCal = prefs.getBool("hasFreq", false);
  baselineFreq = prefs.getFloat("f0", NAN);
  prefs.end();

  refRoll  = atan2f(refG[1], refG[2]) * RAD_TO_DEG;
  refPitch = atan2f(-refG[0], sqrtf(refG[1] * refG[1] + refG[2] * refG[2])) * RAD_TO_DEG;
  if (!isfinite(baselineFreq)) freqCal = false;
}

static void saveTiltCal() {
  prefs.begin("sentry", false);
  prefs.putBool("hasTilt", true);
  prefs.putFloat("gx", refG[0]);
  prefs.putFloat("gy", refG[1]);
  prefs.putFloat("gz", refG[2]);
  prefs.end();
}

static void saveFreqCal() {
  prefs.begin("sentry", false);
  prefs.putBool("hasFreq", true);
  prefs.putFloat("f0", baselineFreq);
  prefs.end();
}

static void clearCalibration() {
  prefs.begin("sentry", false);
  prefs.clear();
  prefs.end();
  tiltCal = freqCal = false;
  baselineFreq = NAN;
  Serial.println("[CAL] Calibration cleared.");
}

static void startCalibration(bool tilt, bool freq) {
  if (tilt) {
    calTiltRunning = true;
    calTiltCount = 0;
    calSum[0] = calSum[1] = calSum[2] = 0;
    Serial.printf("[CAL] Tilt zero: keep the pier still for %d s...\n", CAL_TILT_BLOCKS);
  }
  if (freq) {
    calFreqRunning = true;
    calFreqCount = 0;
    calFreqSeen = 0;
    calFreqSum = 0;
    Serial.printf("[CAL] Baseline frequency: averaging %d FFT windows (~%d s)...\n",
                  CAL_FREQ_WINDOWS, (int)(CAL_FREQ_WINDOWS * FFT_SIZE / SAMPLE_RATE_HZ));
  }
}

static void calTiltStep(const TiltBlock &tb) {
  if (!calTiltRunning) return;
  if (tb.rmsMg > CAL_MAX_VIB_MG) {
    Serial.printf("[CAL] Too much movement (%.1f mg), restarting tilt zero.\n", tb.rmsMg);
    calTiltCount = 0;
    calSum[0] = calSum[1] = calSum[2] = 0;
    return;
  }
  calSum[0] += tb.gx; calSum[1] += tb.gy; calSum[2] += tb.gz;
  if (++calTiltCount >= CAL_TILT_BLOCKS) {
    for (int i = 0; i < 3; i++) refG[i] = calSum[i] / calTiltCount;
    refRoll  = atan2f(refG[1], refG[2]) * RAD_TO_DEG;
    refPitch = atan2f(-refG[0], sqrtf(refG[1] * refG[1] + refG[2] * refG[2])) * RAD_TO_DEG;
    tiltCal = true;
    calTiltRunning = false;
    saveTiltCal();
    Serial.printf("[CAL] Tilt zero saved. Roll %.2f deg, Pitch %.2f deg\n", refRoll, refPitch);
  }
}

static void calFreqStep(const Analysis &a) {
  if (!calFreqRunning) return;
  calFreqSeen++;
  if (a.freqValid) {
    calFreqSum += a.freq;
    calFreqCount++;
    Serial.printf("[CAL] Window %d: %.3f Hz\n", calFreqCount, a.freq);
  } else {
    Serial.println("[CAL] Window skipped (no clear peak).");
  }
  if (calFreqCount >= CAL_FREQ_WINDOWS) {
    baselineFreq = calFreqSum / calFreqCount;
    freqCal = true;
    calFreqRunning = false;
    saveFreqCal();
    Serial.printf("[CAL] Baseline frequency saved: %.3f Hz\n", baselineFreq);
  } else if (calFreqSeen >= CAL_FREQ_MAX_WINDOWS) {
    calFreqRunning = false;
    Serial.println("[CAL] Baseline frequency FAILED: no clear natural frequency. "
                   "Excite the structure (tap) and send F to retry.");
  }
}

// ============================================================================
//  ALARM LOGIC
// ============================================================================
static void evaluateAlarm() {
  int tl = 0, fl = 0, vl = 0;
  float freqChange = NAN;

  if (tiltCal && isfinite(curTilt)) {
    if (curTilt >= TILT_DANGER_DEG) tl = 2;
    else if (curTilt >= TILT_WARN_DEG) tl = 1;
  }

  bool anaFresh = haveAna && (millis() - lastAnaMs) < 3UL * FFT_SIZE * 1000 / SAMPLE_RATE_HZ;
  if (anaFresh) {
    if (freqCal && lastAna.freqValid) {
      freqChange = (lastAna.freq - baselineFreq) / baselineFreq * 100.0f;
      float drop = -freqChange;
      if (drop >= FREQ_DROP_DANGER_PCT) fl = 2;
      else if (drop >= FREQ_DROP_WARN_PCT) fl = 1;
    }
    if (lastAna.vibRms >= VIB_DANGER_MG) vl = 2;
    else if (lastAna.vibRms >= VIB_WARN_MG) vl = 1;
  }

  int raw = max(tl, max(fl, vl));
  char buf[40] = "";
  if (tl) strcat(buf, "TILT,");
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
    Serial.printf("[ALARM] %s -> %s  (%s)\n", levelName(prev), levelName(alarmLevel), alertStr);

    EventRec ev;
    ev.level = alarmLevel;
    strncpy(ev.alerts, alertStr, sizeof(ev.alerts));
    ev.tilt = curTilt;
    ev.freq = (haveAna && lastAna.freqValid) ? lastAna.freq : NAN;
    ev.vibRms = haveAna ? lastAna.vibRms : NAN;
    ev.epoch = timeSynced() ? (uint32_t)time(nullptr) : 0;
    xQueueSend(eventQueue, &ev, 0);

    if (sdOk) {
      File f = SD.open(EVENT_FILE, FILE_APPEND);
      if (f) {
        char ts[24];
        timeString(ts, sizeof(ts));
        f.printf("%s,%lu,%s,%s,%.3f,%.3f,%.2f\n", ts, millis() / 1000, levelName(alarmLevel),
                 alertStr, ev.tilt, ev.freq, ev.vibRms);
        f.close();
      }
    }
  }

  // publish live state
  LiveState ls;
  ls.roll = curRoll;           ls.pitch = curPitch;       ls.tilt = curTilt;
  ls.rollRaw = curRollRaw;     ls.pitchRaw = curPitchRaw;
  ls.freq = haveAna ? lastAna.freq : NAN;
  ls.freqValid = anaFresh && lastAna.freqValid;
  ls.freqPeakMg = haveAna ? lastAna.peakMg : NAN;
  ls.baselineFreq = freqCal ? baselineFreq : NAN;
  ls.freqChangePct = freqChange;
  ls.vibRms = haveAna ? lastAna.vibRms : NAN;
  ls.vibPeak = haveAna ? lastAna.vibPeak : NAN;
  ls.fs = haveAna ? lastAna.fs : NAN;
  ls.level = alarmLevel;
  strncpy(ls.alerts, alertStr, sizeof(ls.alerts));
  ls.buzzer = buzzerActive;
  ls.muted = buzzerMuted;
  ls.sensorOk = sensorOk;
  ls.sdOk = sdOk;
  ls.tiltCal = tiltCal;
  ls.freqCal = freqCal;
  ls.calibrating = calTiltRunning || calFreqRunning;
  ls.uptimeS = millis() / 1000;

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  gLive = ls;
  xSemaphoreGive(dataMutex);
}

// ============================================================================
//  LEDS + BUZZER (non-blocking)
// ============================================================================
static void updateOutputs() {
  uint32_t ms = millis();
  bool r = false, y = false, g = false, buzz = false;

  if (!sensorOk) {
    y = (ms / 500) % 2;                     // blinking yellow = sensor fault
  } else if (alarmLevel >= 2) {
    r = true;
    buzz = (ms % 800) < 400;                // fast beeping
  } else if (alarmLevel == 1) {
    y = true;
    buzz = (ms % 3000) < 150;               // short chirp every 3 s
  } else {
    g = true;
  }
  if (calTiltRunning || calFreqRunning) g = (ms / 250) % 2;   // blinking green while calibrating

  buzzerActive = alarmLevel > 0 && !buzzerMuted;
  if (buzzerMuted) buzz = false;

  digitalWrite(LED_RED, r);
  digitalWrite(LED_YELLOW, y);
  digitalWrite(LED_GREEN, g);
  digitalWrite(BUZZER_PIN, buzz ? BUZZER_ON : BUZZER_OFF);
}

// ============================================================================
//  SD CARD
// ============================================================================
static bool sdInit() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 4000000)) return false;

  if (!SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (!f) return false;
    f.println("datetime,uptime_s,roll_deg,pitch_deg,tilt_deg,freq_hz,freq_valid,"
              "baseline_hz,freq_change_pct,vib_rms_mg,vib_peak_mg,level,alerts");
    f.close();
  }
  if (!SD.exists(EVENT_FILE)) {
    File f = SD.open(EVENT_FILE, FILE_WRITE);
    if (f) {
      f.println("datetime,uptime_s,level,alerts,tilt_deg,freq_hz,vib_rms_mg");
      f.close();
    }
  }
  return true;
}

static void sdMaintain() {
  if (sdOk || millis() - lastSdRetry < SD_RETRY_MS) return;
  lastSdRetry = millis();
  SD.end();
  sdOk = sdInit();
  if (sdOk) Serial.println("[SD] Card mounted.");
}

static void sdLogRow() {
  if (!sdOk) return;
  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[SD] Write failed, card removed?");
    sdOk = false;
    lastSdRetry = millis();
    return;
  }
  char ts[24];
  timeString(ts, sizeof(ts));
  bool fv = haveAna && lastAna.freqValid;
  f.printf("%s,%lu,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.2f,%.2f,%.2f,%s,%s\n",
           ts, millis() / 1000, curRoll, curPitch, curTilt,
           haveAna ? lastAna.freq : NAN, fv ? 1 : 0,
           freqCal ? baselineFreq : NAN, gLive.freqChangePct,
           haveAna ? lastAna.vibRms : NAN, haveAna ? lastAna.vibPeak : NAN,
           levelName(alarmLevel), alertStr);
  f.close();
}

// ============================================================================
//  FIREBASE (REST API over HTTPS)
// ============================================================================
static WiFiClientSecure tlsClient;
static HTTPClient http;

static bool cloudConfigured() {
  return strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0 && strstr(FIREBASE_DB_URL, "YOUR-PROJECT") == nullptr;
}

static bool fbSend(const char *method, const char *path, const String &body) {
  String url = String(FIREBASE_DB_URL) + "/scourSentry/" + DEVICE_ID + path + ".json";
  if (strlen(FIREBASE_AUTH)) url += String("?auth=") + FIREBASE_AUTH;

  if (!http.begin(tlsClient, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, body);
  if (code != 200) {
    Serial.printf("[FB] FAIL %s %s -> %d %s\n", method, path, code,
                  code > 0 ? http.getString().c_str() : http.errorToString(code).c_str());
  }
  http.end();
  return code == 200;
}

static String buildLiveJson(const LiveState &s) {
  String j;
  j.reserve(1100);
  j += "{\"ts\":{\".sv\":\"timestamp\"}";
  j += ",\"deviceTime\":" + String(timeSynced() ? (uint32_t)time(nullptr) : 0);
  j += ",\"uptime\":" + String(s.uptimeS);

  // Hardware connection status (always present - never null)
  jAddBool(j, "mpuConnected", s.sensorOk);
  jAddBool(j, "sdConnected", s.sdOk);
  jAddBool(j, "wifiConnected", true);
  jAddBool(j, "sensorOk", s.sensorOk);
  jAddBool(j, "sdOk", s.sdOk);
  jAddBool(j, "hasReadings", s.sensorOk && isfinite(s.tilt));
  jAddStr(j, "mpuStatus", s.sensorOk ? "CONNECTED" : "NOT_CONNECTED");
  jAddStr(j, "sdStatus", s.sdOk ? "CONNECTED" : "NOT_CONNECTED");
  jAddStr(j, "dataNote",
          s.sensorOk
            ? (isfinite(s.tilt) ? "Live MPU6050 readings" : "MPU connected - waiting for calibration/readings")
            : "MPU6050 NOT CONNECTED - no tilt/frequency data");

  // Readings only when finite (Firebase deletes null keys)
  jAddNum(j, "roll", s.roll, 3);
  jAddNum(j, "pitch", s.pitch, 3);
  jAddNum(j, "tilt", s.tilt, 3);
  jAddNum(j, "rollRaw", s.rollRaw, 2);
  jAddNum(j, "pitchRaw", s.pitchRaw, 2);
  jAddNum(j, "freq", s.freq, 3);
  jAddBool(j, "freqValid", s.freqValid);
  jAddNum(j, "freqPeakMg", s.freqPeakMg, 2);
  jAddNum(j, "baselineFreq", s.baselineFreq, 3);
  jAddNum(j, "freqChangePct", s.freqChangePct, 2);
  jAddNum(j, "vibRms", s.vibRms, 2);
  jAddNum(j, "vibPeak", s.vibPeak, 2);
  jAddNum(j, "sampleRate", s.fs, 1);

  j += ",\"level\":" + String(s.level);
  jAddStr(j, "status", levelName(s.level));
  jAddStr(j, "alerts", s.alerts);
  jAddBool(j, "buzzer", s.buzzer);
  jAddBool(j, "muted", s.muted);
  jAddBool(j, "tiltCal", s.tiltCal);
  jAddBool(j, "freqCal", s.freqCal);
  jAddBool(j, "calibrating", s.calibrating);
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"thresholds\":{";
  j += "\"tiltWarn\":" + String(TILT_WARN_DEG, 2);
  j += ",\"tiltDanger\":" + String(TILT_DANGER_DEG, 2);
  j += ",\"freqDropWarn\":" + String(FREQ_DROP_WARN_PCT, 1);
  j += ",\"freqDropDanger\":" + String(FREQ_DROP_DANGER_PCT, 1);
  j += ",\"vibWarn\":" + String(VIB_WARN_MG, 1);
  j += ",\"vibDanger\":" + String(VIB_DANGER_MG, 1);
  j += "}}";
  return j;
}

static String buildHistoryJson(const LiveState &s) {
  String j = "{\"ts\":{\".sv\":\"timestamp\"}";
  jAddBool(j, "mpuConnected", s.sensorOk);
  jAddBool(j, "hasReadings", s.sensorOk && isfinite(s.tilt));
  jAddNum(j, "tilt", s.tilt, 3);
  jAddNum(j, "roll", s.roll, 3);
  jAddNum(j, "pitch", s.pitch, 3);
  if (s.freqValid) jAddNum(j, "freq", s.freq, 3);
  jAddNum(j, "vibRms", s.vibRms, 2);
  j += ",\"level\":" + String(s.level);
  j += "}";
  return j;
}

static String buildSpectrumJson(const Analysis &a) {
  String j;
  j.reserve(64 + SPECTRUM_POINTS * 8);
  j += "{\"ts\":{\".sv\":\"timestamp\"}";
  j += ",\"df\":" + String(a.df, 5);
  j += ",\"group\":" + String(a.group);
  jAddNum(j, "peakFreq", a.freq, 3);
  jAddBool(j, "valid", a.freqValid);
  j += ",\"amp\":[";
  for (int i = 0; i < SPECTRUM_POINTS; i++) {
    if (i) j += ",";
    j += String(a.spectrum[i], 3);
  }
  j += "]}";
  return j;
}

static String buildEventJson(const EventRec &e) {
  String j = "{\"ts\":{\".sv\":\"timestamp\"}";
  j += ",\"level\":" + String(e.level);
  jAddStr(j, "status", levelName(e.level));
  jAddStr(j, "alerts", e.alerts);
  jAddNum(j, "tilt", e.tilt, 3);
  jAddNum(j, "freq", e.freq, 3);
  jAddNum(j, "vibRms", e.vibRms, 2);
  j += "}";
  return j;
}

static void cloudTask(void *) {
  if (!cloudConfigured()) {
    Serial.println("[NET] WiFi/Firebase not configured - running offline (SD + local alarms only).");
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
  uint32_t sentAnaSeq = 0;
  bool ntpStarted = false;

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
      Serial.printf("[NET] WiFi connected, IP %s, RSSI %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (!ntpStarted) {
        configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
        ntpStarted = true;
      }
    }

    EventRec ev;
    while (xQueueReceive(eventQueue, &ev, 0) == pdTRUE) {
      if (!fbSend("POST", "/events", buildEventJson(ev))) {
        xQueueSendToFront(eventQueue, &ev, 0);
        break;
      }
    }

    uint32_t now = millis();
    if (now - lastLive >= LIVE_UPLOAD_MS) {
      lastLive = now;
      LiveState s;
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      s = gLive;
      xSemaphoreGive(dataMutex);

      bool liveOk = fbSend("PUT", "/live", buildLiveJson(s));
      if (liveOk) {
        Serial.printf("[FB] LIVE sent | MPU:%s  SD:%s  status:%s  alerts:%s\n",
                      s.sensorOk ? "CONNECTED" : "NOT_CONNECTED",
                      s.sdOk ? "OK" : "NO",
                      levelName(s.level),
                      s.alerts[0] ? s.alerts : "-");
      }

      if (now - lastHist >= HISTORY_UPLOAD_MS) {
        lastHist = now;
        if (fbSend("POST", "/history", buildHistoryJson(s)))
          Serial.println("[FB] HISTORY point sent");
      }
    }

    uint32_t seq;
    static Analysis a;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    seq = gAnaSeq;
    if (seq != sentAnaSeq) a = gAna;
    xSemaphoreGive(dataMutex);
    // Only publish spectrum when the MPU is actually connected
    if (seq != sentAnaSeq && seq > 0 && sensorOk) {
      if (fbSend("PUT", "/spectrum", buildSpectrumJson(a))) {
        sentAnaSeq = seq;
        Serial.println("[FB] SPECTRUM sent");
      }
    } else if (seq != sentAnaSeq) {
      sentAnaSeq = seq;  // discard spectrum from failed sensor window
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================================================================
//  SERIAL COMMANDS
// ============================================================================
static void printHelp() {
  Serial.println("Commands: Z=full calibration  T=tilt zero  F=baseline freq  "
                 "X=clear calibration  M=mute/unmute  P=status  H=help");
}

static void printStatus() {
  Serial.println("------------------------------------------------------------");
  Serial.printf("Status   : %s %s\n", levelName(alarmLevel), alertStr);
  Serial.printf("Tilt     : %.3f deg (roll %.3f, pitch %.3f)  zero:%s\n",
                curTilt, curRoll, curPitch, tiltCal ? "yes" : "NO");
  Serial.printf("Freq     : %.3f Hz valid:%d  baseline %.3f Hz  peak %.2f mg\n",
                haveAna ? lastAna.freq : NAN, haveAna && lastAna.freqValid,
                baselineFreq, haveAna ? lastAna.peakMg : NAN);
  Serial.printf("Vibration: RMS %.2f mg, peak %.2f mg, fs %.1f Hz\n",
                haveAna ? lastAna.vibRms : NAN, haveAna ? lastAna.vibPeak : NAN,
                haveAna ? lastAna.fs : NAN);
  Serial.printf("Sensor:%s  SD:%s  WiFi:%s  Buzzer muted:%s\n",
                sensorOk ? "OK" : "FAULT", sdOk ? "OK" : "NO", wifiOk ? "OK" : "NO",
                buzzerMuted ? "yes" : "no");
  Serial.println("------------------------------------------------------------");
}

static void handleSerial() {
  while (Serial.available()) {
    char c = toupper(Serial.read());
    switch (c) {
      case 'Z': startCalibration(true, true); break;
      case 'T': startCalibration(true, false); break;
      case 'F': startCalibration(false, true); break;
      case 'X': clearCalibration(); break;
      case 'M':
        buzzerMuted = !buzzerMuted;
        Serial.printf("[BUZZER] %s\n", buzzerMuted ? "muted until next level change" : "unmuted");
        break;
      case 'P': printStatus(); break;
      case 'H': case '?': printHelp(); break;
      default: break;
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

  // power-on self test
  int leds[3] = {LED_RED, LED_YELLOW, LED_GREEN};
  for (int i = 0; i < 3; i++) {
    digitalWrite(leds[i], HIGH);
    delay(250);
    digitalWrite(leds[i], LOW);
  }
  digitalWrite(BUZZER_PIN, BUZZER_ON);
  delay(100);
  digitalWrite(BUZZER_PIN, BUZZER_OFF);

  dataMutex  = xSemaphoreCreateMutex();
  eventQueue = xQueueCreate(16, sizeof(EventRec));
  memset(&gLive, 0, sizeof(gLive));
  gLive.roll = gLive.pitch = gLive.tilt = NAN;
  gLive.freq = gLive.baselineFreq = gLive.freqChangePct = gLive.vibRms = gLive.vibPeak = NAN;

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  if (mpuInit()) {
    sensorOk = true;
    Serial.println("[MPU] MPU6050 ready (200 Hz, +-2 g, DLPF 44 Hz).");
  } else {
    Serial.println("[MPU] MPU6050 NOT DETECTED - check wiring. Will keep retrying.");
  }

  sdOk = sdInit();
  lastSdRetry = millis();
  Serial.println(sdOk ? "[SD] Card mounted." : "[SD] Card NOT found - will keep retrying.");

  loadCalibration();
  if (tiltCal) Serial.printf("[CAL] Tilt zero loaded (roll %.2f, pitch %.2f).\n", refRoll, refPitch);
  if (freqCal) Serial.printf("[CAL] Baseline frequency loaded: %.3f Hz\n", baselineFreq);
  if (!tiltCal || !freqCal) {
    Serial.println("[CAL] No stored calibration - calibrating automatically.");
    startCalibration(!tiltCal, !freqCal);
  }

  xTaskCreatePinnedToCore(analysisTask, "analysis", 4096, nullptr, 2, &analysisTaskHandle, 1);
  xTaskCreatePinnedToCore(sensorTask,   "sensor",   4096, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(cloudTask,    "cloud",   10240, nullptr, 1, nullptr, 0);

  printHelp();
}

void loop() {
  handleSerial();

  static uint32_t seenTiltSeq = 0, seenAnaSeq = 0, lastEvalMs = 0;
  TiltBlock tb;
  uint32_t tSeq, aSeq;
  bool newAna = false;

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  tSeq = gTiltSeq;
  tb = gTilt;
  aSeq = gAnaSeq;
  if (aSeq != seenAnaSeq) { lastAna = gAna; newAna = true; }
  xSemaphoreGive(dataMutex);

  if (newAna) {
    seenAnaSeq = aSeq;
    haveAna = true;
    lastAnaMs = millis();
    calFreqStep(lastAna);
  }

  if (tSeq != seenTiltSeq) {
    seenTiltSeq = tSeq;

    curRollRaw  = atan2f(tb.gy, tb.gz) * RAD_TO_DEG;
    curPitchRaw = atan2f(-tb.gx, sqrtf(tb.gy * tb.gy + tb.gz * tb.gz)) * RAD_TO_DEG;

    calTiltStep(tb);

    if (tiltCal) {
      curRoll  = wrap180(curRollRaw - refRoll);
      curPitch = curPitchRaw - refPitch;
      float cx = tb.gy * refG[2] - tb.gz * refG[1];
      float cy = tb.gz * refG[0] - tb.gx * refG[2];
      float cz = tb.gx * refG[1] - tb.gy * refG[0];
      float dot = tb.gx * refG[0] + tb.gy * refG[1] + tb.gz * refG[2];
      curTilt = atan2f(sqrtf(cx * cx + cy * cy + cz * cz), dot) * RAD_TO_DEG;
    } else {
      curRoll = curPitch = curTilt = NAN;
    }

    evaluateAlarm();
    lastEvalMs = millis();
    sdLogRow();

    Serial.printf("%-7s tilt %6.3f  roll %6.3f  pitch %6.3f | f %6.3f Hz%s  base %6.3f | vib %6.2f mg\n",
                  levelName(alarmLevel), curTilt, curRoll, curPitch,
                  haveAna ? lastAna.freq : NAN, (haveAna && lastAna.freqValid) ? " " : "?",
                  baselineFreq, haveAna ? lastAna.vibRms : NAN);
  } else if (millis() - lastEvalMs > 2000) {
    // no fresh tilt data (sensor fault) - keep status and cloud updated
    curRoll = curPitch = curTilt = NAN;
    evaluateAlarm();
    lastEvalMs = millis();
  }

  updateOutputs();
  sdMaintain();
  delay(5);
}
