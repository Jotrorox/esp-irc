#pragma once

#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

#define IRC_SERVER_NAME "esp-irc"
#define IRC_MOTD        "A tiny IRC server running on an ESP32."

#define PORT                6667 /* Plaintext IRC; TLS uses CONFIG_IRC_TLS_PORT. */
#define KEEPALIVE_IDLE      60
#define KEEPALIVE_INTERVAL  10
#define KEEPALIVE_COUNT     3

#define WIFI_HEALTH_CHECK_INTERVAL_MS 30000

#define TAG "esp-irc"
