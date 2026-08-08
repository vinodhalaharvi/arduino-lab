# arduino-lab

Bare-metal Arduino experiments, built and flashed from the command line
with `arduino-cli`. No IDE.

## Layout

Each numbered folder is a self-contained sketch.

| Sketch | What it does |
|---|---|
| `01-serial-heartbeat` | Blink + serial telemetry; validates the toolchain |

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
