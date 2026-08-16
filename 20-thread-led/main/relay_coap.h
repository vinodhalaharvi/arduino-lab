#pragma once

/* Configure the relay GPIO and register the /relay CoAP resource.
   Call after led_coap_init(), which already started the CoAP server. */
void relay_coap_init(void);
