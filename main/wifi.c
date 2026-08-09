#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "config.h"
#include "wifi.h"

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t wifi_event_group;
static esp_ip4_addr_t wifi_ip_address;

static void wifi_health_task(void *pvParameters)
{
    (void)pvParameters;

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_HEALTH_CHECK_INTERVAL_MS));

        if ((bits & WIFI_CONNECTED_BIT) == 0) {
            ESP_LOGW(TAG, "Wi-Fi health: disconnected");
            continue;
        }

        wifi_ap_record_t ap_info;
        esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi health: connected, RSSI %d dBm, channel %u",
                     ap_info.rssi, (unsigned int)ap_info.primary);
        } else {
            ESP_LOGW(TAG, "Wi-Fi health check failed: %s; reconnecting",
                     esp_err_to_name(err));
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
            err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_HEALTH_CHECK_INTERVAL_MS));
    }
}

static void event_handler(void *arg,
                          esp_event_base_t event_base,
                          int32_t event_id,
                          void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();
    }

    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *)event_data;

        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_ip_address.addr = 0;
        ESP_LOGW(TAG, "Disconnected (reason %d), reconnecting...", event->reason);

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(err));
        }
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "Got IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));
        wifi_ip_address = event->ip_info.ip;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    // Create default Wi-Fi station interface
    esp_netif_create_default_wifi_sta();

    // Initialize Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &event_handler,
            NULL));

    // Configure Wi-Fi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start());

    BaseType_t task_created = xTaskCreatePinnedToCore(
        wifi_health_task,
        "wifi_health",
        3072,
        NULL,
        4,
        NULL,
        0);
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "Wi-Fi initialization finished");
}

bool wifi_get_ip_address(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0 || wifi_event_group == NULL ||
        (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) == 0) {
        return false;
    }

    esp_ip4_addr_t ip = wifi_ip_address;
    snprintf(buffer, buffer_size, IPSTR, IP2STR(&ip));
    return true;
}
