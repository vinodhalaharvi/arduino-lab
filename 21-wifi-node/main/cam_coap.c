/*
 * cam_coap.c — /cam CoAP resource.
 *
 *   GET  /cam           -> "on http://<ip>/stream"  |  "off"
 *   POST /cam  "on"     -> start serving frames over HTTP
 *   POST /cam  "off"    -> stop serving frames over HTTP
 *
 * GET returning the URL means a controller can discover it rather than
 * constructing it from an address it has to know already.
 *
 * Neither on nor off touches the sensor — camera_init() ran at boot
 * and the OV2640 stays powered for the life of the app. Only the HTTP
 * handlers gate on the flag.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"

#include "coap3/coap.h"
#include "coap_server.h"
#include "camera.h"

static const char *TAG = "cam_coap";

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

/* Best-effort: fetch the current IPv4 of the default netif so GET /cam
   can return a browsable URL. If it's unavailable for some reason we
   still tell the caller the camera is on. */
static bool current_ip(char *out, size_t cap)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return false;
    }
    snprintf(out, cap, IPSTR, IP2STR(&ip.ip));
    return true;
}

static void hnd_cam(coap_resource_t *r, coap_session_t *s,
                    const coap_pdu_t *req, const coap_string_t *query,
                    coap_pdu_t *resp)
{
    (void)r; (void)s; (void)query;
    coap_pdu_code_t code = coap_pdu_get_code(req);

    if (code == COAP_REQUEST_CODE_GET) {
        if (camera_serving_get()) {
            char ip[16];
            char out[64];
            if (current_ip(ip, sizeof(ip))) {
                snprintf(out, sizeof(out), "on http://%s/stream", ip);
                reply(resp, COAP_RESPONSE_CODE_CONTENT, out);
            } else {
                reply(resp, COAP_RESPONSE_CODE_CONTENT, "on");
            }
        } else {
            reply(resp, COAP_RESPONSE_CODE_CONTENT, "off");
        }
        return;
    }

    char buf[16];
    body_string(req, buf, sizeof(buf));

    if (strcmp(buf, "on") == 0) {
        camera_serving_set(true);
        ESP_LOGI(TAG, "serving on");
        reply(resp, COAP_RESPONSE_CODE_CHANGED, "on");

    } else if (strcmp(buf, "off") == 0) {
        camera_serving_set(false);
        ESP_LOGI(TAG, "serving off");
        reply(resp, COAP_RESPONSE_CODE_CHANGED, "off");

    } else {
        reply(resp, COAP_RESPONSE_CODE_BAD_REQUEST, "on|off");
    }
}

void cam_coap_register(coap_context_t *ctx)
{
    coap_resource_t *r = coap_resource_init(coap_make_str_const("cam"), 0);
    coap_register_handler(r, COAP_REQUEST_GET,  hnd_cam);
    coap_register_handler(r, COAP_REQUEST_POST, hnd_cam);
    coap_register_handler(r, COAP_REQUEST_PUT,  hnd_cam);
    coap_add_resource(ctx, r);

    ESP_LOGI(TAG, "/cam ready");
}
