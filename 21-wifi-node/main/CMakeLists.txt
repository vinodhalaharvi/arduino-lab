idf_component_register(SRCS "wifi_main.c"
                            "coap_server.c"
                            "led_coap.c"
                            "relay_coap.c"
                            "cam_coap.c"
                            "camera.c"
                            "http_server.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver
                                     esp_driver_ledc
                                     esp_event
                                     esp_http_server
                                     esp_netif
                                     esp_wifi
                                     nvs_flash
                                     protocol_examples_common)
