#include "display.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "clock_sync.h"
#include "config.h"
#include "irc_server.h"
#include "wifi.h"

#undef TAG
static const char *TAG = "display";

/* T-Display-S3 ST7789, landscape with the USB connector on the left.
 * Pinout and panel timing: https://github.com/Xinyuan-LilyGO/LilyGo-Display-IDF
 */
#define DISPLAY_WIDTH       320
#define DISPLAY_HEIGHT      170
#define TRANSFER_ROWS       20
#define TRANSFER_BYTES      (DISPLAY_WIDTH * TRANSFER_ROWS * sizeof(uint16_t))
#define LCD_POWER_GPIO      15
#define LCD_BACKLIGHT_GPIO  38
#define LCD_RESET_GPIO      5
#define LCD_CS_GPIO         6
#define LCD_DC_GPIO         7
#define LCD_WR_GPIO         8
#define LCD_RD_GPIO         9

#define COLOR_BACKGROUND 0x0841
#define COLOR_CARD       0x10c3
#define COLOR_TEXT       0xffff
#define COLOR_MUTED      0x9cf3
#define COLOR_ACCENT     0x2e7f
#define COLOR_ONLINE     0x47ef
#define COLOR_OFFLINE    0xfd26

static uint16_t *framebuffer;
static uint16_t *transfer_buffer;
static SemaphoreHandle_t transfer_done;
static esp_lcd_i80_bus_handle_t lcd_bus;
static esp_lcd_panel_io_handle_t lcd_io;
static esp_lcd_panel_handle_t lcd_panel;

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

static bool display_transfer_done(esp_lcd_panel_io_handle_t io,
                                  esp_lcd_panel_io_event_data_t *event,
                                  void *context)
{
    (void)io;
    (void)event;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &task_woken);
    return task_woken == pdTRUE;
}

static esp_err_t display_controller_init(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << LCD_POWER_GPIO) |
                        (1ULL << LCD_BACKLIGHT_GPIO) | (1ULL << LCD_RD_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "configure LCD outputs");
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_BACKLIGHT_GPIO, 0), TAG, "backlight off");
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_POWER_GPIO, 1), TAG, "LCD power on");
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_RD_GPIO, 1), TAG, "disable LCD reads");
    vTaskDelay(pdMS_TO_TICKS(10));

    transfer_done = xSemaphoreCreateBinary();
    framebuffer = heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (transfer_done == NULL || framebuffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = LCD_DC_GPIO,
        .wr_gpio_num = LCD_WR_GPIO,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {39, 40, 41, 42, 45, 46, 47, 48},
        .bus_width = 8,
        .max_transfer_bytes = TRANSFER_BYTES,
        .dma_burst_size = 64,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_config, &lcd_bus), TAG,
                        "create LCD bus");

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = LCD_CS_GPIO,
        .pclk_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {.dc_data_level = 1},
        /* Native RGB565 words are little endian; ST7789 expects MSB first. */
        .flags.swap_color_bytes = true,
        .on_color_trans_done = display_transfer_done,
        .user_ctx = transfer_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(lcd_bus, &io_config, &lcd_io),
                        TAG, "create LCD IO");
    transfer_buffer = esp_lcd_i80_alloc_draw_buffer(
        lcd_io, TRANSFER_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (transfer_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io, &panel_config, &lcd_panel),
                        TAG, "create ST7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(lcd_panel), TAG, "reset LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(lcd_panel), TAG, "initialize LCD");

    /* LILYGO's panel-specific porch, voltage and gamma register values. */
    static const struct {
        uint8_t command;
        uint8_t length;
        uint8_t data[14];
    } panel_commands[] = {
        {0x3a, 1, {0x05}},
        {0xb2, 5, {0x0b, 0x0b, 0x00, 0x33, 0x33}},
        {0xb7, 1, {0x75}},
        {0xbb, 1, {0x28}},
        {0xc0, 1, {0x2c}},
        {0xc2, 1, {0x01}},
        {0xc3, 1, {0x1f}},
        {0xc6, 1, {0x13}},
        {0xd0, 1, {0xa7}},
        {0xd0, 2, {0xa4, 0xa1}},
        {0xd6, 1, {0xa1}},
        {0xe0, 14, {0xf0, 0x05, 0x0a, 0x06, 0x06, 0x03, 0x2b,
                    0x32, 0x43, 0x36, 0x11, 0x10, 0x2b, 0x32}},
        {0xe1, 14, {0xf0, 0x08, 0x0c, 0x0b, 0x09, 0x24, 0x2b,
                    0x22, 0x43, 0x38, 0x15, 0x16, 0x2f, 0x37}},
    };
    for (size_t i = 0; i < sizeof(panel_commands) / sizeof(panel_commands[0]); ++i) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd_io,
                            panel_commands[i].command, panel_commands[i].data,
                            panel_commands[i].length), TAG, "configure LCD panel");
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(lcd_panel, true), TAG, "LCD inversion");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(lcd_panel, true), TAG, "LCD landscape");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(lcd_panel, false, true), TAG, "LCD orientation");
    /* The visible 170 rows occupy the middle of the controller's 240 rows. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(lcd_panel, 0, 35), TAG, "LCD offset");
    return esp_lcd_panel_disp_on_off(lcd_panel, true);
}

static void display_cleanup(void)
{
    gpio_set_level(LCD_BACKLIGHT_GPIO, 0);
    if (lcd_panel != NULL) esp_lcd_panel_del(lcd_panel);
    if (lcd_io != NULL) esp_lcd_panel_io_del(lcd_io);
    if (lcd_bus != NULL) esp_lcd_del_i80_bus(lcd_bus);
    if (transfer_done != NULL) vSemaphoreDelete(transfer_done);
    heap_caps_free(transfer_buffer);
    heap_caps_free(framebuffer);
}

static void set_pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT) {
        framebuffer[y * DISPLAY_WIDTH + x] = color;
    }
}

static void fill_rectangle(int x, int y, int width, int height, uint16_t color)
{
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            set_pixel(column, row, color);
        }
    }
}

static void draw_character(int x, int y, char character, int scale, uint16_t color)
{
    const char *match = strchr(glyph_characters, character);
    if (match == NULL) {
        return;
    }
    const uint8_t *glyph = glyphs[match - glyph_characters];
    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 7; ++row) {
            if ((glyph[column] & (1U << row)) != 0) {
                fill_rectangle(x + column * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, int scale, uint16_t color)
{
    while (*text != '\0' && x + 5 * scale <= DISPLAY_WIDTH) {
        draw_character(x, y, *text++, scale, color);
        x += 6 * scale;
    }
}

static void draw_wifi_icon(int x, int y, const wifi_status_t *wifi)
{
    int bars = !wifi->connected ? 0 : wifi->rssi >= -60 ? 3 : wifi->rssi >= -75 ? 2 : 1;
    for (int i = 0; i < 3; ++i) {
        int height = 6 + i * 5;
        fill_rectangle(x + i * 7, y + 16 - height, 5, height,
                       i < bars ? COLOR_ONLINE : COLOR_MUTED);
    }
    if (!wifi->connected) {
        for (int i = 0; i < 19; ++i) {
            set_pixel(x + i, y + 17 - i, COLOR_OFFLINE);
            set_pixel(x + i, y + 18 - i, COLOR_OFFLINE);
        }
    }
}

static void compose_screen(const wifi_status_t *wifi, uint32_t users,
                           uint32_t free_heap, uint64_t uptime_seconds)
{
    fill_rectangle(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, COLOR_BACKGROUND);
    draw_text(12, 10, "ESP-IRC", 2, COLOR_ACCENT);

    char line[48];
    uint64_t days = uptime_seconds / 86400;
    uint64_t hours = (uptime_seconds / 3600) % 24;
    uint64_t minutes = (uptime_seconds / 60) % 60;
    if (days > 0) {
        snprintf(line, sizeof(line), "UP %lluD %02lluH",
                 (unsigned long long)days, (unsigned long long)hours);
    } else {
        snprintf(line, sizeof(line), "UP %02llu:%02llu",
                 (unsigned long long)hours, (unsigned long long)minutes);
    }
    draw_text(DISPLAY_WIDTH - 12 - (int)strlen(line) * 6, 14, line, 1, COLOR_MUTED);
    fill_rectangle(12, 32, 296, 1, COLOR_MUTED);

    draw_wifi_icon(12, 45, wifi);
    draw_text(42, 44, wifi->connected ? wifi->ip_address : "WIFI DISCONNECTED",
              2, wifi->connected ? COLOR_ONLINE : COLOR_OFFLINE);
    if (wifi->connected) {
        snprintf(line, sizeof(line), "SIGNAL %d DBM   CHANNEL %u", wifi->rssi,
                 (unsigned int)wifi->channel);
    } else {
        snprintf(line, sizeof(line), "WAITING TO RECONNECT");
    }
    draw_text(42, 66, line, 1, COLOR_MUTED);

    fill_rectangle(12, 84, 142, 47, COLOR_CARD);
    fill_rectangle(164, 84, 144, 47, COLOR_CARD);
    draw_text(22, 92, "CONNECTED USERS", 1, COLOR_MUTED);
    snprintf(line, sizeof(line), "%lu", (unsigned long)users);
    draw_text(22, 108, line, 2, COLOR_TEXT);
    draw_text(174, 92, "FREE MEMORY", 1, COLOR_MUTED);
    snprintf(line, sizeof(line), "%lu KB", (unsigned long)(free_heap / 1024));
    draw_text(174, 108, line, 2, COLOR_TEXT);

#if CONFIG_IRC_TLS_ENABLED
    snprintf(line, sizeof(line), "IRC %d   TLS %d", PORT, CONFIG_IRC_TLS_PORT);
#else
    snprintf(line, sizeof(line), "IRC PORT %d", PORT);
#endif
    draw_text(12, 141, line, 1, COLOR_TEXT);

    int64_t seconds;
    if (clock_sync_now(&seconds, NULL)) {
        time_t timestamp = (time_t)seconds;
        struct tm utc;
        gmtime_r(&timestamp, &utc);
        snprintf(line, sizeof(line), "UTC %02d:%02d:%02d", utc.tm_hour,
                 utc.tm_min, utc.tm_sec);
    } else {
        snprintf(line, sizeof(line), "UTC SYNCING");
    }
    draw_text(12, 156, line, 1, COLOR_MUTED);
    draw_text(236, 156, "T-DISPLAY-S3", 1, COLOR_MUTED);
}

static esp_err_t display_flush(void)
{
    for (int y = 0; y < DISPLAY_HEIGHT; y += TRANSFER_ROWS) {
        int rows = DISPLAY_HEIGHT - y;
        if (rows > TRANSFER_ROWS) rows = TRANSFER_ROWS;
        memcpy(transfer_buffer, &framebuffer[y * DISPLAY_WIDTH],
               DISPLAY_WIDTH * rows * sizeof(uint16_t));
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(lcd_panel, 0, y,
                            DISPLAY_WIDTH, y + rows, transfer_buffer),
                            TAG, "write LCD pixels");
        /* The draw call queues DMA; do not reuse its buffer until completion. */
        xSemaphoreTake(transfer_done, portMAX_DELAY);
    }
    return ESP_OK;
}

void display_task(void *pvParameters)
{
    (void)pvParameters;
    esp_err_t err = display_controller_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display initialization failed: %s", esp_err_to_name(err));
        display_cleanup();
        vTaskDelete(NULL);
        return;
    }

    uint64_t previous_uptime = UINT64_MAX;
    bool backlight_on = false;
    while (1) {
        uint64_t uptime = (uint64_t)(esp_timer_get_time() / 1000000);
        if (uptime != previous_uptime) {
            wifi_status_t wifi;
            wifi_get_status(&wifi);
            compose_screen(&wifi, irc_server_get_user_count(),
                           esp_get_free_heap_size(), uptime);
            err = display_flush();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Display update failed: %s", esp_err_to_name(err));
            } else if (!backlight_on) {
                /* Reveal only a completely drawn frame on startup. */
                err = gpio_set_level(LCD_BACKLIGHT_GPIO, 1);
                if (err == ESP_OK) {
                    backlight_on = true;
                    ESP_LOGI(TAG, "T-Display-S3 ST7789 initialized: 320x170 landscape, first frame displayed");
                }
            }
            previous_uptime = uptime;
        }
        TickType_t delay = pdMS_TO_TICKS(CONFIG_DISPLAY_UPDATE_INTERVAL_MS);
        vTaskDelay(delay > 0 ? delay : 1);
    }
}
