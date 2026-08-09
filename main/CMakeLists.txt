idf_component_register(SRCS "main.c" "wifi.c" "irc_server.c"
                       PRIV_REQUIRES nvs_flash
                       PRIV_REQUIRES esp_wifi
                       INCLUDE_DIRS ".")
