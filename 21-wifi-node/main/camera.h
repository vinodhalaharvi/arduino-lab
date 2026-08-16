#pragma once

#include <stdbool.h>

/* Bring up the OV2640 once at boot: VGA JPEG, quality 12, two frame
   buffers in PSRAM. The sensor stays powered for the life of the app —
   this is a mains-powered indoor monitor, so there is no reason to
   pay reinit latency (~100 ms) on every enable. */
void camera_init(void);

/* Enable/disable serving frames over HTTP. The sensor is untouched;
   only the HTTP handlers gate on this flag. */
void camera_serving_set(bool on);
bool camera_serving_get(void);
