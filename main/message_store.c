#include "message_store.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "wear_levelling.h"

#include "config.h"

#define STORE_PATH "/store"
#define STORE_PARTITION "storage"
#define STORE_TASK_STACK 4096
#define STORE_QUEUE_LENGTH 64
#define STORE_SEGMENT_COUNT 44
#define STORE_SEGMENT_SIZE (128 * 1024)
#define STORE_MAGIC 0x43495245U /* "ERIC", stored little-endian */
#define STORE_VERSION 1
typedef struct {
    uint8_t type;
    int64_t timestamp_seconds;
    int32_t timestamp_microseconds;
    char channel[MESSAGE_STORE_CHANNEL_MAX + 1];
    char nick[MESSAGE_STORE_NICK_MAX + 1];
    char message[MESSAGE_STORE_MESSAGE_MAX + 1];
} queued_message_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    int64_t timestamp_seconds;
    int32_t timestamp_microseconds;
    uint8_t type;
    uint8_t channel_length;
    uint8_t nick_length;
    uint16_t message_length;
    uint32_t crc32;
} stored_header_t;

static QueueHandle_t store_queue;
static wl_handle_t wl_handle = WL_INVALID_HANDLE;

static uint32_t crc32_update(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    while (length--) {
        crc ^= *bytes++;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc;
}

static void segment_path(unsigned index, char *path, size_t size)
{
    snprintf(path, size, STORE_PATH "/messages-%u.bin", index);
}

static unsigned choose_segment(size_t *current_size)
{
    unsigned chosen = 0;
    time_t newest = 0;
    bool found = false;

    for (unsigned i = 0; i < STORE_SEGMENT_COUNT; ++i) {
        char path[48];
        struct stat st;
        segment_path(i, path, sizeof(path));
        if (stat(path, &st) == 0 && (!found || st.st_mtime >= newest)) {
            chosen = i;
            newest = st.st_mtime;
            *current_size = (size_t)st.st_size;
            found = true;
        }
    }
    if (!found) *current_size = 0;
    return chosen;
}

static FILE *open_segment(unsigned index, const char *mode)
{
    char path[48];
    segment_path(index, path, sizeof(path));
    return fopen(path, mode);
}

static void save_current_segment(unsigned index)
{
    FILE *index_file = fopen(STORE_PATH "/current.idx", "wb");
    if (!index_file) return;
    uint8_t value = (uint8_t)index;
    if (fwrite(&value, sizeof(value), 1, index_file) != 1 ||
        fflush(index_file) != 0 || fsync(fileno(index_file)) != 0)
        ESP_LOGW(TAG, "Cannot update message segment index: errno %d", errno);
    fclose(index_file);
}

static bool load_current_segment(unsigned *index, size_t *size)
{
    FILE *index_file = fopen(STORE_PATH "/current.idx", "rb");
    uint8_t value;
    if (!index_file) return false;
    bool ok = fread(&value, sizeof(value), 1, index_file) == 1 &&
              value < STORE_SEGMENT_COUNT;
    fclose(index_file);
    if (!ok) return false;

    char path[48];
    struct stat st;
    segment_path(value, path, sizeof(path));
    *index = value;
    *size = stat(path, &st) == 0 ? (size_t)st.st_size : 0;
    return true;
}

static bool write_message(FILE *file, const queued_message_t *message)
{
    stored_header_t header = {
        .magic = STORE_MAGIC,
        .version = STORE_VERSION,
        .type = message->type,
        .channel_length = (uint8_t)strlen(message->channel),
        .nick_length = (uint8_t)strlen(message->nick),
        .message_length = (uint16_t)strlen(message->message),
    };
    header.timestamp_seconds = message->timestamp_seconds;
    header.timestamp_microseconds = message->timestamp_microseconds;
    header.record_size = sizeof(header) + header.channel_length +
                         header.nick_length + header.message_length;

    stored_header_t crc_header = header;
    crc_header.crc32 = 0;
    uint32_t crc = crc32_update(UINT32_MAX, &crc_header, sizeof(crc_header));
    crc = crc32_update(crc, message->channel, header.channel_length);
    crc = crc32_update(crc, message->nick, header.nick_length);
    crc = crc32_update(crc, message->message, header.message_length);
    header.crc32 = crc ^ UINT32_MAX;

    bool ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
              fwrite(message->channel, header.channel_length, 1, file) == 1 &&
              fwrite(message->nick, header.nick_length, 1, file) == 1 &&
              fwrite(message->message, header.message_length, 1, file) == 1 &&
              fflush(file) == 0 && fsync(fileno(file)) == 0;
    return ok;
}

static void store_task(void *parameter)
{
    (void)parameter;
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 2,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .use_one_fat = false,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        STORE_PATH, STORE_PARTITION, &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Message storage unavailable: %s", esp_err_to_name(err));
        queued_message_t discarded;
        while (true) xQueueReceive(store_queue, &discarded, portMAX_DELAY);
    }

    size_t segment_size = 0;
    unsigned segment;
    if (!load_current_segment(&segment, &segment_size))
        segment = choose_segment(&segment_size);
    if (segment_size >= STORE_SEGMENT_SIZE) {
        segment = (segment + 1) % STORE_SEGMENT_COUNT;
        segment_size = 0;
    }
    save_current_segment(segment);
    FILE *file = open_segment(segment, segment_size ? "ab" : "wb");
    if (!file) ESP_LOGE(TAG, "Cannot open message segment: errno %d", errno);

    queued_message_t message;
    while (xQueueReceive(store_queue, &message, portMAX_DELAY) == pdTRUE) {
        size_t record_size = sizeof(stored_header_t) + strlen(message.channel) +
                             strlen(message.nick) + strlen(message.message);
        if (!file) {
            file = open_segment(segment, "ab");
            if (!file) {
                ESP_LOGE(TAG, "Cannot reopen message segment: errno %d", errno);
                continue;
            }
        }
        if (segment_size + record_size > STORE_SEGMENT_SIZE) {
            fclose(file);
            segment = (segment + 1) % STORE_SEGMENT_COUNT;
            segment_size = 0;
            file = open_segment(segment, "wb");
            if (!file) {
                ESP_LOGE(TAG, "Cannot rotate message segment: errno %d", errno);
                continue;
            }
            save_current_segment(segment);
        }
        if (write_message(file, &message)) {
            segment_size += record_size;
        } else {
            ESP_LOGE(TAG, "Message write failed: errno %d", errno);
            fclose(file);
            file = NULL;
        }
    }
}

void message_store_init(void)
{
    /* The backlog is intentionally placed in the XIAO's 8 MB octal PSRAM. */
    store_queue = xQueueCreateWithCaps(STORE_QUEUE_LENGTH,
                                      sizeof(queued_message_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!store_queue) {
        ESP_LOGE(TAG, "Cannot allocate message storage queue");
        return;
    }
    if (xTaskCreatePinnedToCore(store_task, "message_store", STORE_TASK_STACK,
                                NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create message storage task");
        vQueueDeleteWithCaps(store_queue);
        store_queue = NULL;
    }
}

bool message_store_enqueue(message_store_type_t type, const char *channel,
                           const char *nick, const char *message,
                           int64_t timestamp_seconds, int32_t timestamp_microseconds)
{
    if (!store_queue || !channel || !nick || !message) return false;
    queued_message_t queued = {
        .type = (uint8_t)type,
        .timestamp_seconds = timestamp_seconds,
        .timestamp_microseconds = timestamp_microseconds,
    };
    strlcpy(queued.channel, channel, sizeof(queued.channel));
    strlcpy(queued.nick, nick, sizeof(queued.nick));
    strlcpy(queued.message, message, sizeof(queued.message));
    if (xQueueSend(store_queue, &queued, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Message storage queue full; dropping persisted copy");
        return false;
    }
    return true;
}

static bool timestamp_matches(message_store_query_t query,
                              int64_t seconds, int32_t microseconds,
                              int64_t reference_seconds, int32_t reference_microseconds)
{
    int comparison = seconds < reference_seconds ? -1 :
                     seconds > reference_seconds ? 1 :
                     microseconds < reference_microseconds ? -1 :
                     microseconds > reference_microseconds ? 1 : 0;
    return query == MESSAGE_STORE_LATEST ||
           (query == MESSAGE_STORE_LATEST_AFTER && comparison > 0) ||
           (query == MESSAGE_STORE_BEFORE && comparison < 0) ||
           (query == MESSAGE_STORE_AFTER && comparison > 0);
}

static bool read_record(FILE *file, message_store_record_t *record)
{
    stored_header_t header;
    if (fread(&header, sizeof(header), 1, file) != 1) return false;
    size_t payload_size = (size_t)header.channel_length + header.nick_length +
                          header.message_length;
    if (header.magic != STORE_MAGIC || header.version != STORE_VERSION ||
        header.type < MESSAGE_STORE_PRIVMSG || header.type > MESSAGE_STORE_NOTICE ||
        header.channel_length > MESSAGE_STORE_CHANNEL_MAX ||
        header.nick_length > MESSAGE_STORE_NICK_MAX ||
        header.message_length > MESSAGE_STORE_MESSAGE_MAX ||
        header.record_size != sizeof(header) + payload_size) return false;

    record->type = (message_store_type_t)header.type;
    record->timestamp_seconds = header.timestamp_seconds;
    record->timestamp_microseconds = header.timestamp_microseconds;
    if (fread(record->channel, header.channel_length, 1, file) != 1 ||
        fread(record->nick, header.nick_length, 1, file) != 1 ||
        fread(record->message, header.message_length, 1, file) != 1) return false;
    record->channel[header.channel_length] = '\0';
    record->nick[header.nick_length] = '\0';
    record->message[header.message_length] = '\0';

    stored_header_t crc_header = header;
    crc_header.crc32 = 0;
    uint32_t crc = crc32_update(UINT32_MAX, &crc_header, sizeof(crc_header));
    crc = crc32_update(crc, record->channel, header.channel_length);
    crc = crc32_update(crc, record->nick, header.nick_length);
    crc = crc32_update(crc, record->message, header.message_length);
    return (crc ^ UINT32_MAX) == header.crc32;
}

bool message_store_query(const char *channel, message_store_query_t query,
                         int64_t reference_seconds, int32_t reference_microseconds,
                         size_t limit, message_store_record_t *records,
                         size_t *count, bool *more_available)
{
    if (!channel || !records || !count || !more_available || limit == 0 ||
        query < MESSAGE_STORE_LATEST || query > MESSAGE_STORE_AFTER ||
        wl_handle == WL_INVALID_HANDLE) return false;

    unsigned current;
    size_t ignored_size;
    if (!load_current_segment(&current, &ignored_size)) return false;

    size_t matched = 0;
    for (unsigned step = 1; step <= STORE_SEGMENT_COUNT; ++step) {
        unsigned segment = (current + step) % STORE_SEGMENT_COUNT;
        FILE *file = open_segment(segment, "rb");
        if (!file) continue;
        message_store_record_t record;
        while (read_record(file, &record)) {
            if (strcasecmp(record.channel, channel) != 0 ||
                !timestamp_matches(query, record.timestamp_seconds,
                                   record.timestamp_microseconds,
                                   reference_seconds, reference_microseconds)) continue;
            if (matched < limit) {
                records[matched] = record;
            } else if (query != MESSAGE_STORE_AFTER) {
                memmove(records, records + 1, (limit - 1) * sizeof(*records));
                records[limit - 1] = record;
            }
            matched++;
        }
        fclose(file);
    }
    *more_available = matched > limit;
    *count = matched < limit ? matched : limit;
    return true;
}
