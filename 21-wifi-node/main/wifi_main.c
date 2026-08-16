/*
 * wifi_main.c — join Wi-Fi, then hand off to the CoAP server.
 *
 * Same LED/relay CoAP surface as 20-thread-led, but over UDP on IPv4.
 * The client scripts don't care which transport carries the packets.
 */

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "camera.h"
#include "coap_server.h"
#include "http_server.h"

static const char *TAG = "wifi_main";

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Camera up before the network: the sensor is mains-powered and
       stays initialised for the life of the app, so the ~100 ms init
       cost is paid once, out of the CoAP hot path. */
    camera_init();

    /* Blocks until an IP is obtained. Configure SSID/PSK via
       CONFIG_EXAMPLE_WIFI_SSID / CONFIG_EXAMPLE_WIFI_PASSWORD. */
    ESP_ERROR_CHECK(example_connect());

    /* The default WIFI_PS_MIN_MODEM batches inbound packets to save
       power; on a mains-powered node it just adds multi-second CoAP
       latency for no benefit. Turn it off. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "wifi power save disabled");

    coap_server_start();
    http_server_start();
}
