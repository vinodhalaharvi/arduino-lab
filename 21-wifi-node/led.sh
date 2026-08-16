#!/usr/bin/env bash
# led.sh — drive the Wi-Fi LED node over CoAP/UDP.
#
#   ADDR=192.168.1.42 ./led.sh colour 255 80 0
#   ADDR=192.168.1.42 ./led.sh bri 40
#   ADDR=192.168.1.42 ./led.sh fx rainbow
#   ADDR=192.168.1.42 ./led.sh speed 300
#   ADDR=192.168.1.42 ./led.sh status
#   ADDR=192.168.1.42 ./led.sh demo
#
# Same client, same API, same script as the Thread node — the transport
# is the only thing that changed. The XIAO's LED is single-colour, so
# "colour" is projected onto brightness via luma.

set -euo pipefail

ADDR="${ADDR:?set ADDR=<ipv4> or ADDR=<ipv6> for the target}"
if [[ "$ADDR" == *:* ]]; then HOST="[$ADDR]"; else HOST="$ADDR"; fi
BASE="coap://$HOST"

post() { coap-client -m post -e "$2" "$BASE/$1"; }
get()  { coap-client -m get "$BASE/$1"; }

case "${1:-status}" in
  colour|color)
    post led "${2:-0},${3:-0},${4:-0}" ;;
  bri|brightness)
    post bri "${2:-255}" ;;
  fx|effect)
    post fx "${2:-solid}" ;;
  speed)
    post speed "${2:-100}" ;;
  on)   post fx solid ;;
  off)  post fx off ;;
  status)
    printf 'colour : ' ; get led
    printf 'bright : ' ; get bri
    printf 'effect : ' ; get fx
    printf 'speed  : ' ; get speed
    ;;
  demo)
    echo "solid full"     ; post fx solid   >/dev/null
                            post led 255,255,255 >/dev/null ; sleep 2
    echo "dim to 20%"     ; post bri 51     >/dev/null ; sleep 2
    echo "back to full"   ; post bri 255    >/dev/null ; sleep 1
    echo "pulse"          ; post fx pulse   >/dev/null ; sleep 5
    echo "blink"          ; post fx blink   >/dev/null ; sleep 4
    echo "off"            ; post fx off     >/dev/null
    ;;
  *)
    echo "usage: ADDR=<ip> $0 {colour R G B|bri N|fx NAME|speed N|on|off|status|demo}" >&2
    echo "  fx: off solid rainbow pulse blink cycle" >&2
    exit 1 ;;
esac
