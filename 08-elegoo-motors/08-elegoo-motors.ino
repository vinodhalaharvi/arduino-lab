// Elegoo Smart Robot Car V4 — TB6612FNG motor test.
// Motors wired as two pairs: A = right side, B = left side.
// Commands: fwd <0-255> | rev <0-255> | left <s> | right <s> | stop

const uint8_t PIN_PWMA = 5;    // right pair speed
const uint8_t PIN_PWMB = 6;    // left pair speed
const uint8_t PIN_AIN  = 7;    // right pair direction
const uint8_t PIN_BIN  = 8;    // left pair direction
const uint8_t PIN_STBY = 3;    // LOW = driver asleep

const uint8_t DIR_FWD    = HIGH;
const uint8_t DIR_REV    = LOW;
const uint8_t SPEED_STOP = 0;
const uint8_t SPEED_MAX  = 255;

const uint32_t BAUD = 115200;
const uint8_t  BUF  = 32;

char    buf[BUF];
uint8_t len = 0;

void setup() {
  Serial.begin(BAUD);
  while (!Serial) { ; }
  delay(50);

  pinMode(PIN_PWMA, OUTPUT);
  pinMode(PIN_PWMB, OUTPUT);
  pinMode(PIN_AIN,  OUTPUT);
  pinMode(PIN_BIN,  OUTPUT);
  pinMode(PIN_STBY, OUTPUT);

  digitalWrite(PIN_STBY, HIGH);    // wake the TB6612
  halt();
  Serial.println(F("ready. fwd|rev|left|right <0-255> | stop"));
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { buf[len] = '\0'; if (len) handle(buf); len = 0; }
    else if (len < BUF - 1) buf[len++] = c;
  }
}

void drive(uint8_t sR, uint8_t dR, uint8_t sL, uint8_t dL) {
  digitalWrite(PIN_AIN, dR); analogWrite(PIN_PWMA, sR);
  digitalWrite(PIN_BIN, dL); analogWrite(PIN_PWMB, sL);
}

void halt() { drive(SPEED_STOP, DIR_FWD, SPEED_STOP, DIR_FWD); }

const char *argAfter(const char *cmd, const char *pre) {
  size_t n = strlen(pre);
  return strncmp(cmd, pre, n) == 0 ? cmd + n : nullptr;
}

void handle(const char *cmd) {
  const char *arg;
  long s;

  if (!strcmp(cmd, "stop")) { halt(); Serial.println(F("ok: stop")); return; }

  if      ((arg = argAfter(cmd, "fwd ")))   s = atol(arg);
  else if ((arg = argAfter(cmd, "rev ")))   s = atol(arg);
  else if ((arg = argAfter(cmd, "left ")))  s = atol(arg);
  else if ((arg = argAfter(cmd, "right "))) s = atol(arg);
  else { Serial.println(F("err: unknown")); return; }

  if (s < SPEED_STOP || s > SPEED_MAX) { Serial.println(F("err: 0..255")); return; }
  uint8_t sp = (uint8_t)s;

  if      (cmd[0] == 'f') drive(sp, DIR_FWD, sp, DIR_FWD);
  else if (cmd[0] == 'r' && cmd[1] == 'e') drive(sp, DIR_REV, sp, DIR_REV);
  else if (cmd[0] == 'l') drive(sp, DIR_FWD, sp, DIR_REV);
  else                    drive(sp, DIR_REV, sp, DIR_FWD);

  Serial.print(F("ok: ")); Serial.print(cmd[0]); Serial.print(' '); Serial.println(sp);
}
