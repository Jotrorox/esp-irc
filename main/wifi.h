#pragma once

#include <stdbool.h>
#include <stddef.h>

void wifi_init(void);

/** Returns true and writes the station IPv4 address when connected. */
bool wifi_get_ip_address(char *buffer, size_t buffer_size);
