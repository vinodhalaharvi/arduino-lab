/*
 * http_server.c — esp_http_server on tcp/80, serving:
 *
 *   GET /         -> tiny HTML that <img src="/stream">
 *   GET /snap     -> single JPEG (Content-Type: image/jpeg)
 *   GET /stream   -> MJPEG (multipart/x-mixed-replace)
 *
 * When camera_serving_get() is false, all three return 503 with a short
 * plain-text body. Explicit 503 beats hanging or returning an empty
 * 200 — the browser tab actually says something instead of spinning.
 *
 * Concurrency notes:
 *
 *  - esp_http_server runs ONE task that services all sockets. While a
 *    /stream handler is looping, /snap and / requests queue behind it.
 *    Acceptable for a single-viewer cat monitor. If two independent
 *    concurrent viewers ever matter, split into two httpd instances
 *    on different ports.
 *  - Stream slots are capped by a counting semaphore. Extras get 503
 *    rather than degrading the primary viewer's framerate.
 *  - esp_camera_fb_return() must run on every path or the sensor
 *    stalls after ~fb_count frames. Every error path below returns
 *    the buffer.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "camera.h"
#include "http_server.h"

static const char *TAG = "http";

/* One primary viewer plus one spare (phone + laptop). More than this
   is capture-bound and just makes everyone's stream janky. */
#define STREAM_CLIENT_MAX 2

static SemaphoreHandle_t s_stream_slots;

#define BOUNDARY     "esp32camboundary"
#define STREAM_CT    "multipart/x-mixed-replace;boundary=" BOUNDARY
#define PART_HEADER  "\r\n--" BOUNDARY "\r\n" \
                     "Content-Type: image/jpeg\r\n" \
                     "Content-Length: %u\r\n\r\n"

static const char INDEX_HTML[] =
    "<!doctype html><meta charset=utf-8>"
    "<title>cat monitor</title>"
    "<body style='margin:0;background:#111;color:#eee;font:14px system-ui'>"
    "<img src='/stream' style='display:block;max-width:100%;height:auto;margin:auto'>"
    "</body>";

/* Returns true (and sends the response) if serving is off. Callers
   return ESP_OK immediately after. */
static bool disabled_reply(httpd_req_t *req)
{
    if (camera_serving_get()) {
        return false;
    }
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "camera off\n");
    return true;
}

static esp_err_t hnd_index(httpd_req_t *req)
{
    if (disabled_reply(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t hnd_snap(httpd_req_t *req)
{
    if (disabled_reply(req)) {
        return ESP_OK;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGW(TAG, "snap: capture failed");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "capture failed\n");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return err;
}

static esp_err_t hnd_stream(httpd_req_t *req)
{
    if (disabled_reply(req)) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_stream_slots, 0) != pdTRUE) {
        ESP_LOGW(TAG, "stream: slot busy, rejecting");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "stream busy\n");
        return ESP_OK;
    }

    esp_err_t err = httpd_resp_set_type(req, STREAM_CT);
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_stream_slots);
        return err;
    }

    char part[80];
    ESP_LOGI(TAG, "stream: client connected");
    while (camera_serving_get()) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGW(TAG, "stream: capture failed, ending");
            err = ESP_FAIL;
            break;
        }

        int hlen = snprintf(part, sizeof(part), PART_HEADER, (unsigned)fb->len);
        err = httpd_resp_send_chunk(req, part, hlen);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        esp_camera_fb_return(fb);

        if (err != ESP_OK) {
            /* Client hung up or socket errored — normal end of stream. */
            break;
        }
    }

    /* Terminate the chunked reply so the browser closes cleanly. */
    httpd_resp_send_chunk(req, NULL, 0);
    xSemaphoreGive(s_stream_slots);
    ESP_LOGI(TAG, "stream: client done (%s)",
             err == ESP_OK ? "camera off" : "socket closed");
    return err;
}

void http_server_start(void)
{
    s_stream_slots = xSemaphoreCreateCounting(STREAM_CLIENT_MAX,
                                              STREAM_CLIENT_MAX);
    configASSERT(s_stream_slots != NULL);

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.task_priority     = 5;              /* == coap io task */
    cfg.stack_size        = 8192;           /* headroom for capture path */
    cfg.max_open_sockets  = 4;              /* index + snap + <=2 streams */
    cfg.max_uri_handlers  = 4;
    cfg.lru_purge_enable  = true;

    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return;
    }

    httpd_uri_t u_index  = { .uri = "/",       .method = HTTP_GET,
                             .handler = hnd_index  };
    httpd_uri_t u_snap   = { .uri = "/snap",   .method = HTTP_GET,
                             .handler = hnd_snap   };
    httpd_uri_t u_stream = { .uri = "/stream", .method = HTTP_GET,
                             .handler = hnd_stream };
    httpd_register_uri_handler(srv, &u_index);
    httpd_register_uri_handler(srv, &u_snap);
    httpd_register_uri_handler(srv, &u_stream);

    ESP_LOGI(TAG, "http server ready on tcp/%d (streams<=%d)",
             cfg.server_port, STREAM_CLIENT_MAX);
}
