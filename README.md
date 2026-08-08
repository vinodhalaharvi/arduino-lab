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
