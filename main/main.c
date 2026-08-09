#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "config.h"

#include "wifi.h"
#include "irc_server.h"
#include "display.h"
#include "clock_sync.h"
#include "message_store.h"

#define NETWORK_CORE 0
#define APPLICATION_CORE 1

void app_main(void)
{
    // NVS is required by the Wi-Fi driver
    ESP_ERROR_CHECK(nvs_flash_init());

    // TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI("main", "XIAO ESP32-S3: %u MB PSRAM available, using CPU cores 0 and 1",
             (unsigned int)(esp_psram_get_size() / (1024 * 1024)));

    clock_sync_init();
    message_store_init();
    wifi_init();
    BaseType_t irc_task_created = xTaskCreatePinnedToCore(
        irc_server_task, "irc_server", 4096, (void *)AF_INET, 5, NULL,
        NETWORK_CORE);
    ESP_ERROR_CHECK(irc_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t display_task_created = xTaskCreatePinnedToCore(
        display_task,
        "display",
        3072,
        NULL,
        4,
        NULL,
        APPLICATION_CORE);
    ESP_ERROR_CHECK(display_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
