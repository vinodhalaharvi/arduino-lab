// Heartbeat: non-blocking blink + uptime telemetry over serial.
// No delay() anywhere — the loop stays free for future work.

const uint8_t  LED       = LED_BUILTIN;
const uint16_t BLINK_MS  = 500;
const uint16_t REPORT_MS = 1000;

uint32_t lastBlink  = 0;
uint32_t lastReport = 0;
bool     ledOn      = false;

void setup() {
  pinMode(LED, OUTPUT);
  Serial.begin(115200);
  while (!Serial) { ; }          // needed on native-USB boards
  Serial.println(F("heartbeat online"));
}

void loop() {
  const uint32_t now = millis();

  if (now - lastBlink >= BLINK_MS) {
    lastBlink = now;
    ledOn = !ledOn;
    digitalWrite(LED, ledOn);
  }

  if (now - lastReport >= REPORT_MS) {
    lastReport = now;
    Serial.print(F("uptime_s="));
    Serial.print(now / 1000);
    Serial.print(F(" free_ram="));
    Serial.println(freeRam());
  }
}

int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

