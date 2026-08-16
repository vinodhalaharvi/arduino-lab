/*
 * relay_coap.c — same relay CoAP surface as the Thread node.
 *
 *   GET  /relay              -> "on" | "off"
 *   POST /relay  "on"        -> energise
 *   POST /relay  "off"       -> release
 *   POST /relay  "toggle"    -> flip
 *   POST /relay  "pulse 500" -> on for 500 ms, then off
 *
 * XIAO ESP32S3 wiring, Songle SRD-05VDC-SL-C module:
 *   5V   -> VCC
 *   GND  -> GND
 *   D1 (GPIO2) -> IN
 *
 * Kept on GPIO2 to match the C6 node's convention — same screw-terminal
 * assignment across both boards. GPIO2 on the ESP32-S3 is not a
 * strapping pin, so it's safe to drive at boot.
 *
 * The module is active-low: driving IN low energises the coil. The
 * pin is configured with the level set BEFORE the mode, plus a
 * pull-up, so a floating pin through reset does not click the relay.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "coap3/coap.h"
#include "coap_server.h"

static const char *TAG = "relay";

#define RELAY_GPIO      GPIO_NUM_2
#define RELAY_ON_LEVEL  0
#define RELAY_OFF_LEVEL 1

#define PULSE_MIN_MS    50
#define PULSE_MAX_MS    30000

static bool s_on;
static TimerHandle_t s_pulse_timer;

/* --------------------------------------------------------------- relay -- */

static void relay_write(bool on)
{
    s_on = on;
    gpio_set_level(RELAY_GPIO, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
    ESP_LOGI(TAG, "relay %s", on ? "on" : "off");
}

static void pulse_expired(TimerHandle_t t)
{
    (void)t;
    relay_write(false);
}

static void relay_init_gpio(void)
{
    /* Level BEFORE mode: an ESP32 GPIO floats through reset, and on an
       active-low module a floating pin can read as low — which
       energises the coil before any of our code runs. Setting the
       level first, then adding the pull-up in gpio_config, holds the
       contact open through reset. */
    gpio_set_level(RELAY_GPIO, RELAY_OFF_LEVEL);

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    relay_write(false);
}

/* ---------------------------------------------------------------- CoAP -- */

static size_t body_string(const coap_pdu_t *req, char *buf, size_t cap)
{
    size_t   len = 0;
    const uint8_t *data = NULL;
    size_t   off = 0, total = 0;

    if (!coap_get_data_large(req, &len, &data, &off, &total) || data == NULL) {
        buf[0] = '\0';
        return 0;
    }
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';
    return len;
}

static void reply(coap_pdu_t *resp, coap_pdu_code_t code, const char *body)
{
    coap_pdu_set_code(resp, code);
    if (body != NULL && body[0] != '\0') {
        coap_add_data(resp, strlen(body), (const uint8_t *)body);
    }
}

static void hnd_relay(coap_resource_t *r, coap_session_t *s,
                      const coap_pdu_t *req, const coap_string_t *query,
                      coap_pdu_t *resp)
{
    (void)r; (void)s; (void)query;
    coap_pdu_code_t code = coap_pdu_get_code(req);

    if (code == COAP_REQUEST_CODE_GET) {
        reply(resp, COAP_RESPONSE_CODE_CONTENT, s_on ? "on" : "off");
        return;
    }

    char buf[32];
    body_string(req, buf, sizeof(buf));

    /* Any new command cancels a pulse in flight, so "pulse 5000"
       followed by "off" really means off. */
    xTimerStop(s_pulse_timer, 0);

    if (strcmp(buf, "on") == 0) {
        relay_write(true);
        reply(resp, COAP_RESPONSE_CODE_CHANGED, "on");

    } else if (strcmp(buf, "off") == 0) {
        relay_write(false);
        reply(resp, COAP_RESPONSE_CODE_CHANGED, "off");

    } else if (strcmp(buf, "toggle") == 0) {
        relay_write(!s_on);
        reply(resp, COAP_RESPONSE_CODE_CHANGED, s_on ? "on" : "off");

    } else if (strncmp(buf, "pulse", 5) == 0) {
        long ms = 500;
        if (buf[5] != '\0') {
            ms = strtol(buf + 5, NULL, 10);
        }
        if (ms < PULSE_MIN_MS || ms > PULSE_MAX_MS) {
            reply(resp, COAP_RESPONSE_CODE_BAD_REQUEST, "pulse 50-30000");
            return;
        }
        /* Device owns the timing: a dropped "off" cannot leave the
           contact closed. */
        xTimerChangePeriod(s_pulse_timer, pdMS_TO_TICKS(ms), 0);
        relay_write(true);
        xTimerStart(s_pulse_timer, 0);

        char out[32];
        snprintf(out, sizeof(out), "pulse %ld", ms);
        reply(resp, COAP_RESPONSE_CODE_CHANGED, out);

    } else {
        reply(resp, COAP_RESPONSE_CODE_BAD_REQUEST,
              "on|off|toggle|pulse <ms>");
    }
}

void relay_coap_register(coap_context_t *ctx)
{
    relay_init_gpio();

    s_pulse_timer = xTimerCreate("relay_pulse", pdMS_TO_TICKS(500),
                                 pdFALSE, NULL, pulse_expired);
    configASSERT(s_pulse_timer != NULL);

    coap_resource_t *r = coap_resource_init(coap_make_str_const("relay"), 0);
    coap_register_handler(r, COAP_REQUEST_GET,  hnd_relay);
    coap_register_handler(r, COAP_REQUEST_POST, hnd_relay);
    coap_register_handler(r, COAP_REQUEST_PUT,  hnd_relay);
    coap_add_resource(ctx, r);

    ESP_LOGI(TAG, "/relay ready on gpio %d", RELAY_GPIO);
}
