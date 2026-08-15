/*
 * led_coap.c — CoAP-controlled WS2812 with animations and an orthogonal
 * brightness axis.
 *
 *   GET  /led                    -> "255,80,0"        current colour
 *   POST /led   "255,80,0"       -> set colour
 *
 *   GET  /fx                     -> "rainbow"         current effect
 *   POST /fx    "rainbow"        -> off|solid|rainbow|pulse|blink|cycle
 *
 *   GET  /bri                    -> "128"             current brightness
 *   POST /bri   "128"            -> 0-255, scales whatever is showing
 *
 * Brightness is deliberately a separate axis. Hue and effect decide what
 * colour to show; brightness scales it on the way out. That means dimming
 * does not lose the colour, and a rainbow at 10% is still a rainbow.
 *
 * Animation runs in a task on the device at ~50 Hz. Over a mesh with
 * ~300 ms round trips you could never drive that from the host — so the
 * host sends intent ("rainbow"), not frames.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "ot_led_strip.h"

#include "openthread/coap.h"
#include "openthread/instance.h"
#include "openthread/message.h"

static const char *TAG = "led_coap";

#define FRAME_MS   20     /* 50 Hz — smooth without hammering the RMT */

/* --------------------------------------------------------------- state --- */

typedef enum {
    FX_OFF,
    FX_SOLID,
    FX_RAINBOW,
    FX_PULSE,
    FX_BLINK,
    FX_CYCLE,
} fx_t;

static const char *fx_names[] = {
    [FX_OFF]     = "off",
    [FX_SOLID]   = "solid",
    [FX_RAINBOW] = "rainbow",
    [FX_PULSE]   = "pulse",
    [FX_BLINK]   = "blink",
    [FX_CYCLE]   = "cycle",
};

/* Shared between the CoAP handlers and the animation task, so guarded.
   Without the mutex you can read a half-written colour and get a flash
   of the wrong hue — rare, harmless here, but the habit matters once a
   handler is switching a relay rather than an LED. */
static SemaphoreHandle_t s_lock;

static fx_t    s_fx   = FX_SOLID;
static uint8_t s_r    = 0,  s_g = 0, s_b = 16;   /* base colour */
static uint8_t s_bri  = 255;                     /* brightness, orthogonal */
static uint16_t s_speed = 100;                   /* percent, 10..1000 */

/* ---------------------------------------------------------------- util --- */

static inline uint8_t scale(uint8_t v, uint8_t bri)
{
    return (uint8_t)(((uint16_t)v * bri) / 255);
}

/* HSV with 0..359 hue, 0..255 sat and val. Integer maths only — no FPU
   worth waking for this, and it keeps the animation task cheap. */
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360;
    uint8_t region    = h / 60;
    uint8_t remainder = (h % 60) * 255 / 60;

    uint8_t p = (v * (255 - s)) / 255;
    uint8_t q = (v * (255 - ((s * remainder) / 255))) / 255;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) / 255))) / 255;

    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

/* A triangle wave 0..255..0 over `period` ticks. Cheaper than a sine and
   visually close enough for a pulse. */
static uint8_t triangle(uint32_t tick, uint32_t period)
{
    uint32_t phase = tick % period;
    uint32_t half  = period / 2;
    if (phase < half) {
        return (uint8_t)((phase * 255) / half);
    }
    return (uint8_t)(((period - phase) * 255) / half);
}

/* -------------------------------------------------------------- render --- */

static void render(uint32_t tick)
{
    fx_t     fx;
    uint8_t  r, g, b, bri;
    uint16_t speed;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    fx    = s_fx;
    r     = s_r;
    g     = s_g;
    b     = s_b;
    bri   = s_bri;
    speed = s_speed;
    xSemaphoreGive(s_lock);

    /* Scale the tick by speed so one knob controls every effect. */
    uint32_t t = (tick * speed) / 100;

    switch (fx) {
    case FX_OFF:
        r = g = b = 0;
        break;

    case FX_SOLID:
        break;                       /* r,g,b as set */

    case FX_RAINBOW:
        hsv_to_rgb((t * 2) % 360, 255, 255, &r, &g, &b);
        break;

    case FX_PULSE: {
        uint8_t v = triangle(t, 100);          /* ~2 s at speed 100 */
        r = scale(r, v);
        g = scale(g, v);
        b = scale(b, v);
        break;
    }

    case FX_BLINK:
        if ((t / 25) % 2) {                    /* ~500 ms on, 500 off */
            r = g = b = 0;
        }
        break;

    case FX_CYCLE: {
        /* Step through six primaries rather than sweeping — reads as
           deliberate rather than decorative. */
        static const uint16_t hues[] = {0, 60, 120, 180, 240, 300};
        hsv_to_rgb(hues[(t / 50) % 6], 255, 255, &r, &g, &b);
        break;
    }
    }

    /* Brightness applies last, to whatever the effect produced. That is
       what makes it orthogonal: dimming never changes the hue. */
    esp_openthread_state_indicator_set(0,
                                       scale(r, bri),
                                       scale(g, bri),
                                       scale(b, bri));
}

static void anim_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        render(tick++);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(FRAME_MS));
    }
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

/* Copy the payload out as a NUL-terminated string. */
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

/* ---- /led : base colour ---- */

static void handle_led(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx;
    otCoapCode code = otCoapMessageGetCode(msg);

    if (code == OT_COAP_CODE_GET) {
        char out[32];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(out, sizeof(out), "%u,%u,%u", s_r, s_g, s_b);
        xSemaphoreGive(s_lock);
        respond(msg, info, OT_COAP_CODE_CONTENT, out);
        return;
    }
    if (code != OT_COAP_CODE_POST && code != OT_COAP_CODE_PUT) {
        respond(msg, info, OT_COAP_CODE_METHOD_NOT_ALLOWED, NULL);
        return;
    }

    char buf[32];
    payload(msg, buf, sizeof(buf));

    int r, g, b;
    if (sscanf(buf, "%d,%d,%d", &r, &g, &b) != 3) {
        respond(msg, info, OT_COAP_CODE_BAD_REQUEST, "want r,g,b");
        return;
    }
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        respond(msg, info, OT_COAP_CODE_BAD_REQUEST, "range 0-255");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_r = r; s_g = g; s_b = b;
    /* Setting a colour while off implies you want to see it. */
    if (s_fx == FX_OFF) {
        s_fx = FX_SOLID;
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "colour %d,%d,%d", r, g, b);
    respond(msg, info, OT_COAP_CODE_CHANGED, buf);
}

/* ---- /bri : brightness, orthogonal to colour and effect ---- */

static void handle_bri(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx;
    otCoapCode code = otCoapMessageGetCode(msg);

    if (code == OT_COAP_CODE_GET) {
        char out[8];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(out, sizeof(out), "%u", s_bri);
        xSemaphoreGive(s_lock);
        respond(msg, info, OT_COAP_CODE_CONTENT, out);
        return;
    }
    if (code != OT_COAP_CODE_POST && code != OT_COAP_CODE_PUT) {
        respond(msg, info, OT_COAP_CODE_METHOD_NOT_ALLOWED, NULL);
        return;
    }

    char buf[16];
    payload(msg, buf, sizeof(buf));

    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf || v < 0 || v > 255) {
        respond(msg, info, OT_COAP_CODE_BAD_REQUEST, "range 0-255");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_bri = (uint8_t)v;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "brightness %ld", v);
    respond(msg, info, OT_COAP_CODE_CHANGED, buf);
}

/* ---- /fx : which animation ---- */

static void handle_fx(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx;
    otCoapCode code = otCoapMessageGetCode(msg);

    if (code == OT_COAP_CODE_GET) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const char *name = fx_names[s_fx];
        xSemaphoreGive(s_lock);
        respond(msg, info, OT_COAP_CODE_CONTENT, name);
        return;
    }
    if (code != OT_COAP_CODE_POST && code != OT_COAP_CODE_PUT) {
        respond(msg, info, OT_COAP_CODE_METHOD_NOT_ALLOWED, NULL);
        return;
    }

    char buf[24];
    payload(msg, buf, sizeof(buf));

    for (size_t i = 0; i < sizeof(fx_names) / sizeof(fx_names[0]); i++) {
        if (fx_names[i] != NULL && strcmp(buf, fx_names[i]) == 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_fx = (fx_t)i;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "fx %s", buf);
            respond(msg, info, OT_COAP_CODE_CHANGED, buf);
            return;
        }
    }

    respond(msg, info, OT_COAP_CODE_BAD_REQUEST,
            "off|solid|rainbow|pulse|blink|cycle");
}

/* ---- /speed : animation rate, percent ---- */

static void handle_speed(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx;
    otCoapCode code = otCoapMessageGetCode(msg);

    if (code == OT_COAP_CODE_GET) {
        char out[8];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(out, sizeof(out), "%u", s_speed);
        xSemaphoreGive(s_lock);
        respond(msg, info, OT_COAP_CODE_CONTENT, out);
        return;
    }
    if (code != OT_COAP_CODE_POST && code != OT_COAP_CODE_PUT) {
        respond(msg, info, OT_COAP_CODE_METHOD_NOT_ALLOWED, NULL);
        return;
    }

    char buf[16];
    payload(msg, buf, sizeof(buf));

    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf || v < 10 || v > 1000) {
        respond(msg, info, OT_COAP_CODE_BAD_REQUEST, "range 10-1000");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_speed = (uint16_t)v;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "speed %ld", v);
    respond(msg, info, OT_COAP_CODE_CHANGED, buf);
}

/* --------------------------------------------------------------- setup --- */

static otCoapResource s_res_led   = { .mUriPath = "led",   .mHandler = handle_led   };
static otCoapResource s_res_bri   = { .mUriPath = "bri",   .mHandler = handle_bri   };
static otCoapResource s_res_fx    = { .mUriPath = "fx",    .mHandler = handle_fx    };
static otCoapResource s_res_speed = { .mUriPath = "speed", .mHandler = handle_speed };

void led_coap_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *inst = esp_openthread_get_instance();
    otCoapStart(inst, OT_DEFAULT_COAP_PORT);
    otCoapAddResource(inst, &s_res_led);
    otCoapAddResource(inst, &s_res_bri);
    otCoapAddResource(inst, &s_res_fx);
    otCoapAddResource(inst, &s_res_speed);
    esp_openthread_lock_release();

    xTaskCreate(anim_task, "led_anim", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "coap resources /led /bri /fx /speed ready on port %d",
             OT_DEFAULT_COAP_PORT);
}
