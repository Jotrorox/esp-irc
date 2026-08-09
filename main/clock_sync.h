#pragma once

#include <stdbool.h>
#include <stdint.h>

void clock_sync_init(void);
bool clock_sync_now(int64_t *seconds, int32_t *microseconds);
