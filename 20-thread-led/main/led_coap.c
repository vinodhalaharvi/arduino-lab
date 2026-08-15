/*
 * led_coap.c — a CoAP resource that drives the on-board WS2812.
 *
 *   coap-client -m get  "coap://[ADDR]/led"
 *   coap-client -m post -e "255,80,0" "coap://[ADDR]/led"
 *
 * Payload is three comma-separated integers, 0-255. Deliberately not JSON
 * for iteration 1: sscanf parses it in one line, and coap-client sends it
 * without shell quoting pain.
 *
 * The LED itself is driven through ot_led's state indicator, which the
 * ot_cli example already initialises. Setting up a second RMT device on
 * the same GPIO would fight it.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "ot_led_strip.h"

#include "openthread/coap.h"
#include "openthread/instance.h"
#include "openthread/message.h"

static const char *TAG = "led_coap";

static uint8_t s_r, s_g, s_b;

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    s_r = r; s_g = g; s_b = b;
    esp_openthread_state_indicator_set(0, r, g, b);
    ESP_LOGI(TAG, "led %u,%u,%u", r, g, b);
}

/* Send a response carrying an optional text body. */
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

static void handle_led(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx;

    otCoapCode code = otCoapMessageGetCode(msg);

    /* GET: report the current colour. */
    if (code == OT_COAP_CODE_GET) {
        char out[32];
        snprintf(out, sizeof(out), "%u,%u,%u", s_r, s_g, s_b);
        respond(msg, info, OT_COAP_CODE_CONTENT, out);
        return;
    }

    if (code != OT_COAP_CODE_POST && code != OT_COAP_CODE_PUT) {
        respond(msg, info, OT_COAP_CODE_METHOD_NOT_ALLOWED, NULL);
        return;
    }

    /* POST/PUT: read "r,g,b" from the payload. */
    char buf[32] = {0};
    uint16_t off = otMessageGetOffset(msg);
    uint16_t len = otMessageGetLength(msg) - off;
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    otMessageRead(msg, off, buf, len);
    buf[len] = '\0';

    int r, g, b;
    if (sscanf(buf, "%d,%d,%d", &r, &g, &b) != 3) {
        respond(msg, info, OT_COAP_CODE_BAD_REQUEST, "want r,g,b");
        return;
    }
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        respond(msg, info, OT_COAP_CODE_BAD_REQUEST, "range 0-255");
        return;
    }

    led_set((uint8_t)r, (uint8_t)g, (uint8_t)b);
    respond(msg, info, OT_COAP_CODE_CHANGED, buf);
}

static otCoapResource s_led_resource = {
    .mUriPath = "led",
    .mHandler = handle_led,
    .mContext = NULL,
    .mNext    = NULL,
};

void led_coap_init(void)
{
    /* The OpenThread stack is not thread-safe; take its lock before
       touching the instance from a task other than the OT mainloop. */
    esp_openthread_lock_acquire(portMAX_DELAY);

    otInstance *inst = esp_openthread_get_instance();
    otCoapStart(inst, OT_DEFAULT_COAP_PORT);
    otCoapAddResource(inst, &s_led_resource);

    esp_openthread_lock_release();

    led_set(0, 0, 16);   /* dim blue: booted, not yet commanded */

    ESP_LOGI(TAG, "coap resource /led ready on port %d", OT_DEFAULT_COAP_PORT);
}
