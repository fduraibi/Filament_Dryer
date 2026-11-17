// ============================================================================
//  3D Printer Filament Dryer  -  v2
//
//  Control : simple on/off with hysteresis (target +/- tempTolerance)
//  Target  : adjustable 30..50 C with two buttons, stored in EEPROM
//  Fan     : wired DIRECTLY to 12 V / GND (always on). Only its tach is read.
//  Safety  : see the "SAFETY" notes below. Any latched fault turns the heater
//            OFF and needs a power cycle to clear.
//
//  Buttons : wired between pin and GND (internal pull-ups are used)
//            Hold BOTH for 1.5 s -> config mode, then + / - (hold to repeat).
//            3 s without any button activity -> value is saved and used.
//
//  By      : Fahad Alduraibi
//            https://fadvisor.net
// ============================================================================
#include <EEPROM.h>
#include <avr/wdt.h>
#include <GyverOLED.h>
#include "DHT.h"

// ---------------------------------------------------------------- Pins ------
#define DHTPIN 4
#define DHTTYPE DHT22
#define FANSENSORPIN 2  // tach, external interrupt pin
#define HEATERPIN 3     // ACTIVE LOW: LOW = Heater ON, HIGH = Heater OFF
#define BTN_PLUS 8      // button to GND
#define BTN_MINUS 7     // button to GND

// --------------------------------------------------- Default settings -------
const uint8_t DEFAULT_TARGET = 45;  // used on first boot / corrupted EEPROM
const uint8_t TARGET_MIN = 30;      // Lowest allowed temperature
const uint8_t TARGET_MAX = 50;      // Highest allowed temperature
const float tempTolerance = 1.0;    // heater [ON < target - tol], [OFF > target + tol]

const uint16_t FAN_MAX_PULSES = 700;  // more than this = noise on the tach line, NOT a healthy fan -> treated as failure
const uint32_t FAN_MIN_GAP_US = 1500;  // ignore tach edges closer than this (ringing / noise)
const uint16_t FAN_MIN_PULSES = 200;   // min tach pulses per 2 s (measured ~340 with a healthy fan)

// ------------------------------------------------------ Safety limits -------
const float HARD_MAX_TEMP = 55.0;           // latch fault at/above this (C)
const float SENSOR_MIN_VALID = -10.0;       // readings outside this range are invalid
const float SENSOR_MAX_VALID = 80.0;
const float SENSOR_MAX_JUMP = 8.0;          // max plausible change between 2 readings (C)
const uint8_t SENSOR_FAIL_LIMIT = 5;        // consecutive bad readings before latching
const uint8_t FAN_FAIL_LIMIT = 2;           // consecutive bad fan windows before latching
const uint32_t HEAT_CHECK_MS = 5UL * 60UL * 1000UL;  // heater continuously ON this long (5min = 5*60*1000 milliseconds)
const float HEAT_MIN_RISE = 0.5;            // must raise temperature by at least this (C), if sensor is not reading temp rise then something is wrong
#define USE_WATCHDOG 1                      // set 0 if your bootloader is the old Nano one

// ------------------------------------------------------ UI timing -----------
const uint32_t SENSOR_INTERVAL_MS = 2100;   // DHT22 needs >= 2 s between reads
const uint32_t COMBO_HOLD_MS = 1500;        // time both buttons held to enter config
const uint32_t CONFIG_TIMEOUT_MS = 3000;    // idle time in config before saving
const uint32_t DEBOUNCE_MS = 30;
const uint32_t REPEAT_DELAY_MS = 500;
const uint32_t REPEAT_RATE_MS = 150;

// ------------------------------------------------------ EEPROM layout -------
const int EE_MAGIC = 0;  // 0xA5
const int EE_VALUE = 1;  // target (C)
const int EE_CHECK = 2;  // ~target
const uint8_t EE_MAGIC_VALUE = 0xA5;

// ------------------------------------------------------ State ---------------
enum Fault : uint8_t {
  FAULT_NONE,
  FAULT_SENSOR,    // DHT keeps failing / implausible
  FAULT_OVERTEMP,  // temperature >= HARD_MAX_TEMP
  FAULT_FAN,       // fan not spinning
  FAULT_NO_RISE    // heater ON but temperature not rising (stuck/dead sensor or heater)
};

GyverOLED<SSH1106_128x64> oled;

DHT dht(DHTPIN, DHTTYPE);

volatile uint16_t fanPulses = 0;
volatile uint32_t fanLastEdgeUs = 0;

float TargetTemp = DEFAULT_TARGET;
float lastT = NAN, lastH = NAN;
Fault fault = FAULT_NONE;

bool heaterIsOn = false;
bool fanProven = false;  // tach shows the fan spinning -> heater allowed
uint8_t fanFailCount = 0;
uint8_t sensorFailCount = 0;
bool sensorBad = false;

uint32_t lastTick = 0;
uint32_t heatCheckStart = 0;  // 0 = not tracking
float heatCheckTemp = 0;

bool configMode = false;
uint8_t pendingTarget = DEFAULT_TARGET;
bool plusDown = false, minusDown = false;
uint32_t savedMsUntil = 0;
bool displayDirty = true;

// ======================================================= Hardware helpers ===
void fanISR() {
  uint32_t n = micros();
  if (n - fanLastEdgeUs >= FAN_MIN_GAP_US) {
    fanPulses++;
    fanLastEdgeUs = n;
  }
}

void heaterOff() {
  digitalWrite(HEATERPIN, HIGH);
  heaterIsOn = false;
}

// The ONLY place the heater is switched on. Refuses unless it is safe.
void heaterOn() {
  if (fault != FAULT_NONE || sensorBad || !fanProven) {
    heaterOff();
    return;
  }
  digitalWrite(HEATERPIN, LOW);
  heaterIsOn = true;
}

void latchFault(Fault f) {
  if (fault == FAULT_NONE) fault = f;
  heaterOff();
  displayDirty = true;
  Serial.print(F("FAULT "));
  Serial.println((uint8_t)f);
}

// ============================================================= EEPROM =======
void loadTarget() {
  uint8_t magic = EEPROM.read(EE_MAGIC);
  uint8_t v = EEPROM.read(EE_VALUE);
  uint8_t chk = EEPROM.read(EE_CHECK);
  if (magic == EE_MAGIC_VALUE && (uint8_t)~v == chk && v >= TARGET_MIN && v <= TARGET_MAX) {
    TargetTemp = v;
  } else {
    TargetTemp = DEFAULT_TARGET;
  }
  pendingTarget = (uint8_t)TargetTemp;
}

void saveTarget(uint8_t v) {
  EEPROM.update(EE_VALUE, v);
  EEPROM.update(EE_CHECK, (uint8_t)~v);
  EEPROM.update(EE_MAGIC, EE_MAGIC_VALUE);  // magic last: valid only if the rest was written
}

// ============================================================= Buttons ======
void updateButtons(uint32_t now) {
  static bool rawP = false, rawM = false;
  static uint32_t changedAt = 0;
  static bool prevP = false, prevM = false;
  static uint32_t comboStart = 0, lastActivity = 0, repeatAt = 0;
  static bool waitRelease = false;

  bool p = digitalRead(BTN_PLUS) == LOW;
  bool m = digitalRead(BTN_MINUS) == LOW;
  if (p != rawP || m != rawM) {
    rawP = p;
    rawM = m;
    changedAt = now;
  }
  if (now - changedAt >= DEBOUNCE_MS) {
    plusDown = rawP;
    minusDown = rawM;
  }

  if (!configMode) {
    // Enter config only after BOTH buttons are held continuously
    if (plusDown && minusDown) {
      if (comboStart == 0) comboStart = now;
      else if (now - comboStart >= COMBO_HOLD_MS) {
        configMode = true;
        pendingTarget = (uint8_t)TargetTemp;
        waitRelease = true;  // ignore buttons until both are let go
        lastActivity = now;
        comboStart = 0;
        displayDirty = true;
      }
    } else {
      comboStart = 0;
    }
  } else {
    if (plusDown || minusDown) lastActivity = now;  // holding counts as activity

    if (waitRelease) {
      if (!plusDown && !minusDown) {
        waitRelease = false;
        lastActivity = now;
      }
    } else if (plusDown != minusDown) {  // exactly one button
      bool pressEdge = (plusDown && !prevP) || (minusDown && !prevM);
      bool repeat = !pressEdge && now >= repeatAt;
      if (pressEdge || repeat) {
        if (plusDown && pendingTarget < TARGET_MAX) pendingTarget++;
        if (minusDown && pendingTarget > TARGET_MIN) pendingTarget--;
        repeatAt = now + (pressEdge ? REPEAT_DELAY_MS : REPEAT_RATE_MS);
        displayDirty = true;
      }
    }

    // Idle long enough -> save and leave
    if (!waitRelease && !plusDown && !minusDown && now - lastActivity >= CONFIG_TIMEOUT_MS) {
      if (pendingTarget != (uint8_t)TargetTemp) {
        TargetTemp = pendingTarget;
        saveTarget(pendingTarget);
        savedMsUntil = now + 1500;
      }
      configMode = false;
      displayDirty = true;
    }
  }
  prevP = plusDown;
  prevM = minusDown;
}

// ======================================================= Control (every tick)
void controlTick(uint32_t now) {
  // --- fan tach for the window that just ended ---
  noInterrupts();
  uint16_t pulses = fanPulses;
  fanPulses = 0;
  interrupts();
  uint32_t elapsed = now - lastTick;
  lastTick = now;
  uint16_t pulsesPer2s = (uint16_t)((uint32_t)pulses * 2000UL / elapsed);

  // --- read sensor and validate ---
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  bool valid = !isnan(h) && !isnan(t) && t >= SENSOR_MIN_VALID && t <= SENSOR_MAX_VALID && h >= 0.0 && h <= 100.0;
  if (valid && !isnan(lastT) && fabs(t - lastT) > SENSOR_MAX_JUMP) valid = false;

  if (!valid) {
    heaterOff();  // never heat blind
    sensorBad = true;
    displayDirty = true;
    if (++sensorFailCount >= SENSOR_FAIL_LIMIT) latchFault(FAULT_SENSOR);
    Serial.println(F("Sensor invalid"));
    return;
  }
  sensorBad = false;
  sensorFailCount = 0;
  lastT = t;
  lastH = h;
  displayDirty = true;

  // --- hard over-temperature cut-out ---
  if (t >= HARD_MAX_TEMP) {
    latchFault(FAULT_OVERTEMP);
    return;
  }

  // --- fan verification (fan is always on, so every window is valid) ---
  if (pulsesPer2s >= FAN_MIN_PULSES && pulsesPer2s <= FAN_MAX_PULSES) {
    fanProven = true;
    fanFailCount = 0;
  } else {
    fanProven = false;
    heaterOff();
    if (++fanFailCount >= FAN_FAIL_LIMIT) {
      latchFault(FAULT_FAN);
      return;
    }
  }

  // --- no-temperature-rise watchdog (catches stuck-low sensor or dead heater) ---
  if (heaterIsOn) {
    if (heatCheckStart == 0) {
      heatCheckStart = now;
      heatCheckTemp = t;
    } else if (now - heatCheckStart >= HEAT_CHECK_MS) {
      if (t - heatCheckTemp < HEAT_MIN_RISE) {
        latchFault(FAULT_NO_RISE);
        return;
      }
      heatCheckStart = now;
      heatCheckTemp = t;
    }
  } else {
    heatCheckStart = 0;
  }

  // --- on/off control with hysteresis ---
  bool wantHeat = heaterIsOn ? (t < TargetTemp + tempTolerance) : (t < TargetTemp - tempTolerance);
  if (wantHeat) {
    if (!heaterIsOn) heaterOn();  // refuses unless the fan is proven by the tach
  } else {
    heaterOff();
  }

  Serial.print(heaterIsOn ? F("Heating... ") : F("Idle... "));
  Serial.print(F("Temp = "));
  Serial.print(t);
  Serial.print(F("  Target = "));
  Serial.print(TargetTemp);
  Serial.print(F("  Fan pulses/2s = "));
  Serial.println(pulsesPer2s);
}

// ============================================================== Setup =======
void setup() {
  // Heater OFF *before* making the pin an output (avoids a brief LOW = ON glitch)
  digitalWrite(HEATERPIN, HIGH);
  pinMode(HEATERPIN, OUTPUT);
  pinMode(FANSENSORPIN, INPUT_PULLUP);  // plus an external 4.7k-10k pull-up to 5V is strongly recommended
  pinMode(BTN_PLUS, INPUT_PULLUP);
  pinMode(BTN_MINUS, INPUT_PULLUP);
  heaterIsOn = false;

  Serial.begin(9600);
  loadTarget();

  oled.init();
  dht.begin();
  attachInterrupt(digitalPinToInterrupt(FANSENSORPIN), fanISR, RISING);

  oled.clear();
  oled.println(F("Welcome!"));
  oled.println(F("Booting up....."));
  oled.print(F("Target: "));
  oled.print((int)TargetTemp);
  oled.println(F(" C"));
  oled.update();

  lastTick = millis();  // first sensor read after the DHT has settled

#if USE_WATCHDOG
  wdt_enable(WDTO_8S);  // resets the MCU if the code ever hangs (heater pin returns to OFF at boot)
#endif
}

// ============================================================== Loop ========
void loop() {
#if USE_WATCHDOG
  wdt_reset();
#endif
  uint32_t now = millis();

  updateButtons(now);

  if (fault != FAULT_NONE) {
    heaterOff();  // enforced continuously while faulted
  } else if (now - lastTick >= SENSOR_INTERVAL_MS) {
    controlTick(now);
  }

  if (displayDirty || (savedMsUntil && (int32_t)(now - savedMsUntil) >= 0)) {
    if (savedMsUntil && (int32_t)(now - savedMsUntil) >= 0) savedMsUntil = 0;
    displayDirty = false;
    if (fault != FAULT_NONE) DisplayFault();
    else if (sensorBad) DisplaySensorWarning();
    else DisplayDHT(lastH, lastT);
  }
}

// ============================================================ Display =======
void DisplayFault() {
  oled.clear();
  oled.home();
  oled.setScale(2);
  oled.println(F("FAULT!"));
  oled.setScale(1);
  switch (fault) {
    case FAULT_SENSOR:   oled.println(F("Temp sensor failed")); break;
    case FAULT_OVERTEMP: oled.println(F("Over temperature")); break;
    case FAULT_FAN:      oled.println(F("Fan stalled")); break;
    case FAULT_NO_RISE:  oled.println(F("No temp rise (sensor/")); oled.println(F("heater problem)")); break;
    default: break;
  }
  oled.setCursorXY(0, 40);
  oled.println(F("Heater OFF"));
  oled.println(F("Power cycle to reset"));
  oled.update();
}

void DisplaySensorWarning() {
  oled.clear();
  oled.home();
  oled.println(F("Failed to read from"));
  oled.println(F("DHT sensor!"));
  oled.println(F("Heater paused"));
  oled.update();
}

void DisplayDHT(float h, float t) {
  oled.clear();
  oled.home();

  oled.setScale(1);
  oled.setCursorXY(0, 0);
  oled.print(F("Temperature:"));
  if (heaterIsOn) {
    oled.setCursorXY(76, 0);
    oled.print(F("ON"));  // heater indicator
  }
  oled.setScale(2);
  oled.setCursorXY(0, 10);
  oled.print(t);
  oled.print(F(" C"));

  // Draw the degree sign
  oled.circle(67, 12, 2, 1);
  oled.circle(67, 12, 1, 0);

  oled.fastLineH(31, 0, 90, 1);
  oled.fastLineV(90, 0, 63, 1);

  oled.setScale(1);
  oled.setCursorXY(0, 38);
  oled.print(F("Humidity:"));
  oled.setScale(2);
  oled.setCursorXY(0, 48);
  oled.print(h);
  oled.print(F(" %"));

  oled.setScale(1);
  oled.setCursorXY(103, 2);
  oled.print(configMode ? F("EDIT") : F("SET"));
  oled.setCursorXY(100, 16);
  oled.print(F("TEMP"));

  oled.fastLineH(31, 95, 127, 1);

  if (configMode) {
    oled.setScale(2);
    oled.setCursorXY(98, 38);
    oled.print(pendingTarget);
    oled.setScale(1);
  } else if (savedMsUntil) {
    oled.setCursorXY(97, 42);
    oled.print(F("SAVED"));
  } else {
    oled.setCursorXY(103, 42);
    oled.print((int)TargetTemp);
    oled.print(F("c"));
  }

  oled.setScale(1);
  oled.setCursorXY(95, 55);
  oled.print(F("-"));
  oled.setCursorXY(123, 55);
  oled.print(F("+"));

  oled.update();
}
