/*
 * http_server.c — esp_http_server on tcp/80.
 *
 *   GET /         -> tiny HTML that <img src="/stream">
 *   GET /snap     -> single JPEG (Content-Type: image/jpeg)
 *   GET /stream   -> MJPEG (multipart/x-mixed-replace)
 *
 * When camera_serving_get() is false, all three return 503 with a
 * plain-text body — the browser tab says something instead of spinning.
 *
 * Concurrency: the earlier version ran /stream synchronously on the
 * single esp_http_server worker task, so an abandoned browser tab that
 * had not yet been TCP-torn-down held the worker hostage until
 * send_wait_timeout — and with two abandoned tabs, /snap and / stayed
 * wedged indefinitely. /stream now detaches to its own FreeRTOS task
 * via httpd_req_async_handler_begin(), leaving the httpd worker free.
 *
 * Resource lifecycle: fb, stream slot, terminating chunk, and the
 * async request handle are all released at a single `cleanup:` label
 * in stream_task, so no branch can leak them. hnd_snap uses the same
 * fb-return-then-log discipline (cache size before return so we do
 * not touch fb after freeing it).
 *
 * Timeouts: send_wait_timeout and recv_wait_timeout are set explicitly
 * so a future config edit cannot accidentally regress them to 0.
 *
 * Debug logging: slot acquire/release and fb get/return log at LOGD.
 * Enable at runtime with esp_log_level_set("http", ESP_LOG_DEBUG) or
 * globally via CONFIG_LOG_DEFAULT_LEVEL_DEBUG.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "camera.h"
#include "http_server.h"

static const char *TAG = "http";

#define STREAM_CLIENT_MAX  2
#define STREAM_TASK_STACK  6144
/* Below the coap io task (prio 5) so control-plane traffic (turning
   the camera off, flipping the relay) is never starved by streaming. */
#define STREAM_TASK_PRIO   4

/* Explicit: HTTPD_DEFAULT_CONFIG already picks 5s, but pinning it here
   guarantees a dead peer cannot pin a worker forever regardless of
   future default changes. */
#define SEND_TIMEOUT_S     5
#define RECV_TIMEOUT_S     5

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

/* Best-effort count of slots currently taken, for log lines. */
static int slots_taken(void)
{
    return STREAM_CLIENT_MAX - (int)uxSemaphoreGetCount(s_stream_slots);
}

/* Returns true (and sends the response) if serving is off. */
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
    ESP_LOGD(TAG, "snap fb_get -> %p", fb);
    if (fb == NULL) {
        ESP_LOGW(TAG, "snap: capture failed");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "capture failed\n");
    }

    /* Cache the size for the log line — fb is invalid after
       esp_camera_fb_return(). */
    size_t fb_len = fb->len;

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    ESP_LOGD(TAG, "snap fb_return (%zu bytes, send=%d)", fb_len, err);
    return err;
}

/* ---- stream: runs in its own FreeRTOS task so the httpd worker is
        free to serve /snap and / concurrently ---- */

static void stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    esp_err_t    err = ESP_OK;
    camera_fb_t *fb  = NULL;
    char part[80];

    ESP_LOGI(TAG, "stream: task started, slots=%d/%d",
             slots_taken(), STREAM_CLIENT_MAX);

    err = httpd_resp_set_type(req, STREAM_CT);
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }
    if (err != ESP_OK) {
        goto cleanup;
    }

    while (camera_serving_get()) {
        fb = esp_camera_fb_get();
        ESP_LOGD(TAG, "stream fb_get -> %p", fb);
        if (fb == NULL) {
            ESP_LOGW(TAG, "stream: capture failed, ending");
            err = ESP_FAIL;
            goto cleanup;
        }

        int hlen = snprintf(part, sizeof(part), PART_HEADER,
                            (unsigned)fb->len);
        err = httpd_resp_send_chunk(req, part, hlen);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }

        esp_camera_fb_return(fb);
        ESP_LOGD(TAG, "stream fb_return (send=%d)", err);
        fb = NULL;

        if (err != ESP_OK) {
            /* Send failure = client hung up, or send_wait_timeout hit
               because the peer stopped reading. Either way, break so
               the slot returns instead of looping on a dead socket. */
            ESP_LOGI(TAG, "stream: client gone (send=%d)", err);
            break;
        }
    }

cleanup:
    /* Single owner of every releasable resource. Any error path above
       is a bare `goto cleanup;` — no branch has to know the order. */
    if (fb != NULL) {
        esp_camera_fb_return(fb);
        ESP_LOGD(TAG, "stream fb_return (cleanup path)");
    }
    /* Terminating chunk is best-effort — a dead socket will fail this
       and that is fine. */
    httpd_resp_send_chunk(req, NULL, 0);

    xSemaphoreGive(s_stream_slots);
    ESP_LOGI(TAG, "stream: task done, slots=%d/%d",
             slots_taken(), STREAM_CLIENT_MAX);

    /* Tells the httpd server the socket for this async request may now
       be purged. Must be called exactly once per async_handler_begin. */
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t hnd_stream(httpd_req_t *req)
{
    if (disabled_reply(req)) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_stream_slots, 0) != pdTRUE) {
        ESP_LOGW(TAG, "stream: no slots (%d/%d), rejecting",
                 slots_taken(), STREAM_CLIENT_MAX);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "stream busy\n");
    }
    ESP_LOGD(TAG, "stream: slot taken (%d/%d)",
             slots_taken(), STREAM_CLIENT_MAX);

    /* Detach the request from the httpd worker: without this the sole
       worker task is locked onto this handler and every other URI
       queues behind it until the client's TCP finally times out. */
    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream: async_handler_begin failed: 0x%x", err);
        xSemaphoreGive(s_stream_slots);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "internal error\n");
    }

    BaseType_t ok = xTaskCreate(stream_task, "http_stream",
                                STREAM_TASK_STACK, async_req,
                                STREAM_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "stream: xTaskCreate failed");
        httpd_req_async_handler_complete(async_req);
        xSemaphoreGive(s_stream_slots);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "internal error\n");
    }

    /* stream_task now owns the request. Returning ESP_OK here does NOT
       close the socket — the async handle keeps it open until
       httpd_req_async_handler_complete() runs in the task's cleanup. */
    return ESP_OK;
}

void http_server_start(void)
{
    s_stream_slots = xSemaphoreCreateCounting(STREAM_CLIENT_MAX,
                                              STREAM_CLIENT_MAX);
    configASSERT(s_stream_slots != NULL);

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.task_priority     = 5;              /* == coap io task */
    cfg.stack_size        = 8192;
    /* Each in-flight async /stream keeps its socket alive until
       httpd_req_async_handler_complete(), so budget streams + headroom
       for /snap, /, and one spare listen slot. */
    cfg.max_open_sockets  = STREAM_CLIENT_MAX + 3;
    cfg.max_uri_handlers  = 4;
    cfg.lru_purge_enable  = true;
    cfg.send_wait_timeout = SEND_TIMEOUT_S;
    cfg.recv_wait_timeout = RECV_TIMEOUT_S;

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

    ESP_LOGI(TAG, "http server ready on tcp/%d (streams<=%d, timeout=%ds)",
             cfg.server_port, STREAM_CLIENT_MAX, SEND_TIMEOUT_S);
}
