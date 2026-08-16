#!/usr/bin/env bash
# relay.sh — drive the Wi-Fi relay node over CoAP/UDP.
#
#   ADDR=192.168.1.42 ./relay.sh on
#   ADDR=192.168.1.42 ./relay.sh off
#   ADDR=192.168.1.42 ./relay.sh toggle
#   ADDR=192.168.1.42 ./relay.sh pulse 1000
#   ADDR=192.168.1.42 ./relay.sh status
#   ADDR=192.168.1.42 ./relay.sh click       # five audible clicks

set -euo pipefail

ADDR="${ADDR:?set ADDR=<ipv4> or ADDR=<ipv6> for the target}"
if [[ "$ADDR" == *:* ]]; then HOST="[$ADDR]"; else HOST="$ADDR"; fi
BASE="coap://$HOST"

post() { coap-client -m post -e "$1" "$BASE/relay"; }

case "${1:-status}" in
  on|off|toggle) post "$1" ;;
  pulse)         post "pulse ${2:-500}" ;;
  status)        coap-client -m get "$BASE/relay" ;;
  click)
    for i in 1 2 3 4 5; do
      echo "click $i"
      post on  >/dev/null; sleep 1
      post off >/dev/null; sleep 1
    done
    ;;
  *)
    echo "usage: ADDR=<ip> $0 {on|off|toggle|pulse <ms>|status|click}" >&2
    exit 1 ;;
esac
