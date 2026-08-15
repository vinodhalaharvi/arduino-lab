#pragma once

/* Start the CoAP server and register the /led resource.
   Call after the OpenThread stack and CLI are up. */
void led_coap_init(void);
