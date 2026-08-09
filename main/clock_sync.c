#include "clock_sync.h"

#include <sys/time.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "config.h"

/* Reject the unset Unix epoch and obviously stale RTC values. */
#define MIN_VALID_UNIX_TIME 1704067200LL /* 2024-01-01T00:00:00Z */
#define SNTP_SERVER "pool.ntp.org"

static void time_sync_notification(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP synchronized: %lld", (long long)tv->tv_sec);
}

void clock_sync_init(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    config.sync_cb = time_sync_notification;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP initialization failed: %s", esp_err_to_name(err));
    }
}

bool clock_sync_now(int64_t *seconds, int32_t *microseconds)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    bool valid = (int64_t)now.tv_sec >= MIN_VALID_UNIX_TIME;
    if (seconds) *seconds = valid ? (int64_t)now.tv_sec : 0;
    if (microseconds) *microseconds = valid ? (int32_t)now.tv_usec : 0;
    return valid;
}
