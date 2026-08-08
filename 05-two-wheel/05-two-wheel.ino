// Two-wheel differential drive, Adafruit Motor Shield v2.
//
//   test              sweep each port to identify wired motors
//   map <L> <R>       assign left/right ports, e.g.  map 1 2
//   fwd|rev <speed>   both wheels
//   left|right <speed> pivot in place
//   trim <percent>    bias right wheel to correct veer
//   stop | status

#include <Wire.h>
#include <Adafruit_MotorShield.h>

// ---- hardware -------------------------------------------------------------
const uint8_t  SHIELD_ADDR      = 0x60;   // default; solder jumpers shift it
const uint8_t  PORT_MIN         = 1;
const uint8_t  PORT_MAX         = 4;
const uint8_t  DEFAULT_PORT_L   = 1;
const uint8_t  DEFAULT_PORT_R   = 2;

// ---- serial ---------------------------------------------------------------
const uint32_t BAUD             = 115200;
const uint16_t UART_SETTLE_MS   = 50;     // suppresses reset garbage
const uint8_t  CMD_BUF_LEN      = 32;

// ---- motion limits --------------------------------------------------------
const uint8_t  SPEED_STOP       = 0;
const uint8_t  SPEED_MAX        = 255;
const int8_t   TRIM_MIN         = -100;
const int8_t   TRIM_MAX         =  100;
const int16_t  TRIM_BASE_PCT    =  100;   // 0% trim == unchanged

// ---- port sweep -----------------------------------------------------------
const uint8_t  SWEEP_SPEED      = 180;
const uint16_t SWEEP_RUN_MS     = 1500;
const uint16_t SWEEP_PAUSE_MS   = 600;

// ---- drive modes ----------------------------------------------------------
enum Mode : char {
  MODE_STOP    = 'S',
  MODE_FORWARD = 'F',
  MODE_REVERSE = 'R',
  MODE_PIVOT_L = 'L',
  MODE_PIVOT_R = 'T'
};

Adafruit_MotorShield shield = Adafruit_MotorShield(SHIELD_ADDR);

uint8_t portL = DEFAULT_PORT_L;
uint8_t portR = DEFAULT_PORT_R;
Adafruit_DCMotor *left;
Adafruit_DCMotor *right;

char    cmdBuf[CMD_BUF_LEN];
uint8_t cmdLen = 0;
uint8_t speed  = SPEED_STOP;
int8_t  trim   = 0;
Mode    mode   = MODE_STOP;

void setup() {
  Serial.begin(BAUD);
  while (!Serial) { ; }
  delay(UART_SETTLE_MS);

  if (!shield.begin()) {
    Serial.print(F("ERR: no shield at 0x"));
    Serial.println(SHIELD_ADDR, HEX);
    while (true) { ; }
  }
  bindPorts();
  Serial.println(F("ready. test | map | fwd | rev | left | right | trim | stop | status"));
}

void loop() { readSerial(); }

// ---- motor plumbing -------------------------------------------------------

void bindPorts() {
  left  = shield.getMotor(portL);
  right = shield.getMotor(portR);
  halt();
}

void halt() {
  speed = SPEED_STOP;
  mode  = MODE_STOP;
  left->setSpeed(SPEED_STOP);  left->run(RELEASE);
  right->setSpeed(SPEED_STOP); right->run(RELEASE);
}

// Right wheel gets the trim correction; identical motors never are.
uint8_t trimmed(uint8_t s) {
  int32_t adj = (int32_t)s * (TRIM_BASE_PCT + trim) / TRIM_BASE_PCT;
  if (adj > SPEED_MAX)  adj = SPEED_MAX;
  if (adj < SPEED_STOP) adj = SPEED_STOP;
  return (uint8_t)adj;
}

void drive(uint8_t s, uint8_t dirL, uint8_t dirR) {
  left->setSpeed(s);           left->run(dirL);
  right->setSpeed(trimmed(s)); right->run(dirR);
}

void sweepPorts() {
  Serial.println(F("-- wheels off the ground --"));
  for (uint8_t port = PORT_MIN; port <= PORT_MAX; port++) {
    Adafruit_DCMotor *m = shield.getMotor(port);
    Serial.print(F("M")); Serial.println(port);
    m->setSpeed(SWEEP_SPEED); m->run(FORWARD);
    delay(SWEEP_RUN_MS);
    m->setSpeed(SPEED_STOP);  m->run(RELEASE);
    delay(SWEEP_PAUSE_MS);
  }
  Serial.println(F("-- note which moved, then: map <L> <R> --"));
  bindPorts();
}

// ---- command parsing ------------------------------------------------------

// Returns the argument text if cmd starts with prefix, else nullptr.
// Keeps the literal and its length from drifting apart.
const char *argAfter(const char *cmd, const char *prefix) {
  size_t n = strlen(prefix);
  return strncmp(cmd, prefix, n) == 0 ? cmd + n : nullptr;
}

bool parseSpeed(const char *arg, uint8_t &out) {
  long v = atol(arg);
  if (v < SPEED_STOP || v > SPEED_MAX) {
    Serial.print(F("err: speed ")); Serial.print(SPEED_STOP);
    Serial.print(F("..")); Serial.println(SPEED_MAX);
    return false;
  }
  out = (uint8_t)v;
  return true;
}

void applyMove(const char *arg, Mode m, uint8_t dirL, uint8_t dirR) {
  uint8_t s;
  if (!parseSpeed(arg, s)) return;
  speed = s;
  mode  = m;
  drive(s, dirL, dirR);
  Serial.print(F("ok: ")); Serial.print((char)m);
  Serial.print(' ');       Serial.println(s);
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      cmdBuf[cmdLen] = '\0';
      if (cmdLen) handle(cmdBuf);
      cmdLen = 0;
    } else if (cmdLen < CMD_BUF_LEN - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

void handle(const char *cmd) {
  const char *arg;

  if (!strcmp(cmd, "test")) {
    sweepPorts();

  } else if ((arg = argAfter(cmd, "map "))) {
    int a = 0, b = 0;
    bool ok = sscanf(arg, "%d %d", &a, &b) == 2 &&
              a >= PORT_MIN && a <= PORT_MAX &&
              b >= PORT_MIN && b <= PORT_MAX && a != b;
    if (!ok) { Serial.println(F("err: map <1-4> <1-4>, distinct")); return; }
    halt();
    portL = (uint8_t)a;
    portR = (uint8_t)b;
    bindPorts();
    Serial.print(F("ok: L=M")); Serial.print(portL);
    Serial.print(F(" R=M"));    Serial.println(portR);

  } else if ((arg = argAfter(cmd, "fwd ")))   { applyMove(arg, MODE_FORWARD, FORWARD,  FORWARD);
  } else if ((arg = argAfter(cmd, "rev ")))   { applyMove(arg, MODE_REVERSE, BACKWARD, BACKWARD);
  } else if ((arg = argAfter(cmd, "left ")))  { applyMove(arg, MODE_PIVOT_L, BACKWARD, FORWARD);
  } else if ((arg = argAfter(cmd, "right "))) { applyMove(arg, MODE_PIVOT_R, FORWARD,  BACKWARD);

  } else if ((arg = argAfter(cmd, "trim "))) {
    long t = atol(arg);
    if (t < TRIM_MIN || t > TRIM_MAX) {
      Serial.print(F("err: trim ")); Serial.print(TRIM_MIN);
      Serial.print(F("..")); Serial.println(TRIM_MAX);
      return;
    }
    trim = (int8_t)t;
    Serial.print(F("ok: trim ")); Serial.println(trim);

  } else if (!strcmp(cmd, "stop")) {
    halt();
    Serial.println(F("ok: stop"));

  } else if (!strcmp(cmd, "status")) {
    Serial.print(F("L=M"));       Serial.print(portL);
    Serial.print(F(" R=M"));      Serial.print(portR);
    Serial.print(F(" mode="));    Serial.print((char)mode);
    Serial.print(F(" speed="));   Serial.print(speed);
    Serial.print(F(" trim="));    Serial.print(trim);
    Serial.print(F(" uptime_s=")); Serial.println(millis() / 1000);

  } else {
    Serial.println(F("err: unknown"));
  }
}
