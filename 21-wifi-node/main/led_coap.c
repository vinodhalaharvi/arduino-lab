/*
 * led_coap.c — same CoAP surface as the Thread node, driving the
 * XIAO ESP32S3's single yellow user LED on GPIO21 via LEDC PWM.
 *
 *   GET  /led               -> "r,g,b"
 *   POST /led   "r,g,b"     -> stores the RGB triple
 *
 *   GET  /bri               -> "0..255"
 *   POST /bri   "0..255"    -> orthogonal brightness axis
 *
 *   GET  /fx                -> "off|solid|rainbow|pulse|blink|cycle"
 *   POST /fx    NAME        -> switch effect
 *
 *   GET  /speed             -> "10..1000"
 *   POST /speed N           -> percent of nominal animation rate
 *
 * The XIAO's LED is monochrome, so "colour" is lossy: the LED shows
 * the luminance Y = 0.30 R + 0.59 G + 0.11 B, scaled by brightness.
 * `/led 0,0,0` is dark, `/led 255,255,255` is bright, `/led 255,80,0`
 * is somewhere in between. The rainbow and cycle effects lose their
 * hue and become brightness ripples — the API stays byte-identical to
 * the C6 node so a client cannot tell which transport it hit.
 *
 * The LED is active-LOW: PWM 0% = fully on. That is inverted inside
 * pwm_write() so the rest of the code reads normally.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/ledc.h"
#include "esp_log.h"

#include "coap3/coap.h"
#include "coap_server.h"

static const char *TAG = "led_coap";

#define LED_GPIO           21          /* XIAO ESP32S3 built-in yellow LED */
#define LED_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define LED_LEDC_TIMER     LEDC_TIMER_0
#define LED_LEDC_CHANNEL   LEDC_CHANNEL_0
#define LED_LEDC_DUTY_RES  LEDC_TIMER_8_BIT       /* 0..255 */
#define LED_LEDC_FREQ_HZ   5000
#define LED_ACTIVE_LOW     1

#define FRAME_MS           20          /* 50 Hz — plenty for a single LED */

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

static SemaphoreHandle_t s_lock;

static fx_t     s_fx    = FX_SOLID;
static uint8_t  s_r     = 0, s_g = 0, s_b = 16;
static uint8_t  s_bri   = 255;
static uint16_t s_speed = 100;

/* ----------------------------------------------------------- LED driver -- */

static inline uint8_t scale(uint8_t v, uint8_t bri)
{
    return (uint8_t)(((uint16_t)v * bri) / 255);
}

/* ITU-R BT.601 luma, integer maths only. */
static uint8_t luma(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
}

static void pwm_write(uint8_t v)
{
    /* At 8-bit resolution the PWM range is 0..255, i.e. duty 255 gives
       255/256 = ~99.6% high — a faint on-time. On an active-low LED
       that's enough to leave the pin visibly lit at "off". Duty
       (1<<duty_res) is LEDC's "always high" sentinel and gives a
       clean full-off. Mirror for the always-low end of the range. */
    uint32_t duty;
    if (LED_ACTIVE_LOW) {
        duty = (v == 0) ? (1u << LED_LEDC_DUTY_RES) : (255u - v);
    } else {
        duty = (v == 255) ? (1u << LED_LEDC_DUTY_RES) : v;
    }
    ledc_set_duty(LED_LEDC_MODE, LED_LEDC_CHANNEL, duty);
    ledc_update_duty(LED_LEDC_MODE, LED_LEDC_CHANNEL);
}

static void pwm_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LED_LEDC_MODE,
        .timer_num       = LED_LEDC_TIMER,
        .duty_resolution = LED_LEDC_DUTY_RES,
        .freq_hz         = LED_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .gpio_num   = LED_GPIO,
        .speed_mode = LED_LEDC_MODE,
        .channel    = LED_LEDC_CHANNEL,
        .timer_sel  = LED_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        /* (1<<res) is "always high" — a clean off for active-low. */
        .duty       = LED_ACTIVE_LOW ? (1u << LED_LEDC_DUTY_RES) : 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

/* -------------------------------------------------------------- render --- */

/* Triangle 0..255..0 over `period` ticks — cheaper than a sine and
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

    uint32_t t = (tick * speed) / 100;
    uint8_t  base = luma(r, g, b);
    uint8_t  v;

    switch (fx) {
    case FX_OFF:
        v = 0;
        break;
    case FX_SOLID:
        v = base;
        break;
    case FX_RAINBOW:
        /* No hue on a single-colour LED — degrade to a slow bright pulse
           so the "effect changed" is at least visible. */
        v = triangle(t, 180);
        break;
    case FX_PULSE:
        v = scale(base, triangle(t, 100));
        break;
    case FX_BLINK:
        v = ((t / 25) % 2) ? 0 : base;
        break;
    case FX_CYCLE:
        /* Six equal "steps", each at full-luma to mark the beat. */
        v = ((t / 50) % 6 == 0) ? 0 : base;
        break;
    default:
        v = 0;
        break;
    }

    pwm_write(scale(v, bri));
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

/* ---------------------------------------------------------------- CoAP -- */

/* Copy the request body out as a NUL-terminated string. Returns the
   number of body bytes copied (excluding the terminator). */
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

/* ---- /led ---- */

static void hnd_led(coap_resource_t *r, coap_session_t *s,
                    const coap_pdu_t *req, const coap_string_t *query,
                    coap_pdu_t *resp)
{
    (void)r; (void)s; (void)query;
    coap_pdu_code_t code = coap_pdu_get_code(req);

    if (code == COAP_REQUEST_CODE_GET) {
        char out[32];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(out, sizeof(out), "%u,%u,%u", s_r, s_g, s_b);
        xSemaphoreGive(s_lock);
        reply(resp, COAP_RESPONSE_CODE_CONTENT, out);
        return;
    }

    char buf[32];
    body_string(req, buf, sizeof(buf));

    int r_, g_, b_;
    if (sscanf(buf, "%d,%d,%d", &r_, &g_, &b_) != 3) {
        reply(resp, COAP_RESPONSE_CODE_BAD_REQUEST, "want r,g,b");
        return;
    }
    if (r_ < 0 || r_ > 255 || g_ < 0 || g_ > 255 || b_ < 0 || b_ > 255) {
        reply(resp, COAP_RESPONSE_CODE_BAD_REQUEST, "range 0-255");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_r = r_; s_g = g_; s_b = b_;
    if (s_fx == FX_OFF) {
        s_fx = FX_SOLID;
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "colour %d,%d,%d (luma %u)", r_, g_, b_, luma(r_, g_, b_));
    reply(resp, COAP_RESPONSE_CODE_CHANGED, buf);
}

/* ---- /bri ---- */

static void hnd_bri(coap_resource_t *r, coap_session_t *s,
                    const coap_pdu_t *req, const coap_string_t *query,
                    coap_pdu_t *resp)
{
    (void)r; (void)s; (void)query;
    coap_pdu_code_t code = coap_pdu_get_code(req);

    if (code == COAP_REQUEST_CODE_GET) {
        char out[8];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(out, sizeof(out), "%u", s_bri);
        xSemaphoreGive(s_lock);
        reply(resp, COAP_RESPONSE_CODE_CONTENT, out);
        return;
    }

    char buf[16];
    body_string(req, buf, sizeof(buf));
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf || v < 0 || v > 255) {
        reply(resp, COAP_RESPONSE_CODE_BAD_REQUEST, "range 0-255");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_bri = (uint8_t)v;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "brightness %ld", v);
    reply(resp, COAP_RESPONSE_CODE_CHANGED, buf);
}

/* ---- /fx ---- */

static void hnd_fx(coap_resource_t *r, coap_session_t *s,
                   const coap_pdu_t *req, const coap_string_t *query,
                   coap_pdu_t *resp)
{
    (void)r; (void)s; (void)query;
    coap_pdu_code_t code = coap_pdu_get_code(req);

    if (code == COAP_REQUEST_CODE_GET) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const char *name = fx_names[s_fx];
        xSemaphoreGive(s_lock);
        reply(resp, COAP_RESPONSE_CODE_CONTENT, name);
        return;
    }

    char buf[24];
    body_string(req, buf, sizeof(buf));
    for (size_t i = 0; i < sizeof(fx_names) / sizeof(fx_names[0]); i++) {
        if (fx_names[i] != NULL && strcmp(buf, fx_names[i]) == 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_fx = (fx_t)i;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "fx %s", buf);
            reply(resp, COAP_RESPONSE_CODE_CHANGED, buf);
            return;
        }
    }
    reply(resp, COAP_RESPONSE_CODE_BAD_REQUEST,
          "off|solid|rainbow|pulse|blink|cycle");
}

/* ---- /speed ---- */

static void hnd_speed(coap_resource_t *r, coap_session_t *s,
                      const coap_pdu_t *req, const coap_string_t *query,
                      coap_pdu_t *resp)
{
    (void)r; (void)s; (void)query;
    coap_pdu_code_t code = coap_pdu_get_code(req);

    if (code == COAP_REQUEST_CODE_GET) {
        char out[8];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(out, sizeof(out), "%u", s_speed);
        xSemaphoreGive(s_lock);
        reply(resp, COAP_RESPONSE_CODE_CONTENT, out);
        return;
    }

    char buf[16];
    body_string(req, buf, sizeof(buf));
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf || v < 10 || v > 1000) {
        reply(resp, COAP_RESPONSE_CODE_BAD_REQUEST, "range 10-1000");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_speed = (uint16_t)v;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "speed %ld", v);
    reply(resp, COAP_RESPONSE_CODE_CHANGED, buf);
}

/* --------------------------------------------------------------- setup -- */

static void add_res(coap_context_t *ctx, const char *uri,
                    coap_method_handler_t h)
{
    coap_resource_t *r = coap_resource_init(coap_make_str_const(uri), 0);
    coap_register_handler(r, COAP_REQUEST_GET,  h);
    coap_register_handler(r, COAP_REQUEST_POST, h);
    coap_register_handler(r, COAP_REQUEST_PUT,  h);
    coap_add_resource(ctx, r);
}

void led_coap_register(coap_context_t *ctx)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    pwm_init();

    add_res(ctx, "led",   hnd_led);
    add_res(ctx, "bri",   hnd_bri);
    add_res(ctx, "fx",    hnd_fx);
    add_res(ctx, "speed", hnd_speed);

    xTaskCreate(anim_task, "led_anim", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "/led /bri /fx /speed ready on gpio %d (active-low pwm)",
             LED_GPIO);
}
