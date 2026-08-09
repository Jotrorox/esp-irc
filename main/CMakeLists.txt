idf_component_register(SRCS "main.c" "wifi.c" "irc_server.c" "display.c"
                       PRIV_REQUIRES nvs_flash
                       PRIV_REQUIRES esp_wifi
                       PRIV_REQUIRES esp_driver_i2c
                       INCLUDE_DIRS ".")
