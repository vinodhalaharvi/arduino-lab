#!/usr/bin/env bash
# Rejoin the Thread network after a reflash wipes NVS.
set -euo pipefail
PORT="${PORT:-/dev/cu.usbmodem21201}"
DATASET="${DATASET:-0e0800000000000100004a0300000b35060004001fffe00208f175832da54b22440708fd0664debf0253720510d6cb1a5fba74e5a6659056a70ca35ac9030f4f70656e5468726561642d3864346601028d4f0410f36ddeca1109752f9b148be3520554380c0402a0f7f8000300000f}

python3 ~/bin/otsend.py "$PORT" 3 <<EOF
ot dataset set active $DATASET
ot ifconfig up
ot thread start
ot mode rdn
EOF

sleep 12
python3 ~/bin/otsend.py "$PORT" 2 <<'EOF'
ot state
ot ipaddr
EOF
