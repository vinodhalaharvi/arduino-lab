#!/usr/bin/env bash
# led.sh — drive the Thread LED node.
#
#   ./led.sh colour 255 80 0
#   ./led.sh bri 40
#   ./led.sh fx rainbow
#   ./led.sh speed 300
#   ./led.sh status
#   ./led.sh demo
#
# Override the target with:  ADDR=fdc0:... ./led.sh fx pulse

set -euo pipefail

ADDR="${ADDR:-fdc0:eabe:bc2d:1:471e:f310:befc:b794}"
BASE="coap://[$ADDR]"

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
    # Each step is one packet. The device does the 50 Hz work locally —
    # sending frames over a ~300 ms link would be a slideshow.
    echo "solid red"      ; post fx solid   >/dev/null
                            post led 255,0,0 >/dev/null ; sleep 2
    echo "dim to 20%"     ; post bri 51     >/dev/null ; sleep 2
    echo "back to full"   ; post bri 255    >/dev/null ; sleep 1
    echo "pulse"          ; post fx pulse   >/dev/null ; sleep 5
    echo "rainbow"        ; post fx rainbow >/dev/null ; sleep 5
    echo "rainbow, fast"  ; post speed 400  >/dev/null ; sleep 4
    echo "rainbow, dim"   ; post bri 40     >/dev/null ; sleep 4
    echo "cycle"          ; post speed 100  >/dev/null
                            post bri 255    >/dev/null
                            post fx cycle   >/dev/null ; sleep 6
    echo "blink"          ; post fx blink   >/dev/null
                            post led 0,255,120 >/dev/null ; sleep 4
    echo "off"            ; post fx off     >/dev/null
    ;;

  *)
    echo "usage: $0 {colour R G B|bri N|fx NAME|speed N|on|off|status|demo}" >&2
    echo "  fx: off solid rainbow pulse blink cycle" >&2
    exit 1 ;;
esac
