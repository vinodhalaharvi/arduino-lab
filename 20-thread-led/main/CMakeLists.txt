idf_component_register(SRCS "esp_ot_cli.c"
                            "led_coap.c"
                            "relay_coap.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver
                                     esp_event
                                     esp_netif
                                     openthread
                                     nvs_flash
                                     vfs)
