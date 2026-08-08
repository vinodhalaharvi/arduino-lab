// Elegoo V4: TB6612 motors + HC-SR04 ultrasonic, with autonomous avoid.
//
//   ping              one distance reading
//   watch             stream distance until any key
//   pins <trig> <echo> retarget the sensor at runtime
//   fwd|rev|left|right <0-255>
//   auto              autonomous obstacle avoidance
//   stop              halt everything (also exits auto)
//   status

// ---- motors (verified) ----------------------------------------------------
const uint8_t PIN_PWMA = 5;
const uint8_t PIN_PWMB = 6;
const uint8_t PIN_AIN  = 7;
const uint8_t PIN_BIN  = 8;
const uint8_t PIN_STBY = 3;

const uint8_t DIR_FWD    = HIGH;
const uint8_t DIR_REV    = LOW;
const uint8_t SPEED_STOP = 0;
const uint8_t SPEED_MAX  = 255;

// ---- ultrasonic (defaults; override with `pins`) --------------------------
uint8_t pinTrig = 13;
uint8_t pinEcho = 12;

const uint16_t TRIG_PULSE_US   = 10;
const uint32_t ECHO_TIMEOUT_US = 25000UL;   // ~4.2 m ceiling
const float    CM_PER_US       = 0.0343;    // speed of sound, round trip halved
const int16_t  DIST_INVALID    = -1;
const uint8_t  SAMPLES         = 3;         // median filter

// ---- autonomous tuning ----------------------------------------------------
const int16_t  STOP_CM      = 25;
const uint8_t  CRUISE_SPEED = 170;
const uint8_t  TURN_SPEED   = 190;
const uint16_t BACK_MS      = 400;
const uint16_t TURN_MS      = 450;
const uint16_t SENSE_MS     = 60;

enum AutoState : uint8_t { AUTO_OFF, AUTO_DRIVE, AUTO_BACK, AUTO_TURN };
AutoState autoState = AUTO_OFF;
uint32_t  stateSince = 0;
uint32_t  lastSense  = 0;
int16_t   lastDist   = DIST_INVALID;

const uint32_t BAUD = 115200;
char    buf[32];
uint8_t len = 0;

void setup() {
  Serial.begin(BAUD);
  while (!Serial) { ; }
  delay(50);

  pinMode(PIN_PWMA, OUTPUT); pinMode(PIN_PWMB, OUTPUT);
  pinMode(PIN_AIN,  OUTPUT); pinMode(PIN_BIN,  OUTPUT);
  pinMode(PIN_STBY, OUTPUT);
  digitalWrite(PIN_STBY, HIGH);
  halt();

  configSensor();
  Serial.println(F("ready. ping | watch | pins <t> <e> | fwd|rev|left|right <s> | auto | stop"));
}

void loop() {
  readSerial();
  if (autoState != AUTO_OFF) stepAuto();
}

// ---- motors ---------------------------------------------------------------

void drive(uint8_t sR, uint8_t dR, uint8_t sL, uint8_t dL) {
  digitalWrite(PIN_AIN, dR); analogWrite(PIN_PWMA, sR);
  digitalWrite(PIN_BIN, dL); analogWrite(PIN_PWMB, sL);
}

void halt() { drive(SPEED_STOP, DIR_FWD, SPEED_STOP, DIR_FWD); }

// ---- ultrasonic -----------------------------------------------------------

void configSensor() {
  pinMode(pinTrig, OUTPUT);
  pinMode(pinEcho, INPUT);
  digitalWrite(pinTrig, LOW);
}

// One shot. Returns cm, or DIST_INVALID on timeout.
int16_t pingOnce() {
  digitalWrite(pinTrig, LOW);
  delayMicroseconds(2);
  digitalWrite(pinTrig, HIGH);
  delayMicroseconds(TRIG_PULSE_US);
  digitalWrite(pinTrig, LOW);

  uint32_t us = pulseIn(pinEcho, HIGH, ECHO_TIMEOUT_US);
  if (us == 0) return DIST_INVALID;
  return (int16_t)(us * CM_PER_US / 2.0);
}

// Median of 3 — ultrasonic throws wild single-sample spikes off soft or
// angled surfaces, and the median kills them where an average would not.
int16_t distance() {
  int16_t v[SAMPLES];
  for (uint8_t i = 0; i < SAMPLES; i++) {
    v[i] = pingOnce();
    delay(12);                    // let echoes die before re-firing
  }
  for (uint8_t i = 1; i < SAMPLES; i++)
    for (uint8_t j = i; j > 0 && v[j] < v[j-1]; j--) {
      int16_t t = v[j]; v[j] = v[j-1]; v[j-1] = t;
    }
  return v[SAMPLES / 2];
}

// ---- autonomous state machine --------------------------------------------

void enter(AutoState s) { autoState = s; stateSince = millis(); }

void stepAuto() {
  uint32_t now = millis();

  switch (autoState) {
    case AUTO_DRIVE:
      if (now - lastSense >= SENSE_MS) {
        lastSense = now;
        lastDist = distance();
        if (lastDist != DIST_INVALID && lastDist < STOP_CM) {
          Serial.print(F("obstacle ")); Serial.println(lastDist);
          halt();
          enter(AUTO_BACK);
          return;
        }
      }
      drive(CRUISE_SPEED, DIR_FWD, CRUISE_SPEED, DIR_FWD);
      break;

    case AUTO_BACK:
      drive(CRUISE_SPEED, DIR_REV, CRUISE_SPEED, DIR_REV);
      if (now - stateSince >= BACK_MS) enter(AUTO_TURN);
      break;

    case AUTO_TURN:
      drive(TURN_SPEED, DIR_FWD, TURN_SPEED, DIR_REV);
      if (now - stateSince >= TURN_MS) { lastSense = 0; enter(AUTO_DRIVE); }
      break;

    default: break;
  }
}

// ---- command parsing ------------------------------------------------------

const char *argAfter(const char *cmd, const char *pre) {
  size_t n = strlen(pre);
  return strncmp(cmd, pre, n) == 0 ? cmd + n : nullptr;
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { buf[len] = '\0'; if (len) handle(buf); len = 0; }
    else if (len < 31) buf[len++] = c;
  }
}

void handle(const char *cmd) {
  const char *arg;
  int a, b;

  if (!strcmp(cmd, "stop")) {
    autoState = AUTO_OFF; halt();
    Serial.println(F("ok: stop"));

  } else if (!strcmp(cmd, "ping")) {
    int16_t d = distance();
    Serial.print(F("dist="));
    if (d == DIST_INVALID) Serial.println(F("none")); else Serial.println(d);

  } else if (!strcmp(cmd, "watch")) {
    Serial.println(F("streaming - send anything to stop"));
    while (!Serial.available()) {
      int16_t d = distance();
      Serial.print(F("dist="));
      if (d == DIST_INVALID) Serial.println(F("none")); else Serial.println(d);
      delay(150);
    }
    while (Serial.available()) Serial.read();

  } else if (!strcmp(cmd, "auto")) {
    lastSense = 0;
    enter(AUTO_DRIVE);
    Serial.println(F("ok: auto - send stop to halt"));

  } else if (!strcmp(cmd, "status")) {
    Serial.print(F("trig=")); Serial.print(pinTrig);
    Serial.print(F(" echo=")); Serial.print(pinEcho);
    Serial.print(F(" auto=")); Serial.print(autoState);
    Serial.print(F(" last=")); Serial.println(lastDist);

  } else if (sscanf(cmd, "pins %d %d", &a, &b) == 2) {
    pinTrig = (uint8_t)a; pinEcho = (uint8_t)b;
    configSensor();
    Serial.print(F("ok: trig=")); Serial.print(pinTrig);
    Serial.print(F(" echo=")); Serial.println(pinEcho);

  } else if ((arg = argAfter(cmd, "fwd ")) || (arg = argAfter(cmd, "rev ")) ||
             (arg = argAfter(cmd, "left ")) || (arg = argAfter(cmd, "right "))) {
    long s = atol(arg);
    if (s < SPEED_STOP || s > SPEED_MAX) { Serial.println(F("err: 0..255")); return; }
    autoState = AUTO_OFF;
    uint8_t sp = (uint8_t)s;
    if      (cmd[0] == 'f') drive(sp, DIR_FWD, sp, DIR_FWD);
    else if (cmd[0] == 'r' && cmd[1] == 'e') drive(sp, DIR_REV, sp, DIR_REV);
    else if (cmd[0] == 'l') drive(sp, DIR_FWD, sp, DIR_REV);
    else                    drive(sp, DIR_REV, sp, DIR_FWD);
    Serial.print(F("ok: ")); Serial.print(cmd[0]); Serial.print(' '); Serial.println(sp);

  } else {
    Serial.println(F("err: unknown"));
  }
}
