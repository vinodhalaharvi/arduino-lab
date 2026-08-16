#pragma once

#include "coap3/coap.h"

/* Bring up libcoap on UDP port 5683, register the /led /bri /fx /speed
   /relay /cam resources, and spawn the io task. Call once, after
   Wi-Fi is up. */
void coap_server_start(void);

/* Implemented by led_coap.c, relay_coap.c, cam_coap.c. Each registers
   its own resources against the shared context and starts whatever
   timers or tasks it needs. Called by coap_server_start(). */
void led_coap_register(coap_context_t *ctx);
void relay_coap_register(coap_context_t *ctx);
void cam_coap_register(coap_context_t *ctx);
