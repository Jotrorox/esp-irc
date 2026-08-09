#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESSAGE_STORE_CHANNEL_MAX 31
#define MESSAGE_STORE_NICK_MAX 23
#define MESSAGE_STORE_MESSAGE_MAX 511

typedef enum {
    MESSAGE_STORE_PRIVMSG = 1,
    MESSAGE_STORE_NOTICE = 2,
} message_store_type_t;

void message_store_init(void);

/* Copies the message into a bounded queue; never blocks the IRC client task. */
bool message_store_enqueue(message_store_type_t type, const char *channel,
                           const char *nick, const char *message,
                           int64_t timestamp_seconds, int32_t timestamp_microseconds);

typedef struct {
    message_store_type_t type;
    int64_t timestamp_seconds;
    int32_t timestamp_microseconds;
    char channel[MESSAGE_STORE_CHANNEL_MAX + 1];
    char nick[MESSAGE_STORE_NICK_MAX + 1];
    char message[MESSAGE_STORE_MESSAGE_MAX + 1];
} message_store_record_t;

typedef enum {
    MESSAGE_STORE_LATEST,
    MESSAGE_STORE_LATEST_AFTER,
    MESSAGE_STORE_BEFORE,
    MESSAGE_STORE_AFTER,
} message_store_query_t;

/* Returns matching records in ascending order. The caller owns records. */
bool message_store_query(const char *channel, message_store_query_t query,
                         int64_t reference_seconds, int32_t reference_microseconds,
                         size_t limit, message_store_record_t *records,
                         size_t *count, bool *more_available);
