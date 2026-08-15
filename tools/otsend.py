#!/usr/bin/env python3
"""Drive an ESP console reliably.

The console's line editor probes the terminal with a Device Status Report
(ESC[6n) and drops input while waiting for the answer. A dumb writer never
answers, so the first characters of a command get swallowed. This script
watches for the probe and replies like a real terminal would.
"""
import sys, time, threading, serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
gap  = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0

s = serial.Serial(port, 115200, timeout=0.05)
stop = threading.Event()

def pump():
    """Echo device output, and answer cursor-position queries."""
    buf = b""
    while not stop.is_set():
        d = s.read(256)
        if not d:
            continue
        buf += d
        # ESC[6n -> reply ESC[24;1R  (any plausible row/col works)
        while b"\x1b[6n" in buf:
            buf = buf.replace(b"\x1b[6n", b"", 1)
            s.write(b"\x1b[24;1R")
            s.flush()
        sys.stdout.write(buf.decode(errors="replace"))
        sys.stdout.flush()
        buf = b""

t = threading.Thread(target=pump, daemon=True)
t.start()

time.sleep(0.5)

for line in sys.stdin:
    cmd = line.strip()
    if not cmd or cmd.startswith("#"):
        continue
    print(f"\n>>> {cmd}", flush=True)
    s.write((cmd + "\r\n").encode())
    s.flush()
    time.sleep(gap)

time.sleep(0.5)
stop.set()
t.join(timeout=1)
s.close()
