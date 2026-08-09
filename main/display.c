#include "display.h"

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "display";

#define I2C_MASTER_PORT I2C_NUM_0

static esp_err_t display_i2c_init(i2c_master_bus_handle_t *bus_handle,
                                  i2c_master_dev_handle_t *dev_handle)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_PORT,
        .sda_io_num = CONFIG_DISPLAY_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_DISPLAY_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, bus_handle);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_DISPLAY_I2C_ADDRESS,
        .scl_speed_hz = CONFIG_DISPLAY_I2C_FREQUENCY,
    };

    err = i2c_master_bus_add_device(*bus_handle, &device_config, dev_handle);
    if (err != ESP_OK) {
        i2c_del_master_bus(*bus_handle);
        *bus_handle = NULL;
    }

    return err;
}

void display_task(void *pvParameters)
{
    (void)pvParameters;

    i2c_master_bus_handle_t bus_handle = NULL;
    i2c_master_dev_handle_t dev_handle = NULL;

    esp_err_t err = display_i2c_init(&bus_handle, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C initialization failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "I2C display initialized successfully");

    while (1) {
        /* TODO: Read IRC state and update the display here. */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DISPLAY_UPDATE_INTERVAL_MS));
    }

    /* Kept here for when the task receives a shutdown path. */
    i2c_master_bus_rm_device(dev_handle);
    i2c_del_master_bus(bus_handle);
    vTaskDelete(NULL);
}
