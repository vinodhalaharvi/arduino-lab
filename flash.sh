#!/usr/bin/env bash
set -euo pipefail

SKETCH="${1:?usage: ./flash.sh <sketch-dir> [--monitor]}"
FQBN="${FQBN:-arduino:avr:uno}"
PORT="${PORT:-$(arduino-cli board list | awk '/usbmodem|usbserial/{print $1; exit}')}"

[ -n "$PORT" ] || { echo "no board found"; exit 1; }
echo "==> $SKETCH -> $PORT ($FQBN)"

arduino-cli compile --fqbn "$FQBN" "$SKETCH"
arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$SKETCH"

if [ "${2:-}" = "--monitor" ]; then
  arduino-cli monitor -p "$PORT" -c baud=115200
fi
