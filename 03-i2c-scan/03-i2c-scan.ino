#include <Wire.h>

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }
  delay(50);
  Wire.begin();
  Serial.println(F("scanning i2c 0x08..0x77"));

  uint8_t found = 0;
  for (uint8_t addr = 8; addr < 120; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  device at 0x"));
      if (addr < 16) Serial.print('0');
      Serial.println(addr, HEX);
      found++;
    }
  }
  Serial.print(F("done, "));
  Serial.print(found);
  Serial.println(F(" device(s)"));
}

void loop() {}
