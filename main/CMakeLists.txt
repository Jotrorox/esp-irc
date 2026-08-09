idf_component_register(SRCS "main.c" "wifi.c" "irc_server.c" "display.c"
                            "clock_sync.c" "message_store.c"
                       PRIV_REQUIRES nvs_flash esp_wifi esp_driver_i2c
                                     fatfs wear_levelling esp_netif esp_psram
                       INCLUDE_DIRS ".")
