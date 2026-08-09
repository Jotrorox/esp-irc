#pragma once

#include <stdbool.h>

typedef enum {
    MESSAGE_STORE_PRIVMSG = 1,
    MESSAGE_STORE_NOTICE = 2,
} message_store_type_t;

void message_store_init(void);

/* Copies the message into a bounded queue; never blocks the IRC client task. */
bool message_store_enqueue(message_store_type_t type, const char *channel,
                           const char *nick, const char *message);
