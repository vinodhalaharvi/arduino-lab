// Elegoo V4: TB6612 motors + panning HC-SR04 + servo.
//
//   look <deg>        aim the head (0..180, 90 = ahead)
//   servopin <p>      retarget the servo at runtime
//   pins <trig> <echo>
//   ping | scan       one reading / three-point sweep
//   fwd|rev|left|right <0-255>
//   auto              scan-and-choose obstacle avoidance
//   stop | status

#include <Servo.h>

// ---- motors (verified) ----------------------------------------------------
const uint8_t PIN_PWMA = 5, PIN_PWMB = 6;
const uint8_t PIN_AIN  = 7, PIN_BIN  = 8;
const uint8_t PIN_STBY = 3;
const uint8_t DIR_FWD = HIGH, DIR_REV = LOW;
const uint8_t SPEED_STOP = 0, SPEED_MAX = 255;

// ---- sensor / servo (verified trig+echo; servo pin is a guess) ------------
uint8_t pinTrig = 13, pinEcho = 12;
uint8_t pinServo = 10;
Servo head;

const uint8_t  LOOK_LEFT = 150, LOOK_AHEAD = 90, LOOK_RIGHT = 30;
const uint16_t SERVO_SETTLE_MS = 320;

const uint32_t ECHO_TIMEOUT_US = 25000UL;
const float    CM_PER_US       = 0.0343;
const int16_t  DIST_INVALID    = -1;
const int16_t  DIST_MIN_TRUST  = 3;      // HC-SR04 lies below ~2cm
const int16_t  DIST_FAR        = 400;    // treat "no echo" as wide open

// ---- autonomy -------------------------------------------------------------
const int16_t  STOP_CM      = 28;
const uint8_t  CRUISE_SPEED = 170;
const uint8_t  TURN_SPEED   = 190;
const uint16_t BACK_MS      = 380;
const uint16_t TURN_MS      = 420;
const uint16_t SENSE_MS     = 60;

enum St : uint8_t { OFF, DRIVE, BACK, SCAN, TURN };
St       state = OFF;
uint32_t since = 0, lastSense = 0;
int16_t  lastDist = DIST_INVALID;
bool     turnLeft = true;

char buf[32];
uint8_t len = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }
  delay(50);

  pinMode(PIN_PWMA, OUTPUT); pinMode(PIN_PWMB, OUTPUT);
  pinMode(PIN_AIN, OUTPUT);  pinMode(PIN_BIN, OUTPUT);
  pinMode(PIN_STBY, OUTPUT);
  digitalWrite(PIN_STBY, HIGH);
  halt();

  pinMode(pinTrig, OUTPUT); pinMode(pinEcho, INPUT);
  digitalWrite(pinTrig, LOW);

  head.attach(pinServo);
  head.write(LOOK_AHEAD);
  delay(400);

  Serial.println(F("ready. look|servopin|pins|ping|scan|fwd|rev|left|right|auto|stop"));
}

void loop() {
  readSerial();
  if (state != OFF) stepAuto();
}

// ---- motors ---------------------------------------------------------------
void drive(uint8_t sR, uint8_t dR, uint8_t sL, uint8_t dL) {
  digitalWrite(PIN_AIN, dR); analogWrite(PIN_PWMA, sR);
  digitalWrite(PIN_BIN, dL); analogWrite(PIN_PWMB, sL);
}
void halt() { drive(SPEED_STOP, DIR_FWD, SPEED_STOP, DIR_FWD); }

// ---- ranging --------------------------------------------------------------
int16_t pingOnce() {
  digitalWrite(pinTrig, LOW);  delayMicroseconds(2);
  digitalWrite(pinTrig, HIGH); delayMicroseconds(10);
  digitalWrite(pinTrig, LOW);
  uint32_t us = pulseIn(pinEcho, HIGH, ECHO_TIMEOUT_US);
  if (us == 0) return DIST_INVALID;
  int16_t cm = (int16_t)(us * CM_PER_US / 2.0);
  return cm < DIST_MIN_TRUST ? DIST_MIN_TRUST : cm;
}

int16_t distance() {
  int16_t v[3];
  for (uint8_t i = 0; i < 3; i++) { v[i] = pingOnce(); delay(12); }
  for (uint8_t i = 1; i < 3; i++)
    for (uint8_t j = i; j > 0 && v[j] < v[j-1]; j--) {
      int16_t t = v[j]; v[j] = v[j-1]; v[j-1] = t;
    }
  return v[1];
}

// Invalid means "nothing bounced back" — which is open space, not an obstacle.
int16_t openness(int16_t d) { return d == DIST_INVALID ? DIST_FAR : d; }

int16_t lookAndPing(uint8_t deg) {
  head.write(deg);
  delay(SERVO_SETTLE_MS);
  return distance();
}

// ---- autonomy -------------------------------------------------------------
void enter(St s) { state = s; since = millis(); }

void stepAuto() {
  uint32_t now = millis();

  switch (state) {
    case DRIVE:
      if (now - lastSense >= SENSE_MS) {
        lastSense = now;
        lastDist = distance();
        if (lastDist != DIST_INVALID && lastDist < STOP_CM) {
          Serial.print(F("blocked ")); Serial.println(lastDist);
          halt();
          enter(BACK);
          return;
        }
      }
      drive(CRUISE_SPEED, DIR_FWD, CRUISE_SPEED, DIR_FWD);
      break;

    case BACK:
      drive(CRUISE_SPEED, DIR_REV, CRUISE_SPEED, DIR_REV);
      if (now - since >= BACK_MS) { halt(); enter(SCAN); }
      break;

    case SCAN: {
      int16_t l = openness(lookAndPing(LOOK_LEFT));
      int16_t r = openness(lookAndPing(LOOK_RIGHT));
      head.write(LOOK_AHEAD);
      turnLeft = (l >= r);
      Serial.print(F("scan L=")); Serial.print(l);
      Serial.print(F(" R="));     Serial.print(r);
      Serial.println(turnLeft ? F(" -> left") : F(" -> right"));
      enter(TURN);
      break;
    }

    case TURN:
      if (turnLeft) drive(TURN_SPEED, DIR_FWD, TURN_SPEED, DIR_REV);
      else          drive(TURN_SPEED, DIR_REV, TURN_SPEED, DIR_FWD);
      if (now - since >= TURN_MS) { lastSense = 0; enter(DRIVE); }
      break;

    default: break;
  }
}

// ---- commands -------------------------------------------------------------
const char *argAfter(const char *c, const char *p) {
  size_t n = strlen(p);
  return strncmp(c, p, n) == 0 ? c + n : nullptr;
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { buf[len] = '\0'; if (len) handle(buf); len = 0; }
    else if (len < 31) buf[len++] = c;
  }
}

void printDist(int16_t d) {
  if (d == DIST_INVALID) Serial.println(F("none")); else Serial.println(d);
}

void handle(const char *cmd) {
  const char *arg;
  int a, b;

  if (!strcmp(cmd, "stop")) {
    state = OFF; halt(); head.write(LOOK_AHEAD);
    Serial.println(F("ok: stop"));

  } else if (!strcmp(cmd, "ping")) {
    Serial.print(F("dist=")); printDist(distance());

  } else if (!strcmp(cmd, "scan")) {
    Serial.print(F("L=")); Serial.print(openness(lookAndPing(LOOK_LEFT)));
    Serial.print(F(" C=")); Serial.print(openness(lookAndPing(LOOK_AHEAD)));
    Serial.print(F(" R=")); Serial.println(openness(lookAndPing(LOOK_RIGHT)));
    head.write(LOOK_AHEAD);

  } else if (!strcmp(cmd, "auto")) {
    lastSense = 0; enter(DRIVE);
    Serial.println(F("ok: auto"));

  } else if (!strcmp(cmd, "status")) {
    Serial.print(F("trig=")); Serial.print(pinTrig);
    Serial.print(F(" echo=")); Serial.print(pinEcho);
    Serial.print(F(" servo=")); Serial.print(pinServo);
    Serial.print(F(" state=")); Serial.print(state);
    Serial.print(F(" last=")); Serial.println(lastDist);

  } else if ((arg = argAfter(cmd, "look "))) {
    long d = atol(arg);
    if (d < 0 || d > 180) { Serial.println(F("err: 0..180")); return; }
    head.write((uint8_t)d);
    Serial.print(F("ok: look ")); Serial.println(d);

  } else if (sscanf(cmd, "servopin %d", &a) == 1) {
    head.detach(); pinServo = (uint8_t)a; head.attach(pinServo);
    head.write(LOOK_AHEAD);
    Serial.print(F("ok: servo pin ")); Serial.println(pinServo);

  } else if (sscanf(cmd, "pins %d %d", &a, &b) == 2) {
    pinTrig = (uint8_t)a; pinEcho = (uint8_t)b;
    pinMode(pinTrig, OUTPUT); pinMode(pinEcho, INPUT); digitalWrite(pinTrig, LOW);
    Serial.println(F("ok: pins set"));

  } else if ((arg = argAfter(cmd, "fwd ")) || (arg = argAfter(cmd, "rev ")) ||
             (arg = argAfter(cmd, "left ")) || (arg = argAfter(cmd, "right "))) {
    long s = atol(arg);
    if (s < 0 || s > SPEED_MAX) { Serial.println(F("err: 0..255")); return; }
    state = OFF;
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
