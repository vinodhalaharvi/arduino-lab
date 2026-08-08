// Adafruit Motor Shield v2 — one DC motor, driven over serial.
// Commands: fwd <0-255> | rev <0-255> | stop | status

#include <Wire.h>
#include <Adafruit_MotorShield.h>

Adafruit_MotorShield AFMS = Adafruit_MotorShield();   // 0x60
Adafruit_DCMotor *m1 = AFMS.getMotor(1);              // M1 terminal block

char    buf[32];
uint8_t len   = 0;
uint8_t speed = 0;
char    dir   = 'S';

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }
  delay(50);

  if (!AFMS.begin()) {
    Serial.println(F("ERR: shield not found at 0x60"));
    while (1) { ; }
  }
  m1->setSpeed(0);
  m1->run(RELEASE);
  Serial.println(F("ready. fwd <0-255> | rev <0-255> | stop | status"));
}

void loop() { readSerial(); }

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { buf[len] = '\0'; if (len) handle(buf); len = 0; }
    else if (len < sizeof(buf) - 1) buf[len++] = c;
  }
}

void handle(const char *cmd) {
  if (!strncmp(cmd, "fwd ", 4) || !strncmp(cmd, "rev ", 4)) {
    long s = atol(cmd + 4);
    if (s < 0 || s > 255) { Serial.println(F("err: 0..255")); return; }
    speed = (uint8_t)s;
    dir   = (cmd[0] == 'f') ? 'F' : 'R';
    m1->setSpeed(speed);
    m1->run(dir == 'F' ? FORWARD : BACKWARD);
    Serial.print(F("ok: ")); Serial.print(dir); Serial.print(' '); Serial.println(speed);

  } else if (!strcmp(cmd, "stop")) {
    speed = 0; dir = 'S';
    m1->setSpeed(0);
    m1->run(RELEASE);
    Serial.println(F("ok: stop"));

  } else if (!strcmp(cmd, "status")) {
    Serial.print(F("dir=")); Serial.print(dir);
    Serial.print(F(" speed=")); Serial.print(speed);
    Serial.print(F(" uptime_s=")); Serial.println(millis() / 1000);

  } else {
    Serial.println(F("err: unknown"));
  }
}
