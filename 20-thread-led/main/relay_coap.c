/*
 * relay_coap.c — a CoAP resource that drives a mechanical relay.
 *
 *   GET  /relay              -> "on" | "off"
 *   POST /relay  "on"        -> energise
 *   POST /relay  "off"       -> release
 *   POST /relay  "toggle"    -> flip
 *   POST /relay  "pulse 500" -> on for 500 ms, then off
 *
 * Wiring, Songle SRD-05VDC-SL-C module:
 *
 *   C6 5V    -> VCC
 *   C6 GND   -> GND
 *   C6 GPIO2 -> IN
 *
 * The module is active-low: driving IN low energises the coil. That is
 * inverted from intuition and it matters at boot — see below.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#include "openthread/coap.h"
#include "openthread/instance.h"
#include "openthread/message.h"

static const char *TAG = "relay";

#define RELAY_GPIO      GPIO_NUM_2

/* Most cheap relay modules energise on a LOW input. If yours clicks the
   wrong way round, flip these two and rebuild — nothing else changes. */
#define RELAY_ON_LEVEL  0
#define RELAY_OFF_LEVEL 1

#define PULSE_MIN_MS    50
#define PULSE_MAX_MS    30000

static bool s_on;
static TimerHandle_t s_pulse_timer;

/* --------------------------------------------------------------- relay --- */

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
    /* Set the level BEFORE configuring the pin as an output. An ESP32 GPIO
       floats through reset, and on an active-low module a floating pin can
       read as low — which energises the relay before any of our code runs.
       Harmless with nothing wired to the contacts; not harmless when the
       relay is switching something that matters. */
    gpio_set_level(RELAY_GPIO, RELAY_OFF_LEVEL);

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* holds it released at boot */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    relay_write(false);
}

/* ---------------------------------------------------------------- CoAP --- */

static void respond(otMessage *req, const otMessageInfo *info,
                    otCoapCode code, const char *body)
{
    otInstance *inst = esp_openthread_get_instance();
    otMessage  *resp = otCoapNewMessage(inst, NULL);
    if (resp == NULL) {
        ESP_LOGE(TAG, "out of messages");
        return;
    }

    otCoapMessageInitResponse(resp, req, OT_COAP_TYPE_ACKNOWLEDGMENT, code);

    if (body != NULL && body[0] != '\0') {
        otCoapMessageSetPayloadMarker(resp);
        if (otMessageAppend(resp, body, strlen(body)) != OT_ERROR_NONE) {
            otMessageFree(resp);
            return;
        }
    }

    if (otCoapSendResponse(inst, resp, info) != OT_ERROR_NONE) {
        otMessageFree(resp);
    }
}

static uint16_t payload(otMessage *msg, char *buf, size_t cap)
{
    uint16_t off = otMessageGetOffset(msg);
    uint16_t len = otMessageGetLength(msg) - off;
    if (len >= cap) {
        len = cap - 1;
    }
    otMessageRead(msg, off, buf, len);
    buf[len] = '\0';
    return len;
}

static void handle_relay(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx;
    otCoapCode code = otCoapMessageGetCode(msg);

    if (code == OT_COAP_CODE_GET) {
        respond(msg, info, OT_COAP_CODE_CONTENT, s_on ? "on" : "off");
        return;
    }
    if (code != OT_COAP_CODE_POST && code != OT_COAP_CODE_PUT) {
        respond(msg, info, OT_COAP_CODE_METHOD_NOT_ALLOWED, NULL);
        return;
    }

    char buf[32];
    payload(msg, buf, sizeof(buf));

    /* Any new command cancels a pulse in flight, so "pulse 5000" followed
       by "off" really means off rather than off-then-on-again. */
    xTimerStop(s_pulse_timer, 0);

    if (strcmp(buf, "on") == 0) {
        relay_write(true);
        respond(msg, info, OT_COAP_CODE_CHANGED, "on");

    } else if (strcmp(buf, "off") == 0) {
        relay_write(false);
        respond(msg, info, OT_COAP_CODE_CHANGED, "off");

    } else if (strcmp(buf, "toggle") == 0) {
        relay_write(!s_on);
        respond(msg, info, OT_COAP_CODE_CHANGED, s_on ? "on" : "off");

    } else if (strncmp(buf, "pulse", 5) == 0) {
        long ms = 500;
        if (buf[5] != '\0') {
            ms = strtol(buf + 5, NULL, 10);
        }
        if (ms < PULSE_MIN_MS || ms > PULSE_MAX_MS) {
            respond(msg, info, OT_COAP_CODE_BAD_REQUEST, "pulse 50-30000");
            return;
        }
        /* A momentary close, which is what a garage door or doorbell
           wants: the device owns the timing, so a lost "off" command
           cannot leave the contact closed indefinitely. */
        xTimerChangePeriod(s_pulse_timer, pdMS_TO_TICKS(ms), 0);
        relay_write(true);
        xTimerStart(s_pulse_timer, 0);

        char out[32];
        snprintf(out, sizeof(out), "pulse %ld", ms);
        respond(msg, info, OT_COAP_CODE_CHANGED, out);

    } else {
        respond(msg, info, OT_COAP_CODE_BAD_REQUEST,
                "on|off|toggle|pulse <ms>");
    }
}

static otCoapResource s_res_relay = { .mUriPath = "relay",
                                      .mHandler = handle_relay };

void relay_coap_init(void)
{
    relay_init_gpio();

    s_pulse_timer = xTimerCreate("relay_pulse", pdMS_TO_TICKS(500),
                                 pdFALSE, NULL, pulse_expired);
    configASSERT(s_pulse_timer != NULL);

    esp_openthread_lock_acquire(portMAX_DELAY);
    otCoapAddResource(esp_openthread_get_instance(), &s_res_relay);
    esp_openthread_lock_release();

    ESP_LOGI(TAG, "coap resource /relay ready on gpio %d", RELAY_GPIO);
}
