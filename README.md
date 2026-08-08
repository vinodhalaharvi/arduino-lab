# arduino-lab

Bare-metal Arduino experiments, built and flashed from the command line
with `arduino-cli`. No IDE.

## Layout

Each numbered folder is a self-contained sketch.

| Sketch | What it does |
|---|---|
| `01-serial-heartbeat` | Blink + serial telemetry; validates the toolchain |
| `02-serial-command` | Line-oriented command parser (reused everywhere after) |
| `03-i2c-scan` | Bus scan; identifies shields and sensors by address |
| `04-motor-basics` | Single DC motor, Adafruit Motor Shield v2 |
| `05-two-wheel` | Differential drive with port mapping and trim |
| `08-elegoo-motors` | Elegoo V4 TB6612FNG, two motor pairs |
| `09-mpu6050-raw` | Raw IMU registers over I2C, no library |
| `10-mpu6050-tilt` | Gyro bias calibration + complementary filter |
| `11-pin-probe` | Empirical pin discovery for unknown motor drivers |
| `02-serial-command` | Line-oriented command parser (reused everywhere after) |
| `03-i2c-scan` | Bus scan; identifies shields and sensors by address |
| `04-motor-basics` | Single DC motor, Adafruit Motor Shield v2 |
| `05-two-wheel` | Differential drive with port mapping and trim |
| `08-elegoo-motors` | Elegoo V4 TB6612FNG, two motor pairs |
| `09-mpu6050-raw` | Raw IMU registers over I2C, no library |
| `10-mpu6050-tilt` | Gyro bias calibration + complementary filter |
| `11-pin-probe` | Empirical pin discovery for unknown motor drivers |

## Build & flash

    ./flash.sh 01-serial-heartbeat

## Requirements

- `arduino-cli` (`brew install arduino-cli`)
- `arduino:avr` core

## Hardware

| Rig | Port | FQBN | Notes |
|---|---|---|---|
| Uno + Adafruit Motor Shield v2 | `usbmodem*` | `arduino:avr:uno` | PCA9685 @ 0x60, TB6612 H-bridges |
| Elegoo Smart Car V4 | `usbserial-*` | `arduino:avr:uno` | CH340; **unplug camera to upload** |
| GY-521 (MPU-6050) | I2C @ 0x68 | — | A4/A5, 6-axis IMU |
