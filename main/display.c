#include "display.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#include "irc_server.h"
#include "wifi.h"

static const char *TAG = "display";

#define DISPLAY_WIDTH  128
#define DISPLAY_HEIGHT 64
#define DISPLAY_PAGES  (DISPLAY_HEIGHT / 8)
#define I2C_MASTER_PORT I2C_NUM_0
#define I2C_TIMEOUT_MS  1000

static uint8_t framebuffer[DISPLAY_WIDTH * DISPLAY_PAGES];

/* 5x7 glyphs for 0-9, A-Z, '.', ':', and '-'. */
static const char glyph_characters[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:-";
static const uint8_t glyphs[][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e},{0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
    {0x00,0x60,0x60,0x00,0x00},{0x00,0x36,0x36,0x00,0x00},
    {0x08,0x08,0x08,0x08,0x08},
};

static esp_err_t display_send_commands(i2c_master_dev_handle_t device,
                                       const uint8_t *commands, size_t length)
{
    uint8_t data[32];
    if (length + 1 > sizeof(data)) {
        return ESP_ERR_INVALID_SIZE;
    }
    data[0] = 0x00;
    memcpy(&data[1], commands, length);
    return i2c_master_transmit(device, data, length + 1, I2C_TIMEOUT_MS);
}

static esp_err_t display_controller_init(i2c_master_dev_handle_t device)
{
    const uint8_t commands[] = {
        0xae,       /* Display off. */
        0xd5, 0x80, /* Clock divide ratio. */
        0xa8, 0x3f, /* 64 multiplexed rows. */
        0xd3, 0x00, /* No display offset. */
        0x40,       /* Start line 0. */
        0x8d, 0x14, /* Enable SSD1306 charge pump (ignored by SSD1309). */
        0x20, 0x00, /* Horizontal addressing mode. */
        0xa1,       /* Mirror columns. */
        0xc8,       /* Scan rows from COM63 to COM0. */
        0xda, 0x12, /* Alternative COM pin layout for 128x64. */
        0x81, 0xcf, /* Contrast. */
        0xd9, 0xf1, /* Pre-charge period. */
        0xdb, 0x40, /* VCOMH deselect level. */
        0xa4,       /* Use display RAM. */
        0xa6,       /* Normal (not inverted) pixels. */
        0xaf,       /* Display on. */
    };
    return display_send_commands(device, commands, sizeof(commands));
}

static esp_err_t display_i2c_init(i2c_master_bus_handle_t *bus,
                                  i2c_master_dev_handle_t *device)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_PORT,
        .sda_io_num = CONFIG_DISPLAY_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_DISPLAY_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, bus);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_DISPLAY_I2C_ADDRESS,
        .scl_speed_hz = CONFIG_DISPLAY_I2C_FREQUENCY,
    };
    err = i2c_master_bus_add_device(*bus, &device_config, device);
    if (err != ESP_OK) {
        i2c_del_master_bus(*bus);
        *bus = NULL;
    }
    return err;
}

static void set_pixel(int x, int y)
{
    if (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT) {
        framebuffer[x + (y / 8) * DISPLAY_WIDTH] |= (uint8_t)(1U << (y & 7));
    }
}

static const uint8_t *find_glyph(char character)
{
    const char *match = strchr(glyph_characters, character);
    return match == NULL ? NULL : glyphs[match - glyph_characters];
}

static void draw_character(int x, int y, char character, int scale)
{
    const uint8_t *glyph = find_glyph(character);
    if (glyph == NULL) {
        return;
    }
    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 7; ++row) {
            if ((glyph[column] & (1U << row)) != 0) {
                for (int dx = 0; dx < scale; ++dx) {
                    for (int dy = 0; dy < scale; ++dy) {
                        set_pixel(x + column * scale + dx, y + row * scale + dy);
                    }
                }
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, int scale)
{
    while (*text != '\0') {
        draw_character(x, y, *text++, scale);
        x += 6 * scale;
    }
}

static void draw_wifi_icon(int x, int y, bool connected)
{
    if (!connected) {
        /* A small X clearly distinguishes disconnected from weak signal. */
        for (int i = 0; i < 9; ++i) {
            set_pixel(x + i, y + i);
            set_pixel(x + 8 - i, y + i);
        }
        return;
    }

    /* Three progressively smaller arcs and the center dot. */
    const uint8_t rows[] = {0x7c, 0x82, 0x38, 0x44, 0x10, 0x28, 0x00, 0x10};
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 8; ++column) {
            if ((rows[row] & (1U << (7 - column))) != 0) {
                set_pixel(x + column, y + row);
            }
        }
    }
}

static void compose_screen(bool connected, const char *ip_address, uint32_t users)
{
    memset(framebuffer, 0, sizeof(framebuffer));
    draw_wifi_icon(2, 2, connected);
    draw_text(15, 2, connected ? ip_address : "DISCONNECTED", 1);
    for (int x = 0; x < DISPLAY_WIDTH; ++x) {
        set_pixel(x, 12);
    }

    draw_text(19, 20, "CONNECTED USERS", 1);

    char count[11];
    snprintf(count, sizeof(count), "%lu", (unsigned long)users);
    int count_width = (int)strlen(count) * 18 - 3;
    draw_text((DISPLAY_WIDTH - count_width) / 2, 36, count, 3);
}

static esp_err_t display_flush(i2c_master_dev_handle_t device)
{
    const uint8_t address_commands[] = {0x21, 0, 127, 0x22, 0, 7};
    esp_err_t err = display_send_commands(device, address_commands,
                                          sizeof(address_commands));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t data[DISPLAY_WIDTH + 1];
    data[0] = 0x40;
    for (int page = 0; page < DISPLAY_PAGES; ++page) {
        memcpy(&data[1], &framebuffer[page * DISPLAY_WIDTH], DISPLAY_WIDTH);
        err = i2c_master_transmit(device, data, sizeof(data), I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

void display_task(void *pvParameters)
{
    (void)pvParameters;
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t device = NULL;

    esp_err_t err = display_i2c_init(&bus, &device);
    if (err == ESP_OK) {
        err = display_controller_init(device);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display initialization failed: %s", esp_err_to_name(err));
        if (device != NULL) i2c_master_bus_rm_device(device);
        if (bus != NULL) i2c_del_master_bus(bus);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SSD1306/SSD1309 display initialized");
    bool previous_connected = false;
    uint32_t previous_users = UINT32_MAX;
    char previous_ip[16] = "";

    while (1) {
        char ip_address[16] = "";
        bool connected = wifi_get_ip_address(ip_address, sizeof(ip_address));
        uint32_t users = irc_server_get_user_count();

        if (connected != previous_connected || users != previous_users ||
            strcmp(ip_address, previous_ip) != 0) {
            compose_screen(connected, ip_address, users);
            err = display_flush(device);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Display update failed: %s", esp_err_to_name(err));
            }
            previous_connected = connected;
            previous_users = users;
            snprintf(previous_ip, sizeof(previous_ip), "%s", ip_address);
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DISPLAY_UPDATE_INTERVAL_MS));
    }
}
