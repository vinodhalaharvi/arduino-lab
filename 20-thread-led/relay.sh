#!/usr/bin/env bash
# relay.sh — drive the Thread relay node.
#
#   ./relay.sh on
#   ./relay.sh off
#   ./relay.sh toggle
#   ./relay.sh pulse 1000
#   ./relay.sh status
#   ./relay.sh click       # audible test: five slow clicks

set -euo pipefail

ADDR="${ADDR:-fdc0:eabe:bc2d:1:471e:f310:befc:b794}"
BASE="coap://[$ADDR]"

post() { coap-client -m post -e "$1" "$BASE/relay"; }

case "${1:-status}" in
  on|off|toggle) post "$1" ;;
  pulse)         post "pulse ${2:-500}" ;;
  status)        coap-client -m get "$BASE/relay" ;;
  click)
    # No load needed — the coil is audible, and the module LED follows it.
    for i in 1 2 3 4 5; do
      echo "click $i"
      post on  >/dev/null; sleep 1
      post off >/dev/null; sleep 1
    done
    ;;
  *)
    echo "usage: $0 {on|off|toggle|pulse <ms>|status|click}" >&2
    exit 1 ;;
esac
