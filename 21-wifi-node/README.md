# 21-wifi-node — CoAP LED/relay node over Wi-Fi

Sibling of `20-thread-led`. Same CoAP API, different transport and different
chip. A client script driving `coap://<addr>/relay` cannot tell whether it's
talking to a C6 on Thread or an S3 on Wi-Fi.

## Hardware

- **Board:** Seeed XIAO ESP32S3 Sense (any XIAO ESP32S3 works; the
  Sense-specific peripherals — camera, mic, SD — are unused).
- **User LED:** GPIO21 (built-in yellow LED, active-low). Driven by LEDC PWM.
  It's a single-colour LED, so `/led r,g,b` is projected onto brightness via
  BT.601 luma. Effects that depend on hue (rainbow, cycle) degrade to
  brightness ripples; the API stays byte-identical to the Thread node.
- **Relay:** GPIO2 (header pin D1), Songle SRD-05VDC-SL-C. Active-low,
  same wiring convention as the C6 node. GPIO2 on ESP32-S3 is not a
  strapping pin.
- **Camera:** OV2640 on the Sense daughterboard. Fixed Seeed pinout;
  frame buffers live in the 8 MB octal PSRAM. Sensor is initialised
  once at boot and stays powered — `/cam on|off` only gates the HTTP
  handlers.

## CoAP surface (unchanged from `20-thread-led`)

| Method | Path    | Body                     | Response                     |
|--------|---------|--------------------------|------------------------------|
| GET    | /led    | —                        | `r,g,b` (2.05)               |
| POST   | /led    | `r,g,b`                  | echoes body (2.04) / 4.00    |
| GET    | /bri    | —                        | `0..255` (2.05)              |
| POST   | /bri    | `0..255`                 | echoes body (2.04) / 4.00    |
| GET    | /fx     | —                        | current name (2.05)          |
| POST   | /fx     | off/solid/rainbow/…      | echoes body (2.04) / 4.00    |
| GET    | /speed  | —                        | `10..1000` (2.05)            |
| POST   | /speed  | `10..1000`               | echoes body (2.04) / 4.00    |
| GET    | /relay  | —                        | `on`/`off` (2.05)            |
| POST   | /relay  | on/off/toggle/pulse `<ms>` | echoes state (2.04) / 4.00 |
| GET    | /cam    | —                        | `on http://<ip>/stream` or `off` (2.05) |
| POST   | /cam    | on/off                   | echoes state (2.04) / 4.00   |

## HTTP surface (cat monitor)

`esp_http_server` runs alongside CoAP on tcp/80. All three return 503
`camera off` when `/cam` is off.

| Method | Path      | Response                                    |
|--------|-----------|---------------------------------------------|
| GET    | /         | minimal HTML page with `<img src="/stream">` |
| GET    | /snap     | single JPEG, `Content-Type: image/jpeg`     |
| GET    | /stream   | MJPEG, `multipart/x-mixed-replace`          |

Concurrent stream clients are capped at 2 (see `STREAM_CLIENT_MAX` in
`http_server.c`); extras get 503.

## Wi-Fi credentials

`sdkconfig.defaults` ships with placeholder `REPLACE_ME_SSID` /
`REPLACE_ME_PASSWORD`. Real values go in a **local** override — either
`sdkconfig.defaults.local` (gitignored via the repo root, but confirm
before committing) or through `idf.py menuconfig` → "Example Connection
Configuration".

## Build

```
matterenv                       # source ESP-IDF v5.5.4
cd ~/embedded_projects/arduino_projects/arduino-lab/21-wifi-node
idf.py set-target esp32s3
idf.py build
```

## Flash and test

```
idf.py -p /dev/cu.usbmodem21201 flash monitor
# note the IP once "example_connect: - IPv4 address: 192.168.x.y" appears

ADDR=192.168.x.y ./relay.sh click
ADDR=192.168.x.y ./led.sh demo
```
