// MPU-6050 with gyro bias calibration + complementary filter.
// Keep still for the first ~2 seconds after reset.

#include <Wire.h>

const uint8_t  MPU_ADDR = 0x68;
const uint8_t  REG_PWR  = 0x6B;
const uint8_t  REG_DATA = 0x3B;
const float    ACC_LSB  = 16384.0;
const float    GYR_LSB  = 131.0;

const uint16_t CAL_SAMPLES = 500;
const float    ALPHA       = 0.98;   // trust gyro short-term, accel long-term
const uint16_t REPORT_MS   = 100;

float gxBias = 0, gyBias = 0, gzBias = 0;
float pitch = 0, roll = 0;
uint32_t lastMicros = 0, lastReport = 0;

void readRaw(int16_t &ax, int16_t &ay, int16_t &az,
             int16_t &gx, int16_t &gy, int16_t &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_DATA);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)14, (uint8_t)true);
  ax = Wire.read() << 8 | Wire.read();
  ay = Wire.read() << 8 | Wire.read();
  az = Wire.read() << 8 | Wire.read();
  Wire.read(); Wire.read();               // discard temp
  gx = Wire.read() << 8 | Wire.read();
  gy = Wire.read() << 8 | Wire.read();
  gz = Wire.read() << 8 | Wire.read();
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }
  delay(50);

  Wire.begin();
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_PWR); Wire.write(0);
  Wire.endTransmission(true);

  Serial.println(F("calibrating - hold still"));
  int16_t ax, ay, az, gx, gy, gz;
  double sx = 0, sy = 0, sz = 0;
  for (uint16_t i = 0; i < CAL_SAMPLES; i++) {
    readRaw(ax, ay, az, gx, gy, gz);
    sx += gx; sy += gy; sz += gz;
    delay(3);
  }
  gxBias = sx / CAL_SAMPLES / GYR_LSB;
  gyBias = sy / CAL_SAMPLES / GYR_LSB;
  gzBias = sz / CAL_SAMPLES / GYR_LSB;

  Serial.print(F("bias dps: "));
  Serial.print(gxBias, 2); Serial.print(' ');
  Serial.print(gyBias, 2); Serial.print(' ');
  Serial.println(gzBias, 2);

  lastMicros = micros();
}

void loop() {
  int16_t ax, ay, az, gx, gy, gz;
  readRaw(ax, ay, az, gx, gy, gz);

  uint32_t now = micros();
  float dt = (now - lastMicros) / 1000000.0;
  lastMicros = now;

  float axg = ax / ACC_LSB, ayg = ay / ACC_LSB, azg = az / ACC_LSB;
  float accPitch = atan2(-axg, sqrt(ayg * ayg + azg * azg)) * 180.0 / PI;
  float accRoll  = atan2(ayg, azg) * 180.0 / PI;

  float gxd = gx / GYR_LSB - gxBias;
  float gyd = gy / GYR_LSB - gyBias;

  pitch = ALPHA * (pitch + gyd * dt) + (1 - ALPHA) * accPitch;
  roll  = ALPHA * (roll  + gxd * dt) + (1 - ALPHA) * accRoll;

  if (millis() - lastReport >= REPORT_MS) {
    lastReport = millis();
    Serial.print(F("pitch=")); Serial.print(pitch, 1);
    Serial.print(F(" roll="));  Serial.println(roll, 1);
  }
}
