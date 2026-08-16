#pragma once

/* Start esp_http_server on tcp/80. Registers /, /snap, /stream against
   the camera module. Safe to call once Wi-Fi is up and camera_init()
   has run. */
void http_server_start(void);
