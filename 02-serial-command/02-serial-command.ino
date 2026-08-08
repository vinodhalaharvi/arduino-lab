// Line-oriented command parser over serial.
// Commands:  on | off | blink <ms> | status

const uint8_t LED = LED_BUILTIN;

char     buf[32];
uint8_t  len      = 0;
uint16_t blinkMs  = 0;        // 0 = not blinking
uint32_t lastTick = 0;
bool     ledOn    = false;

void setup() {
  pinMode(LED, OUTPUT);
  Serial.begin(115200);
  while (!Serial) { ; }
  delay(50);
  Serial.println(F("ready. commands: on | off | blink <ms> | status"));
}

void loop() {
  readSerial();

  if (blinkMs && millis() - lastTick >= blinkMs) {
    lastTick = millis();
    ledOn = !ledOn;
    digitalWrite(LED, ledOn);
  }
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = '\0';
      if (len) handle(buf);
      len = 0;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

void handle(const char *cmd) {
  if (!strcmp(cmd, "on")) {
    blinkMs = 0; ledOn = true; digitalWrite(LED, HIGH);
    Serial.println(F("ok: on"));

  } else if (!strcmp(cmd, "off")) {
    blinkMs = 0; ledOn = false; digitalWrite(LED, LOW);
    Serial.println(F("ok: off"));

  } else if (!strncmp(cmd, "blink ", 6)) {
    long ms = atol(cmd + 6);
    if (ms < 10 || ms > 5000) {
      Serial.println(F("err: 10..5000"));
    } else {
      blinkMs = (uint16_t)ms;
      Serial.print(F("ok: blink ")); Serial.println(blinkMs);
    }

  } else if (!strcmp(cmd, "status")) {
    Serial.print(F("led=")); Serial.print(ledOn ? F("on") : F("off"));
    Serial.print(F(" blink_ms=")); Serial.print(blinkMs);
    Serial.print(F(" uptime_s=")); Serial.println(millis() / 1000);

  } else {
    Serial.print(F("err: unknown '")); Serial.print(cmd); Serial.println('\'');
  }
}
