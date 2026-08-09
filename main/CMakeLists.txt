idf_component_register(SRCS "main.c" "wifi.c" "irc_server.c" "display.c"
                            "clock_sync.c" "message_store.c"
                       PRIV_REQUIRES nvs_flash esp_wifi esp_driver_i2c
                                     fatfs wear_levelling esp_netif esp_psram
                                     mbedtls esp_timer
                       INCLUDE_DIRS ".")

if(CONFIG_IRC_TLS_ENABLED)
    if(NOT IS_ABSOLUTE "${CONFIG_IRC_TLS_CERTIFICATE_PATH}")
        set(IRC_TLS_CERTIFICATE "${PROJECT_DIR}/${CONFIG_IRC_TLS_CERTIFICATE_PATH}")
    else()
        set(IRC_TLS_CERTIFICATE "${CONFIG_IRC_TLS_CERTIFICATE_PATH}")
    endif()
    if(NOT IS_ABSOLUTE "${CONFIG_IRC_TLS_PRIVATE_KEY_PATH}")
        set(IRC_TLS_PRIVATE_KEY "${PROJECT_DIR}/${CONFIG_IRC_TLS_PRIVATE_KEY_PATH}")
    else()
        set(IRC_TLS_PRIVATE_KEY "${CONFIG_IRC_TLS_PRIVATE_KEY_PATH}")
    endif()

    if(NOT EXISTS "${IRC_TLS_CERTIFICATE}")
        message(FATAL_ERROR "IRC TLS certificate not found: ${IRC_TLS_CERTIFICATE}")
    endif()
    if(NOT EXISTS "${IRC_TLS_PRIVATE_KEY}")
        message(FATAL_ERROR "IRC TLS private key not found: ${IRC_TLS_PRIVATE_KEY}")
    endif()

    configure_file("${IRC_TLS_CERTIFICATE}"
                   "${CMAKE_CURRENT_BINARY_DIR}/irc_tls_certificate.pem" COPYONLY)
    configure_file("${IRC_TLS_PRIVATE_KEY}"
                   "${CMAKE_CURRENT_BINARY_DIR}/irc_tls_private_key.pem" COPYONLY)
    target_add_binary_data(${COMPONENT_LIB}
                           "${CMAKE_CURRENT_BINARY_DIR}/irc_tls_certificate.pem" TEXT)
    target_add_binary_data(${COMPONENT_LIB}
                           "${CMAKE_CURRENT_BINARY_DIR}/irc_tls_private_key.pem" TEXT)
endif()
