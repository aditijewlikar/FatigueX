/*
  =====================================================================
  Worker Safety Helmet - Multi-Sensor Fatigue Monitor
  Target: ESP32 Dev Board
  =====================================================================

  SENSORS
    - MAX30102 (SparkFun MAX3010x library, I2C shared) -> Heart rate
    - HW-870 IR blink sensor (digital DO -> GPIO32)     -> Eye closure
    - MPU6050 CLONE (raw register I2C, NO Adafruit lib) -> Pitch/roll,
                                                            jerk, and
                                                            internal die
                                                            temperature
  OUTPUT
    - Buzzer -> GPIO27

  =====================================================================
  WHY THIS SKETCH DOES NOT USE Adafruit_MPU6050
  =====================================================================
  Adafruit_MPU6050::begin() hard-fails unless WHO_AM_I reads 0x68. Your
  clone reports 0x70, so this sketch talks to the MPU6050 registers
  directly via Wire and does NOT gate init on WHO_AM_I.

  =====================================================================
  ALERT ARCHITECTURE (three independent paths)
  =====================================================================

  PATH 1 - SUSTAINED FATIGUE                                [CHANGED]
    Fatigue flags: Blink (eyes closed), Tilt/Jerk, Temp.
    When at least FATIGUE_FLAGS_REQUIRED of them are true (default: ALL 3),
    a millis() timer starts. If the condition holds CONTINUOUSLY for
    FATIGUE_SUSTAIN_MS (5 s) -> fatigue alert + buzzer.
    If the condition breaks at any moment, the timer resets to zero.

  PATH 2 - HEART RATE ALERT (serial + dashboard + buzzer)           [CHANGED]
    Average BPM outside [HR_NORMAL_MIN_BPM, HR_NORMAL_MAX_BPM] for
    HR_ALERT_SUSTAIN_MS -> text alert AND buzzer.
    Heart rate does NOT feed Path 1; it is its own separate condition.
    Disable its buzzer with HR_ALERT_BUZZER_ENABLED = false.

  PATH 3 - HEAD-JERK MASTER OVERRIDE (alert + buzzer)               [CHANGED]
    JERK_COUNT_REQUIRED (3) sudden head jerks inside a rolling
    JERK_WINDOW_MS (60 s) window -> instant alert + buzzer. Evaluated
    on every MPU sample regardless of eyes / BPM / temp / tilt /
    the Path 1 timer. It never waits on any other parameter.

  BUZZER RULE (all three alerts): CONTINUOUS loud tone, and
      - it sounds for AT LEAST that alert's minimum time (5 s each,
        separate constants: FATIGUE_/HR_/JERK_BEEP_MIN_MS), and can
        never be cut short by anything else;
      - if the alert condition is STILL true after the minimum time, the
        buzzer KEEPS sounding until the condition clears.
  Implemented in startBeep() + updateBuzzer(), millis()-based, non-blocking.

  No delay() is used anywhere in loop(); every timer is millis()-based.
  =====================================================================
*/

#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// =====================================================================
// NETWORK & DASHBOARD SETTINGS
// =====================================================================
const char* ssid = "Galaxy A26 5G BDFF";
const char* password = "hotspot65";
const char* backendUrl = "http://10.131.73.178:3000/api/telemetry";
const char* workerId = "ESP32-HLM-01";
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000UL; // boot-time wait, then run without dashboard
const unsigned long WIFI_RETRY_INTERVAL_MS   = 15000UL; // [NEW] non-blocking retry from loop() after that

// WebSocketsServer removed in favor of HTTP POST to Node.js backend
unsigned long lastBroadcastTime = 0;

// =====================================================================
// NON-BLOCKING HTTP POST TASK
// =====================================================================
String postPayload = "";
TaskHandle_t postTaskHandle;

void postTask(void * parameter) {
  for(;;) {
    if (postPayload.length() > 0 && WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(backendUrl);
      http.addHeader("Content-Type", "application/json");
      http.POST(postPayload);
      http.end();
      postPayload = ""; // Clear payload after sending
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}


// Variables for 2-Minute Fatigue Logic (Dashboard)
bool fatigue_timer_active = false;
unsigned long fatigue_timer_start = 0;
unsigned long timer_elapsed_ms = 0;

// =====================================================================
// PIN DEFINITIONS
// =====================================================================
#define I2C_SDA_PIN     21
#define I2C_SCL_PIN     22
#define EYE_SENSOR_PIN  32   // HW-870 IR blink sensor, digital output
#define BUZZER_PIN      25   // Alert buzzer output

// ---- Buzzer type ------------------------------------------------------
// ACTIVE buzzer  (has its own oscillator, sounds from a steady HIGH): false
// PASSIVE buzzer (needs a tone/PWM signal; a steady HIGH is silent or
//                 just clicks once):                                  true
// If the boot self-test beep is silent, try flipping this.
#define BUZZER_IS_PASSIVE   false
#define BUZZER_TONE_HZ      2500   // passive buzzers only
#define BUZZER_LEDC_CHANNEL 0      // passive buzzers, older ESP32 core (2.x) only

// =====================================================================
// MPU6050 REGISTER MAP (accessed directly, no Adafruit library)
// =====================================================================
const uint8_t MPU_I2C_ADDR         = 0x68; // change to 0x69 if AD0 is pulled high
const uint8_t MPU_REG_PWR_MGMT_1   = 0x6B;
const uint8_t MPU_REG_SMPLRT_DIV   = 0x19;
const uint8_t MPU_REG_CONFIG       = 0x1A;
const uint8_t MPU_REG_GYRO_CONFIG  = 0x1B;
const uint8_t MPU_REG_ACCEL_CONFIG = 0x1C;
const uint8_t MPU_REG_ACCEL_XOUT_H = 0x3B; // 14-byte burst: accel(6) + temp(2) + gyro(6)
const uint8_t MPU_REG_WHO_AM_I     = 0x75;

const uint8_t MPU_WHOAMI_GENUINE   = 0x68; // standard MPU6050
const uint8_t MPU_WHOAMI_CLONE     = 0x70; // this board's clone chip

// =====================================================================
// ================ TUNABLE THRESHOLDS -- FIELD-TEST HERE =============
// =====================================================================

// ---- 1) Heart Rate -- ISOLATED ALERT (no buzzer)              [CHANGED]
// Normal range. Average BPM outside this range raises the HR alert.
const int   HR_NORMAL_MIN_BPM           = 60;    // below this  -> LOW heart-rate alert
const int   HR_NORMAL_MAX_BPM           = 100;   // above this  -> HIGH heart-rate alert
const unsigned long HR_ALERT_SUSTAIN_MS = 3000UL;  // must stay out of range this long (0 = instant)
const unsigned long HR_ALERT_REPEAT_MS  = 10000UL; // re-print reminder while still out of range
const long  MIN_IR_FOR_VALID_READING    = 10000; // below this = no skin contact, ignore reading

// [CHANGED] Heart-rate alert also sounds the buzzer (one 5 s beep per alert episode)
const bool  HR_ALERT_BUZZER_ENABLED     = true;

// [CHANGED] MINIMUM time the buzzer sounds continuously, separately per alert.
// The buzzer never stops before this. If the alert condition is still true
// afterwards, it keeps sounding until the condition clears.
const unsigned long FATIGUE_BEEP_MIN_MS = 5000UL;
const unsigned long HR_BEEP_MIN_MS      = 5000UL;
const unsigned long JERK_BEEP_MIN_MS    = 5000UL;

// ---- 2) Eye Blink (Blink Flag) ----
const unsigned long EYE_CLOSED_MICROSLEEP_MS = 800; // continuous closure beyond this = microsleep

// ---- 3) Head Kinematics ----
const float TILT_ANGLE_THRESHOLD_DEG        = 35.0f;  // pitch or roll beyond this = unusual angle
const unsigned long TILT_SUSTAINED_MS       = 5000;   // must stay tilted this long to count
const unsigned long JERK_FLAG_HOLD_MS       = 4000;   // how long a jerk keeps the soft Tilt/Jerk flag latched

// ---- 4) Body Temperature (Temp Flag) ----
// MPU6050 INTERNAL DIE temperature (not skin/ambient). Placeholders --
// log your own baseline before trusting these.
const float TEMP_DIE_LOW_THRESHOLD_C   = 25.0f;
const float TEMP_DIE_HIGH_THRESHOLD_C  = 45.0f;
const float TEMP_DIE_OFFSET_C          = -20.0f;

// ---- PATH 1: Sustained fatigue (buzzer)                       [CHANGED]
// Flags counted: Blink, Tilt/Jerk, Temp  (heart rate is NOT counted).
// [CHANGED] Now requires ALL 3 parameters. Set to 2 for "any 2 of 3",
// or 1 to let any single flag start the 5 s timer.
const int FATIGUE_FLAGS_REQUIRED          = 3;
const unsigned long FATIGUE_SUSTAIN_MS    = 5000UL; // must hold CONTINUOUSLY this long

// =====================================================================
// [NEW] PATH 3: HEAD-JERK MASTER OVERRIDE -- TUNABLE CONSTANTS
// =====================================================================
// Sample-to-sample change in total acceleration magnitude (g) that counts
// as one sudden head jerk. Lower = more sensitive. Watch the "[JERK]"
// serial lines while testing to find a good value.
// [CHANGED] Measured as the peak-to-peak swing in accel magnitude within the
// last JERK_SPAN_SAMPLES samples (~100 ms). Because the MPU is now sampled
// every 20 ms, a short head snap can no longer fall between two samples.
const float JERK_ACCEL_DELTA_THRESHOLD_G   = 1.0f;
const int   JERK_SPAN_SAMPLES              = 6;   // 6 samples x 20 ms = ~100 ms look-back

// Number of jerks needed inside the rolling window to fire the override.
const int   JERK_COUNT_REQUIRED            = 3;

// Rolling window length: 60 seconds.
const unsigned long JERK_WINDOW_MS         = 60000UL;

// Refractory period: one physical snap produces a burst of high deltas
// over several samples (and a rebound). Ignore further detections for
// this long after a counted jerk so one snap = one jerk.
const unsigned long JERK_DEBOUNCE_MS       = 1000UL;

// How long the override alert stays active after the 3rd jerk (a jerk is a
// momentary event). The buzzer sounds at least this long. A further override
// (3 more jerks) restarts it. Keep this >= JERK_BEEP_MIN_MS.
const unsigned long JERK_OVERRIDE_ALERT_HOLD_MS = 5000UL;

// =====================================================================
// TIMING INTERVALS (non-blocking, millis()-based)
// =====================================================================
const unsigned long MPU_READ_INTERVAL_MS     = 20;   // [CHANGED] was 100 ms: fast enough to catch a head snap
const unsigned long EVAL_INTERVAL_MS         = 250;  // fatigue + HR alert evaluation
const unsigned long SERIAL_PRINT_INTERVAL_MS = 1000; // field-tuning status dump
// MAX30102 IR read + eye digital read happen every loop pass (unthrottled)
// since beat detection needs fast, continuous sampling.

// =====================================================================
// SENSOR OBJECTS
// =====================================================================
MAX30105 particleSensor;
bool hrSensorAvailable = false;
bool mpuAvailable      = false;

// =====================================================================
// GLOBAL STATE - LATEST CACHED SENSOR VALUES
// =====================================================================
float headPitchDeg   = 0.0f;
float headRollDeg    = 0.0f;
float accelMagG      = 0.0f;
float dieTempC       = 0.0f;

const byte RATE_SIZE = 4;
byte  rateBuffer[RATE_SIZE];
byte  rateSpot            = 0;
byte  beatSamplesCollected = 0;   // [NEW] how many real beats are in rateBuffer (fixes low-average startup)
long  lastBeatMs          = 0;
float instantBpm          = 0.0f;
int   beatAvgBpm          = 0;
bool  fingerDetected      = false;

// =====================================================================
// ANOMALY FLAGS
// =====================================================================
bool blinkFlag    = false;
bool tiltJerkFlag = false;
bool tempFlag     = false;
// (bpmFlag removed -- heart rate is now its own isolated alert below)

// ---- Sub-state supporting the tilt/jerk flag ----
bool tiltSustainedActive           = false;
unsigned long tiltConditionStartMs = 0;
bool jerkLatchActive               = false;
unsigned long jerkLatchStartMs     = 0;
float previousAccelMagG            = 0.0f;
bool firstAccelSample              = true;

// ---- Sub-state supporting the blink flag ----
unsigned long eyeClosedStartMs = 0;

// =====================================================================
// PATH 1 STATE: SUSTAINED FATIGUE                              [CHANGED]
// =====================================================================
bool fatigueWindowActive         = false; // true while >= FATIGUE_FLAGS_REQUIRED flags are true
unsigned long fatigueTimerStart  = 0;     // [millis() TIMER] stamped when the window opens
bool fatigueAlertActive          = false; // true once held for FATIGUE_SUSTAIN_MS

// =====================================================================
// [NEW] PATH 2 STATE: HEART RATE ALERT (no buzzer)
// =====================================================================
bool hrAlertActive                = false;
bool hrOutOfRangeTiming           = false;
unsigned long hrOutOfRangeStartMs = 0;  // [millis() TIMER] when BPM first left the normal range
unsigned long hrLastAlertPrintMs  = 0;  // [millis() TIMER] for the repeat reminder

// =====================================================================
// [NEW] PATH 3 STATE: HEAD-JERK COUNTER + MASTER OVERRIDE
// =====================================================================
float jerkMagHistory[JERK_SPAN_SAMPLES];      // [NEW] ring of recent accel magnitudes
int   jerkHistoryLen              = 0;
int   jerkHistoryPos              = 0;
float peakSwingG                  = 0.0f;     // [NEW] biggest swing since last status print (tuning aid)
unsigned long jerkTimes[JERK_COUNT_REQUIRED]; // [JERK COUNTER] timestamps of jerks inside the window
int   jerkCount                   = 0;        // [JERK COUNTER] jerks currently inside the rolling window
unsigned long jerkLastCountedMs   = 0;        // for debounce
bool  jerkHasBeenCounted          = false;    // so millis()==0 can't block the first jerk
bool  jerkOverrideActive          = false;    // true while the override holds alert + buzzer
unsigned long jerkOverrideStartMs = 0;        // [millis() TIMER] when the override fired

// =====================================================================
// SCHEDULER TIMESTAMPS
// =====================================================================
bool  buzzerIsOn               = false;   // last state written to the buzzer
bool  beepHoldActive           = false;   // [NEW] minimum-sound period in progress
unsigned long beepHoldUntilMs  = 0;       // [NEW] [millis() TIMER] buzzer may not stop before this
const char*   beepReason       = "";      // [NEW] which alert started it (for the status print)
bool  wifiWasConnected         = false;   // [NEW]
unsigned long lastWifiRetry    = 0;       // [NEW] [millis() TIMER] for non-blocking WiFi retry
unsigned long lastMpuRead      = 0;
unsigned long lastEval         = 0;
unsigned long lastSerialPrint  = 0;

// =====================================================================
// FORWARD DECLARATIONS
// =====================================================================
void updateBuzzer();
void startBeep(const char* reason, unsigned long minDurationMs);
void buzzerSet(bool on);
void printWifiFailureReason();
const char* getHrStatus();

// =====================================================================
// LOW-LEVEL MPU6050 REGISTER ACCESS (no Adafruit library involved)
// =====================================================================
void mpuWriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU_I2C_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

uint8_t mpuReadRegister(uint8_t reg) {
  Wire.beginTransmission(MPU_I2C_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false); // repeated start, keep bus held
  Wire.requestFrom((int)MPU_I2C_ADDR, 1, (int)true);
  if (Wire.available()) {
    return Wire.read();
  }
  return 0;
}

// Clone-tolerant init routine: does NOT gate on WHO_AM_I value.
bool mpuBeginClone() {
  Wire.beginTransmission(MPU_I2C_ADDR);
  uint8_t ackError = Wire.endTransmission();
  if (ackError != 0) {
    Serial.println(F("[MPU INIT] No ACK from MPU6050 at expected address -- check wiring."));
    return false;
  }

  uint8_t whoAmI = mpuReadRegister(MPU_REG_WHO_AM_I);
  Serial.print(F("[MPU INIT] WHO_AM_I = 0x"));
  Serial.println(whoAmI, HEX);

  if (whoAmI == MPU_WHOAMI_GENUINE) {
    Serial.println(F("[MPU INIT] Genuine MPU6050 identified."));
  } else if (whoAmI == MPU_WHOAMI_CLONE) {
    Serial.println(F("[MPU INIT] Known clone (0x70) identified -- proceeding with direct register init."));
  } else {
    Serial.println(F("[MPU INIT] WARNING: unexpected WHO_AM_I value. Proceeding anyway -- "
                      "the standard MPU6050 register map is used regardless."));
  }

  // Wake the device (clear SLEEP bit) and select the gyro-X PLL as clock source.
  mpuWriteRegister(MPU_REG_PWR_MGMT_1, 0x01);
  delay(50); // one-time power-up settle time in setup(), never used in loop()

  mpuWriteRegister(MPU_REG_SMPLRT_DIV, 0x07);    // sample rate = 1kHz / (1+7) = 125 Hz
  mpuWriteRegister(MPU_REG_CONFIG, 0x03);        // DLPF ~44 Hz bandwidth
  mpuWriteRegister(MPU_REG_GYRO_CONFIG, 0x00);   // +-250 deg/s (unused)
  mpuWriteRegister(MPU_REG_ACCEL_CONFIG, 0x00);  // +-2g range -> 16384 LSB/g

  return true;
}

// Reads accel + die temp in one burst and computes pitch/roll/accel magnitude/temperature.
// Returns false on I2C failure; caller keeps last-known values.
bool mpuReadAll(float &pitchDegOut, float &rollDegOut, float &accelMagGOut, float &dieTempCOut) {
  Wire.beginTransmission(MPU_I2C_ADDR);
  Wire.write(MPU_REG_ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) return false;

  uint8_t received = Wire.requestFrom((int)MPU_I2C_ADDR, 14, (int)true);
  if (received < 14) return false;

  int16_t rawAx   = (Wire.read() << 8) | Wire.read();
  int16_t rawAy   = (Wire.read() << 8) | Wire.read();
  int16_t rawAz   = (Wire.read() << 8) | Wire.read();
  int16_t rawTemp = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read(); // gyro X (unused, must be drained)
  Wire.read(); Wire.read(); // gyro Y
  Wire.read(); Wire.read(); // gyro Z

  const float ACCEL_SCALE_LSB_PER_G = 16384.0f; // for +-2g range configured above

  float ax = rawAx / ACCEL_SCALE_LSB_PER_G;
  float ay = rawAy / ACCEL_SCALE_LSB_PER_G;
  float az = rawAz / ACCEL_SCALE_LSB_PER_G;

  // Pitch/roll from accelerometer only. Assumes MPU6050 X axis points
  // toward the front of the head -- adjust if your mounting differs.
  pitchDegOut  = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0f / PI;
  rollDegOut   = atan2(ay, az) * 180.0f / PI;
  accelMagGOut = sqrt(ax * ax + ay * ay + az * az);

  dieTempCOut = (rawTemp / 340.0f) + 36.53f;

  return true;
}

// =====================================================================
// [NEW] WIFI DIAGNOSTICS + NON-BLOCKING RECONNECT
// =====================================================================
// Prints WHY the connection failed and scans for the hotspot. The scan
// is the definitive test: if your SSID is not in the list, the ESP32
// cannot see it (hotspot off, out of range, or on 5 GHz).
void printWifiFailureReason() {
  Serial.print(F("[WIFI] Status code: "));
  Serial.print((int)WiFi.status());
  Serial.print(F(" -> "));
  switch (WiFi.status()) {
    case WL_NO_SSID_AVAIL:
      Serial.println(F("SSID NOT FOUND. Hotspot is off, out of range, hidden, or on 5 GHz (ESP32 is 2.4 GHz ONLY).")); break;
    case WL_CONNECT_FAILED:
      Serial.println(F("Connection rejected -- most likely a WRONG PASSWORD (check case/spaces).")); break;
    case WL_CONNECTION_LOST:
      Serial.println(F("Connection was lost.")); break;
    case WL_DISCONNECTED:
      Serial.println(F("Not connected (no answer from the access point, or auth timed out).")); break;
    case WL_IDLE_STATUS:
      Serial.println(F("Idle -- still trying.")); break;
    default:
      Serial.println(F("Other/unknown state.")); break;
  }

  Serial.println(F("[WIFI] Scanning for nearby 2.4 GHz networks (one-time, ~3 s)..."));
  int n = WiFi.scanNetworks();
  bool found = false;
  for (int i = 0; i < n; i++) {
    bool match = (WiFi.SSID(i) == String(ssid));
    if (match) found = true;
    Serial.print(match ? F("  --> ") : F("      "));
    Serial.print(WiFi.SSID(i));
    Serial.print(F("  ch "));
    Serial.print(WiFi.channel(i));
    Serial.print(F("  "));
    Serial.print(WiFi.RSSI(i));
    Serial.println(F(" dBm"));
  }
  if (n <= 0) Serial.println(F("  (no networks seen at all -- check the antenna/board)"));
  Serial.println(found
    ? F("[WIFI] Your hotspot IS visible -> the password is almost certainly wrong.")
    : F("[WIFI] Your hotspot is NOT visible -> on the phone: turn hotspot ON, set Band to 2.4 GHz, "
        "enable 'Maximize compatibility', and stay close to the ESP32."));
  WiFi.scanDelete();
}

// Called every loop pass. Never blocks: WiFi.begin() just kicks off a connect attempt.
void maintainWifi() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.print(F("[WIFI] Connected. IP address: "));
      Serial.println(WiFi.localIP());
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println(F("[WIFI] Connection lost."));
  }

  // [millis() TIMER] retry every WIFI_RETRY_INTERVAL_MS
  if (now - lastWifiRetry >= WIFI_RETRY_INTERVAL_MS) {
    lastWifiRetry = now;
    Serial.println(F("[WIFI] Retrying connection..."));
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }
}

// =====================================================================
// SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  xTaskCreatePinnedToCore(postTask, "HTTP_POST", 4096, NULL, 1, &postTaskHandle, 0);
  unsigned long serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart < 2000)) {
    ; // brief wait for USB serial, don't hang forever
  }

  // ---- Buzzer init + boot self-test ----
  pinMode(BUZZER_PIN, OUTPUT);
#if BUZZER_IS_PASSIVE
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcAttach(BUZZER_PIN, BUZZER_TONE_HZ, 8);                 // ESP32 core 3.x
  #else
    ledcSetup(BUZZER_LEDC_CHANNEL, BUZZER_TONE_HZ, 8);         // ESP32 core 2.x
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
  #endif
#endif
  buzzerSet(false);
  // [NEW] One 300 ms beep at power-up (one-time delay in setup only).
  // If you do NOT hear this, the problem is the buzzer wiring/type, not the alert logic.
  buzzerSet(true);
  delay(300);
  buzzerSet(false);
  buzzerIsOn = false;

  pinMode(EYE_SENSOR_PIN, INPUT); // HW-870 DO is push-pull digital output

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  Serial.println(F("========================================="));
  Serial.println(F(" Worker Safety Helmet - Initializing..."));
  Serial.println(F("========================================="));

  // ---- 1) MPU6050 (clone-tolerant direct register init) ----
  Serial.print(F("[INIT] MPU6050 (clone-tolerant)... "));
  mpuAvailable = mpuBeginClone();
  Serial.println(mpuAvailable ? F("OK") : F("FAILED - tilt/jerk/temp detection disabled"));

  delay(50); // brief settle before touching the shared I2C bus again

  // ---- 2) MAX30102 ----
  Serial.print(F("[INIT] MAX30102... "));
  if (particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x0A);  // low-power red LED
    particleSensor.setPulseAmplitudeGreen(0);   // green LED unused
    hrSensorAvailable = true;
    Serial.println(F("OK"));
  } else {
    Serial.println(F("FAILED - heart rate detection disabled"));
    hrSensorAvailable = false;
  }

  // [NEW] Jerk counter init
  for (int i = 0; i < JERK_COUNT_REQUIRED; i++) {
    jerkTimes[i] = 0;
  }
  jerkCount = 0;
  Serial.println(F("[INIT] Head-jerk master override armed."));

  // ---- 3) WiFi / dashboard ----
  // [CHANGED] Explicit station mode + clean state, time-limited wait, and a
  // reason printout if it fails. loop() keeps retrying without blocking.
  Serial.print(F("\n[INIT] Connecting to WiFi: "));
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS)) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiWasConnected = true;
    Serial.println(F("\n[INIT] WiFi connected."));
    Serial.print(F("[INIT] IP address: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("\n[INIT] WiFi NOT connected -- running without dashboard (alerts + buzzer still work)."));
    printWifiFailureReason();
    lastWifiRetry = millis();
  }

  // webSocket.begin() removed
  Serial.println(F("[INIT] WebSocket server started on port 81."));

  Serial.println(F("========================================="));
  Serial.println(F(" System ready. Monitoring fatigue..."));
  Serial.println(F("========================================="));
}

// =====================================================================
// SENSOR UPDATE FUNCTIONS
// =====================================================================
void updateKinematicsAndTemp() {
  if (!mpuAvailable) return;

  float p, r, mag, t;
  if (mpuReadAll(p, r, mag, t)) {
    headPitchDeg = p;
    headRollDeg  = r;
    accelMagG    = mag;
    dieTempC     = t + TEMP_DIE_OFFSET_C;
  }
}

// Eye sensor: raw read every loop, sustained-closure timing for microsleep detection.
void updateBlinkFlag() {
  bool eyesClosedNow = (digitalRead(EYE_SENSOR_PIN) == LOW);
  unsigned long now = millis();

  if (eyesClosedNow) {
    if (eyeClosedStartMs == 0) {
      eyeClosedStartMs = now;
    }
    blinkFlag = (now - eyeClosedStartMs) >= EYE_CLOSED_MICROSLEEP_MS;
  } else {
    eyeClosedStartMs = 0;
    blinkFlag = false;
  }
}

// Heart rate: called every loop pass (unthrottled) so beat detection
// doesn't miss pulse peaks. This function only MEASURES; the alert
// decision is made in updateHeartRateAlert() below.
void updateHeartRate() {
  if (!hrSensorAvailable) return;

  long irValue = particleSensor.getIR();
  fingerDetected = (irValue >= MIN_IR_FOR_VALID_READING);

  if (!fingerDetected) {
    // [NEW] Lost contact: discard stale averaging data so an old value
    // can't raise a false HR alert when contact returns.
    beatAvgBpm = 0;
    beatSamplesCollected = 0;
    rateSpot = 0;
    return;
  }

  if (checkForBeat(irValue)) {
    long nowMs = millis();
    long delta = nowMs - lastBeatMs;
    lastBeatMs = nowMs;

    instantBpm = 60.0f / (delta / 1000.0f);

    if (instantBpm > 20 && instantBpm < 255) {
      rateBuffer[rateSpot++] = (byte)instantBpm;
      rateSpot %= RATE_SIZE;

      // [CHANGED] Average only over beats actually collected. The old code
      // divided by RATE_SIZE while the buffer was still zero-filled, giving
      // falsely low averages (18, 36, 54...) for the first few beats.
      if (beatSamplesCollected < RATE_SIZE) beatSamplesCollected++;
      int sum = 0;
      for (byte i = 0; i < beatSamplesCollected; i++) sum += rateBuffer[i];
      beatAvgBpm = sum / beatSamplesCollected;
    }
  }
}

// Soft Tilt/Jerk flag (feeds PATH 1): sustained extreme angle OR a latched jerk.
// Jerk DETECTION itself now lives in updateJerkDetection() (single detector);
// this function only reads the latch that detector sets.
void updateTiltJerkFlag() {
  if (!mpuAvailable) { tiltJerkFlag = false; return; }

  unsigned long now = millis();

  // ---- Sustained extreme tilt (pitch OR roll) ----
  bool extremeAngleNow = (fabs(headPitchDeg) > TILT_ANGLE_THRESHOLD_DEG) ||
                          (fabs(headRollDeg)  > TILT_ANGLE_THRESHOLD_DEG);

  if (extremeAngleNow) {
    if (tiltConditionStartMs == 0) {
      tiltConditionStartMs = now;
    }
    tiltSustainedActive = (now - tiltConditionStartMs) >= TILT_SUSTAINED_MS;
  } else {
    tiltConditionStartMs = 0;
    tiltSustainedActive = false;
  }

  // ---- Expire the jerk latch (set by updateJerkDetection) ----
  if (jerkLatchActive && (now - jerkLatchStartMs >= JERK_FLAG_HOLD_MS)) {
    jerkLatchActive = false;
  }

  tiltJerkFlag = tiltSustainedActive || jerkLatchActive;
}

void updateTempFlag() {
  if (!mpuAvailable) { tempFlag = false; return; }
  tempFlag = (dieTempC < TEMP_DIE_LOW_THRESHOLD_C) || (dieTempC > TEMP_DIE_HIGH_THRESHOLD_C);
}

// =====================================================================
// SINGLE BUZZER OUTPUT                                         [CHANGED]
// =====================================================================
// Low-level on/off. Handles active (steady HIGH) and passive (tone) buzzers.
void buzzerSet(bool on) {
#if BUZZER_IS_PASSIVE
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcWriteTone(BUZZER_PIN, on ? BUZZER_TONE_HZ : 0);
  #else
    ledcWriteTone(BUZZER_LEDC_CHANNEL, on ? BUZZER_TONE_HZ : 0);
  #endif
#else
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
#endif
}

// [CHANGED] Start the alarm with a guaranteed MINIMUM sounding time.
// Called by all three alert paths, each with its own minimum. If an alarm
// is already sounding, the hold is only ever EXTENDED, never shortened.
void startBeep(const char* reason, unsigned long minDurationMs) {
  unsigned long now = millis();
  unsigned long newEnd = now + minDurationMs;

  if (!beepHoldActive || (long)(newEnd - beepHoldUntilMs) > 0) {
    beepHoldUntilMs = newEnd;      // [millis() TIMER] can't stop before this
  }
  beepHoldActive = true;
  beepReason = reason;

  Serial.print(F("[BUZZER] Alarm ON (min "));
  Serial.print(minDurationMs / 1000.0f, 1);
  Serial.print(F(" s): "));
  Serial.println(reason);
  updateBuzzer();                  // sound immediately, don't wait for the next loop pass
}

// The ONLY place that decides the buzzer state. Called every loop pass.
// Buzzer is ON (continuous) while EITHER
//   - the minimum-sound hold has not expired yet, OR
//   - any alert condition is still active (fatigue / heart rate / jerk override).
// It turns OFF only when both are false. Non-blocking.
void updateBuzzer() {
  if (beepHoldActive && (long)(millis() - beepHoldUntilMs) >= 0) {
    beepHoldActive = false;        // minimum time is over
  }

  bool conditionActive = fatigueAlertActive ||
                         (HR_ALERT_BUZZER_ENABLED && hrAlertActive) ||
                         jerkOverrideActive;
  bool wantOn = beepHoldActive || conditionActive;

  if (wantOn != buzzerIsOn) {      // only touch the pin/tone when the state changes
    buzzerIsOn = wantOn;
    buzzerSet(wantOn);
    if (!wantOn) Serial.println(F("[BUZZER] Alarm OFF (condition cleared, minimum time met)."));
  }
}

// =====================================================================
// PATH 1 -- SUSTAINED FATIGUE: FLAGS HELD FOR 5 CONTINUOUS SECONDS [CHANGED]
// =====================================================================
void evaluateSustainedFatigue() {
  int activeCount = (blinkFlag ? 1 : 0) + (tiltJerkFlag ? 1 : 0) + (tempFlag ? 1 : 0);
  unsigned long now = millis();

  if (activeCount >= FATIGUE_FLAGS_REQUIRED) {

    // Condition just became true -> start the millis() timer.
    if (!fatigueWindowActive) {
      fatigueWindowActive = true;
      fatigueTimerStart = now;
      Serial.print(F("[INFO] "));
      Serial.print(activeCount);
      Serial.print(F(" fatigue flag(s) active. "));
      Serial.print(FATIGUE_SUSTAIN_MS / 1000);
      Serial.println(F("s sustain timer started."));
    }

    // Still true -> has it been true, unbroken, for FATIGUE_SUSTAIN_MS?
    if (!fatigueAlertActive && (now - fatigueTimerStart >= FATIGUE_SUSTAIN_MS)) {
      fatigueAlertActive = true;
      Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
      Serial.println(F("Worker is fatigued"));
      Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
      startBeep("fatigue (all parameters met)", FATIGUE_BEEP_MIN_MS);
    }

  } else {
    // Condition broke (even for one pass) -> timer resets to zero.
    if (fatigueWindowActive) {
      Serial.println(F("[INFO] Fatigue condition broken. Sustain timer reset to zero."));
    }
    fatigueWindowActive = false;
    fatigueTimerStart = 0;

    if (fatigueAlertActive) {
      fatigueAlertActive = false;
      Serial.println(F("[INFO] Fatigue alert cleared."));
    }
  }
}

// =====================================================================
// [CHANGED] PATH 2 -- HEART RATE ALERT (serial + dashboard + 5 s beep)
// =====================================================================
// Returns "no_signal", "low", "high" or "normal" (instantaneous status).
const char* getHrStatus() {
  if (!fingerDetected || beatSamplesCollected < RATE_SIZE) return "no_signal"; // not enough real beats yet
  if (beatAvgBpm < HR_NORMAL_MIN_BPM) return "low";
  if (beatAvgBpm > HR_NORMAL_MAX_BPM) return "high";
  return "normal";
}

void printHrAlert() {
  Serial.print(F("[HR ALERT] Heart rate "));
  Serial.print(strcmp(getHrStatus(), "low") == 0 ? F("LOW") : F("HIGH"));
  Serial.print(F(": "));
  Serial.print(beatAvgBpm);
  Serial.print(F(" BPM (normal "));
  Serial.print(HR_NORMAL_MIN_BPM);
  Serial.print(F("-"));
  Serial.print(HR_NORMAL_MAX_BPM);
  Serial.println(HR_ALERT_BUZZER_ENABLED ? F("). Buzzer ON (min 5 s).") : F("). Buzzer disabled for HR."));
}

void updateHeartRateAlert() {
  unsigned long now = millis();
  const char* status = getHrStatus();
  bool outOfRange = (strcmp(status, "low") == 0) || (strcmp(status, "high") == 0);

  if (outOfRange) {
    // [millis() TIMER] BPM must stay out of range for HR_ALERT_SUSTAIN_MS.
    if (!hrOutOfRangeTiming) {
      hrOutOfRangeTiming = true;
      hrOutOfRangeStartMs = now;
    }

    if (!hrAlertActive) {
      if (now - hrOutOfRangeStartMs >= HR_ALERT_SUSTAIN_MS) {
        hrAlertActive = true;
        hrLastAlertPrintMs = now;
        printHrAlert();
        if (HR_ALERT_BUZZER_ENABLED) startBeep("abnormal heart rate", HR_BEEP_MIN_MS);
      }
    } else if (now - hrLastAlertPrintMs >= HR_ALERT_REPEAT_MS) {
      hrLastAlertPrintMs = now;       // periodic reminder, no spam
      printHrAlert();
    }
  } else {
    if (hrAlertActive) {
      Serial.println(F("[HR ALERT] Cleared (heart rate back in range or contact lost)."));
    }
    hrAlertActive = false;
    hrOutOfRangeTiming = false;
  }
  // The buzzer stays on while hrAlertActive is true (see updateBuzzer()).
}

// =====================================================================
// [NEW] PATH 3 -- HEAD-JERK COUNTER + MASTER OVERRIDE
// =====================================================================

// [JERK COUNTER] Drop jerks that have aged out of the rolling window.
// Called every MPU sample so the count decays in real time, not just
// when a new jerk arrives. Timestamps are kept in chronological order.
void pruneJerkWindow(unsigned long now) {
  int kept = 0;
  for (int i = 0; i < jerkCount; i++) {
    if (now - jerkTimes[i] < JERK_WINDOW_MS) {
      jerkTimes[kept++] = jerkTimes[i];
    }
  }
  jerkCount = kept;
}

// Single jerk detector, called right after each MPU6050 read (every 20 ms).
//  1) Peak-to-peak swing of accel magnitude over the last ~100 ms >= threshold = one jerk.
//     (Looking back over a short history instead of only the previous
//      sample means a snap is caught no matter where it falls between samples.)
//  2) Each (debounced) jerk is stored in the rolling 60 s window.
//  3) JERK_COUNT_REQUIRED jerks inside the window -> IMMEDIATE alert +
//     buzzer. No other sensor or flag is consulted (master override).
void updateJerkDetection() {
  if (!mpuAvailable) return;

  unsigned long now = millis();

  // ---- Release the override hold after JERK_OVERRIDE_ALERT_HOLD_MS ----
  if (jerkOverrideActive && (now - jerkOverrideStartMs >= JERK_OVERRIDE_ALERT_HOLD_MS)) {
    jerkOverrideActive = false;
    Serial.println(F("[JERK] Override hold ended."));
  }

  // ---- Age out old jerks (rolling window) ----
  pruneJerkWindow(now);

  // ---- Push this sample into the short history ring ----
  jerkMagHistory[jerkHistoryPos] = accelMagG;
  jerkHistoryPos = (jerkHistoryPos + 1) % JERK_SPAN_SAMPLES;
  if (jerkHistoryLen < JERK_SPAN_SAMPLES) jerkHistoryLen++;
  if (jerkHistoryLen < JERK_SPAN_SAMPLES) return;   // wait until the ring is full

  float lo = jerkMagHistory[0], hi = jerkMagHistory[0];
  for (int i = 1; i < JERK_SPAN_SAMPLES; i++) {
    if (jerkMagHistory[i] < lo) lo = jerkMagHistory[i];
    if (jerkMagHistory[i] > hi) hi = jerkMagHistory[i];
  }
  float swingG = hi - lo;
  if (swingG > peakSwingG) peakSwingG = swingG;     // tuning aid, shown in status print

  if (swingG < JERK_ACCEL_DELTA_THRESHOLD_G) return;

  // Every raw jerk also latches the soft Tilt/Jerk flag (PATH 1 input).
  jerkLatchActive = true;
  jerkLatchStartMs = now;

  // ---- Debounce: one physical snap counts as one jerk ----
  if (jerkHasBeenCounted && (now - jerkLastCountedMs < JERK_DEBOUNCE_MS)) return;

  // ---- [JERK COUNTER] Record this jerk in the rolling window ----
  jerkTimes[jerkCount++] = now;
  jerkLastCountedMs = now;
  jerkHasBeenCounted = true;

  Serial.print(F("[JERK] Sudden head jerk. Swing = "));
  Serial.print(swingG, 2);
  Serial.print(F(" g. Count "));
  Serial.print(jerkCount);
  Serial.print(F(" / "));
  Serial.print(JERK_COUNT_REQUIRED);
  Serial.print(F(" within "));
  Serial.print(JERK_WINDOW_MS / 1000);
  Serial.println(F("s."));

  // ---- MASTER OVERRIDE: threshold reached -> alert + buzzer NOW ----
  if (jerkCount >= JERK_COUNT_REQUIRED) {
    jerkOverrideActive = true;
    jerkOverrideStartMs = now;
    Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
    Serial.println(F("[JERK OVERRIDE] 3 head jerks in 60s -- ALERT + BUZZER"));
    Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
    startBeep("3 head jerks in 60 s", JERK_BEEP_MIN_MS);
    jerkCount = 0; // next episode must earn 3 fresh jerks
  }
}

// =====================================================================
// FIELD-TUNING STATUS DISPLAY
// =====================================================================
void printStatus() {
  Serial.println(F("------------------------------------------"));

  Serial.print(F("[MAX30102] BPM (real-time): "));
  Serial.print(instantBpm, 1);
  Serial.print(F("  Avg BPM: "));
  if (!fingerDetected) {
    Serial.println(F("-- (no contact detected)"));
  } else if (beatSamplesCollected < RATE_SIZE) {
    Serial.println(F("-- (collecting beats)"));
  } else {
    Serial.println(beatAvgBpm);
  }

  Serial.print(F("[HR ALERT] Status: "));
  Serial.print(getHrStatus());
  Serial.print(F("  Alert: "));
  Serial.println(hrAlertActive ? F("ACTIVE") : F("clear"));

  Serial.print(F("[HW-870]   Eyes: "));
  Serial.println(blinkFlag ? F("CLOSED >800ms (microsleep)") :
                  (digitalRead(EYE_SENSOR_PIN) == LOW ? F("Closed (brief)") : F("Open")));

  Serial.print(F("[MPU6050]  Pitch: "));
  Serial.print(headPitchDeg, 1);
  Serial.print(F(" deg  Roll: "));
  Serial.print(headRollDeg, 1);
  Serial.print(F(" deg  DieTemp: "));
  Serial.print(dieTempC, 1);
  Serial.println(F(" C"));

  Serial.print(F("[FLAGS]    Blink:"));
  Serial.print(blinkFlag ? F("TRIG ") : F("ok   "));
  Serial.print(F(" Tilt/Jerk:"));
  Serial.print(tiltJerkFlag ? F("TRIG ") : F("ok   "));
  Serial.print(F(" Temp:"));
  Serial.println(tempFlag ? F("TRIG") : F("ok"));

  if (fatigueWindowActive) {
    unsigned long elapsedMs = millis() - fatigueTimerStart;
    Serial.print(F("[TIMER]    Fatigue condition held: "));
    Serial.print(elapsedMs / 1000.0f, 1);
    Serial.print(F("s / "));
    Serial.print(FATIGUE_SUSTAIN_MS / 1000);
    Serial.println(F("s"));
  } else {
    Serial.println(F("[TIMER]    Fatigue timer idle"));
  }

  Serial.print(F("[JERKS]    In window: "));
  Serial.print(jerkCount);
  Serial.print(F(" / "));
  Serial.print(JERK_COUNT_REQUIRED);
  Serial.print(F("  Override: "));
  Serial.println(jerkOverrideActive ? F("ACTIVE") : F("no"));

  Serial.print(F("  Peak swing last 1s: "));
  Serial.print(peakSwingG, 2);
  Serial.print(F(" g (jerk threshold "));
  Serial.print(JERK_ACCEL_DELTA_THRESHOLD_G, 2);
  Serial.println(F(" g)"));
  peakSwingG = 0.0f;

  Serial.print(F("[BUZZER]   "));
  if (buzzerIsOn) {
    Serial.print(F("ON - "));
    Serial.print(beepReason);
    if (beepHoldActive) {
      Serial.print(F(" (minimum time left: "));
      Serial.print((long)(beepHoldUntilMs - millis()) / 1000.0f, 1);
      Serial.println(F("s)"));
    } else {
      Serial.println(F(" (condition still active)"));
    }
  } else {
    Serial.println(F("off"));
  }

  Serial.println(F("------------------------------------------"));
}

// =====================================================================
// DASHBOARD BROADCAST + 2-MINUTE DASHBOARD TIMER
// =====================================================================
void updateNetworkAndFatigueTimer() {
  // webSocket.loop() removed

  unsigned long now = millis();

  // 1. Evaluate Dashboard Logic
  bool dangerPitch = fabs(headPitchDeg) > 30.0f || fabs(headRollDeg) > 30.0f;
  bool dangerEyes = (digitalRead(EYE_SENSOR_PIN) == LOW);
  bool dangerBpm = (beatAvgBpm > 0 && beatAvgBpm < HR_NORMAL_MIN_BPM); // [CHANGED] uses the tunable constant

  // If ALL THREE parameters hit danger thresholds
  if (dangerPitch && dangerEyes && dangerBpm) {
    if (!fatigue_timer_active) {
      fatigue_timer_active = true;
      fatigue_timer_start = now;
      Serial.println(F("[DASHBOARD] 2-Minute Danger Timer STARTED."));
    }
    timer_elapsed_ms = now - fatigue_timer_start;
  } else {
    if (fatigue_timer_active) {
      Serial.println(F("[DASHBOARD] Danger Timer RESET (Sensors Normalized)."));
    }
    fatigue_timer_active = false;
    fatigue_timer_start = 0;
    timer_elapsed_ms = 0;
  }

  // 2. Broadcast Payload Every 500ms
  if (now - lastBroadcastTime >= 500) {
    lastBroadcastTime = now;

    StaticJsonDocument<384> doc;   // [CHANGED] enlarged for the new fields
    doc["worker_id"] = workerId;
    doc["pitch"] = headPitchDeg;
    doc["roll"] = headRollDeg;
    doc["eyes_closed"] = dangerEyes;
    doc["heart_rate"] = (beatAvgBpm > 0) ? beatAvgBpm : 0.0;
    doc["temperature"] = dieTempC;
    doc["fatigue_timer_active"] = fatigue_timer_active;
    doc["timer_elapsed_ms"] = timer_elapsed_ms;

    // [NEW] Fields for the isolated HR alert, jerk counter and buzzer alerts
    doc["hr_alert"]      = hrAlertActive;        // true = sustained out-of-range BPM (no buzzer)
    doc["hr_status"]     = getHrStatus();        // "low" | "high" | "normal" | "no_signal"
    doc["jerk_count"]    = jerkCount;            // jerks currently in the 60 s window
    doc["jerk_override"] = jerkOverrideActive;   // 3-jerk master override active
    doc["fatigue_alert"] = fatigueAlertActive;   // 5 s sustained-fatigue alert active

    char jsonString[384];
    serializeJson(doc, jsonString);

    if (postPayload.length() == 0) {
        postPayload = String(jsonString); // Handoff to background task
      }
  }
}

// =====================================================================
// MAIN LOOP - fully non-blocking, no delay()
// =====================================================================
void loop() {
  unsigned long now = millis();

  // Networking + dashboard broadcast
  updateNetworkAndFatigueTimer();
  maintainWifi();          // [NEW] non-blocking WiFi retry

  // Time-critical / cheap reads: every loop pass
  updateBlinkFlag();
  updateHeartRate();

  // Kinematics + die temperature + jerk detection: fast interval.
  // [millis() TIMER] MPU_READ_INTERVAL_MS
  if (now - lastMpuRead >= MPU_READ_INTERVAL_MS) {
    lastMpuRead = now;
    updateKinematicsAndTemp();
    updateJerkDetection();   // [NEW] jerk counter + master override (instant buzzer)
    updateTiltJerkFlag();
    updateTempFlag();
  }

  // Alert evaluation.
  // [millis() TIMER] EVAL_INTERVAL_MS
  if (now - lastEval >= EVAL_INTERVAL_MS) {
    lastEval = now;
    evaluateSustainedFatigue();  // [CHANGED] 5 s continuous timer -> buzzer
    updateHeartRateAlert();      // [NEW] separate HR alert, never buzzes
  }

  // [NEW] Advance the buzzer pattern every pass (HR pulse timing)
  updateBuzzer();

  // Field-tuning status display
  if (now - lastSerialPrint >= SERIAL_PRINT_INTERVAL_MS) {
    lastSerialPrint = now;
    printStatus();
  }
}
