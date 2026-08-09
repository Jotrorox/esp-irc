#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool connected;
    int8_t rssi;
    uint8_t channel;
    char ip_address[16];
} wifi_status_t;

void wifi_init(void);

/** Returns true and writes the station IPv4 address when connected. */
bool wifi_get_ip_address(char *buffer, size_t buffer_size);

/** Returns a snapshot suitable for status/diagnostic displays. */
void wifi_get_status(wifi_status_t *status);
