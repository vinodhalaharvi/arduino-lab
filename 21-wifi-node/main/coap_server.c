/*
 * coap_server.c — bring up libcoap on UDP/5683 and run its io loop.
 *
 * Resources are registered by the modules that own them (led_coap.c,
 * relay_coap.c) rather than centralised here, so each domain keeps its
 * handler code and its device driver in one file.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "coap_server.h"

static const char *TAG = "coap_srv";

/* Woken up by fresh packets or by an internal libcoap timeout — the
   value here is just the maximum idle wait, not a poll interval. */
#define IO_WAIT_MS  1000

static coap_context_t *s_ctx;

static void coap_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "coap io task running");
    while (1) {
        int rc = coap_io_process(s_ctx, IO_WAIT_MS);
        if (rc < 0) {
            ESP_LOGE(TAG, "coap_io_process failed: %d", rc);
            break;
        }
    }
    coap_free_context(s_ctx);
    coap_cleanup();
    s_ctx = NULL;
    vTaskDelete(NULL);
}

void coap_server_start(void)
{
    coap_startup();

    s_ctx = coap_new_context(NULL);
    if (s_ctx == NULL) {
        ESP_LOGE(TAG, "coap_new_context failed");
        return;
    }
    coap_context_set_block_mode(s_ctx,
                                COAP_BLOCK_USE_LIBCOAP | COAP_BLOCK_SINGLE_BODY);

    /* Bind to all local addresses on the default CoAP port (5683/udp).
       "0.0.0.0" over "::" because sdkconfig.defaults selects IPv4 only. */
    coap_addr_info_t *info = coap_resolve_address_info(
        coap_make_str_const("0.0.0.0"),
        COAP_DEFAULT_PORT, 0, 0, 0,
        0, 1 << COAP_URI_SCHEME_COAP,
        COAP_RESOLVE_TYPE_LOCAL);
    if (info == NULL) {
        ESP_LOGE(TAG, "coap_resolve_address_info failed");
        coap_free_context(s_ctx);
        s_ctx = NULL;
        return;
    }

    coap_endpoint_t *ep = NULL;
    for (coap_addr_info_t *ai = info; ai != NULL; ai = ai->next) {
        ep = coap_new_endpoint(s_ctx, &ai->addr, ai->proto);
        if (ep != NULL) {
            break;
        }
    }
    coap_free_address_info(info);
    if (ep == NULL) {
        ESP_LOGE(TAG, "coap_new_endpoint failed");
        coap_free_context(s_ctx);
        s_ctx = NULL;
        return;
    }

    /* Domain modules register their own resources against the shared
       context — see led_coap.c and relay_coap.c. */
    led_coap_register(s_ctx);
    relay_coap_register(s_ctx);
    cam_coap_register(s_ctx);

    ESP_LOGI(TAG, "coap server ready on udp/%d", COAP_DEFAULT_PORT);

    /* 5 KB stack: handlers here are trivial but coap_io_process can
       recurse through DTLS state machines and we don't want a stack
       overflow on the day someone flips on CoAPs. */
    xTaskCreate(coap_task, "coap_io", 5120, NULL, 5, NULL);
}
