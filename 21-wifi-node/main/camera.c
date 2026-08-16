/*
 * camera.c — OV2640 on the XIAO ESP32S3 Sense.
 *
 * Pinout is Seeed's, taken from their Sense wiki page
 * (wiki.seeedstudio.com/xiao_esp32s3_getting_started/ → "camera usage")
 * and cross-checked against Seeed's Seeed_Arduino_MJPEGViewer
 * examples/camera_pins.h. Guessing this pinout wastes an afternoon, so
 * treat these values as the single source of truth and don't tweak
 * without confirming against Seeed's docs.
 *
 * PWDN and RESET are hard-wired on the Sense daughterboard, so both
 * are -1 in the config. XCLK generation borrows LEDC_TIMER_1 /
 * LEDC_CHANNEL_1 — LEDC_TIMER_0/CHANNEL_0 is owned by led_coap.c and
 * must stay free.
 *
 * Frame buffers live in PSRAM (CAMERA_FB_IN_PSRAM). Two of them, so
 * capture can DMA into one while the HTTP handler is still shipping
 * the other out over TCP.
 */

#include "camera.h"

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "camera";

/* XIAO ESP32S3 Sense — OV2640 pin map. */
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40
#define CAM_PIN_SIOC    39
#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

static bool s_serving;

void camera_init(void)
{
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram == 0) {
        /* Fatal in spirit but not in effect: log and bail so the CoAP
           side still comes up. camera_serving_get() stays false and
           the HTTP handlers will 503. */
        ESP_LOGE(TAG, "PSRAM not detected — enable CONFIG_SPIRAM");
        return;
    }
    ESP_LOGI(TAG, "PSRAM total: %u bytes", (unsigned)psram);

    camera_config_t cfg = {
        .pin_pwdn       = CAM_PIN_PWDN,
        .pin_reset      = CAM_PIN_RESET,
        .pin_xclk       = CAM_PIN_XCLK,
        .pin_sccb_sda   = CAM_PIN_SIOD,
        .pin_sccb_scl   = CAM_PIN_SIOC,
        .pin_d7         = CAM_PIN_D7,
        .pin_d6         = CAM_PIN_D6,
        .pin_d5         = CAM_PIN_D5,
        .pin_d4         = CAM_PIN_D4,
        .pin_d3         = CAM_PIN_D3,
        .pin_d2         = CAM_PIN_D2,
        .pin_d1         = CAM_PIN_D1,
        .pin_d0         = CAM_PIN_D0,
        .pin_vsync      = CAM_PIN_VSYNC,
        .pin_href       = CAM_PIN_HREF,
        .pin_pclk       = CAM_PIN_PCLK,

        .xclk_freq_hz   = 20 * 1000 * 1000,
        .ledc_timer     = LEDC_TIMER_1,
        .ledc_channel   = LEDC_CHANNEL_1,

        .pixel_format   = PIXFORMAT_JPEG,
        .frame_size     = FRAMESIZE_VGA,      /* 640x480 — start small */
        .jpeg_quality   = 12,                 /* 0=best, 63=worst */
        .fb_count       = 2,
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
        return;
    }
    ESP_LOGI(TAG, "OV2640 ready: VGA jpeg q%d, %d fb in psram",
             cfg.jpeg_quality, cfg.fb_count);
}

void camera_serving_set(bool on)
{
    s_serving = on;
}

bool camera_serving_get(void)
{
    return s_serving;
}
