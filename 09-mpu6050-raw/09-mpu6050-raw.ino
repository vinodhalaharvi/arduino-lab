// MPU-6050 raw read over I2C. No library — 14 bytes straight from 0x3B.
// Accel in g, gyro in deg/s, temp in C.

#include <Wire.h>

const uint8_t MPU_ADDR    = 0x68;
const uint8_t REG_PWR_MGMT = 0x6B;   // bit6 = sleep, set 0 to wake
const uint8_t REG_ACCEL_XOUT = 0x3B; // start of the 14-byte burst
const uint8_t BURST_LEN   = 14;

// Default full-scale ranges: accel +/-2g, gyro +/-250 deg/s
const float ACCEL_LSB_PER_G   = 16384.0;
const float GYRO_LSB_PER_DPS  = 131.0;

const uint32_t BAUD       = 115200;
const uint16_t REPORT_MS  = 200;

int16_t ax, ay, az, tRaw, gx, gy, gz;
uint32_t lastReport = 0;

void setup() {
  Serial.begin(BAUD);
  while (!Serial) { ; }
  delay(50);

  Wire.begin();
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_PWR_MGMT);
  Wire.write(0);                     // wake it up
  if (Wire.endTransmission(true) != 0) {
    Serial.println(F("ERR: no ack from 0x68"));
    while (true) { ; }
  }
  Serial.println(F("mpu6050 awake"));
}

void loop() {
  if (millis() - lastReport < REPORT_MS) return;
  lastReport = millis();

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_ACCEL_XOUT);
  Wire.endTransmission(false);       // repeated start, keeps the bus
  Wire.requestFrom((uint8_t)MPU_ADDR, BURST_LEN, (uint8_t)true);

  ax   = Wire.read() << 8 | Wire.read();
  ay   = Wire.read() << 8 | Wire.read();
  az   = Wire.read() << 8 | Wire.read();
  tRaw = Wire.read() << 8 | Wire.read();
  gx   = Wire.read() << 8 | Wire.read();
  gy   = Wire.read() << 8 | Wire.read();
  gz   = Wire.read() << 8 | Wire.read();

  Serial.print(F("a="));
  Serial.print(ax / ACCEL_LSB_PER_G, 2); Serial.print(',');
  Serial.print(ay / ACCEL_LSB_PER_G, 2); Serial.print(',');
  Serial.print(az / ACCEL_LSB_PER_G, 2);

  Serial.print(F("  g="));
  Serial.print(gx / GYRO_LSB_PER_DPS, 1); Serial.print(',');
  Serial.print(gy / GYRO_LSB_PER_DPS, 1); Serial.print(',');
  Serial.print(gz / GYRO_LSB_PER_DPS, 1);

  Serial.print(F("  t="));
  Serial.println(tRaw / 340.0 + 36.53, 1);
}
