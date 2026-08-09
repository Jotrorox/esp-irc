#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include <lwip/netdb.h>

#if CONFIG_IRC_TLS_ENABLED
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#endif
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config.h"
#include "clock_sync.h"
#include "irc_server.h"
#include "message_store.h"

#define IRC_MAX_USERS          16
#define IRC_MAX_CHANNELS       8
#define IRC_MAX_LINE           512
#define IRC_NICK_LEN           24
#define IRC_USER_LEN           24
#define IRC_REALNAME_LEN       48
#define IRC_CHANNEL_LEN        32
/* TLS handshakes require considerably more stack than plaintext clients. */
#define IRC_CLIENT_STACK_SIZE  12288
#define IRC_CLIENT_PRIORITY    5
#define IRC_MAX_OUTPUT         768
#define IRC_HISTORY_LIMIT      50

typedef struct irc_client {
    int socket;
    bool used;
    bool closing;
    unsigned pending_sends;
    SemaphoreHandle_t tx_lock;
#if CONFIG_IRC_TLS_ENABLED
    mbedtls_net_context tls_net;
    mbedtls_ssl_context tls_ssl;
    bool tls_initialized;
    bool use_tls;
#endif
    bool registered;
    bool cap_negotiating;
    bool cap_batch;
    bool cap_server_time;
    bool cap_message_tags;
    bool cap_chathistory;
    bool away;
    char away_message[96];
    char quit_message[96];
    char nick[IRC_NICK_LEN];
    char user[IRC_USER_LEN];
    char realname[IRC_REALNAME_LEN];
    bool joined[IRC_MAX_CHANNELS];
} irc_client_t;

typedef struct {
    bool used;
    char name[IRC_CHANNEL_LEN];
    char topic[96];
} irc_channel_t;

static irc_client_t clients[IRC_MAX_USERS];
static irc_channel_t channels[IRC_MAX_CHANNELS];
static SemaphoreHandle_t state_lock;

#if CONFIG_IRC_TLS_ENABLED
static mbedtls_ssl_config tls_config;
static mbedtls_x509_crt tls_certificate;
static mbedtls_pk_context tls_private_key;

extern const unsigned char irc_tls_certificate_pem_start[]
    asm("_binary_irc_tls_certificate_pem_start");
extern const unsigned char irc_tls_certificate_pem_end[]
    asm("_binary_irc_tls_certificate_pem_end");
extern const unsigned char irc_tls_private_key_pem_start[]
    asm("_binary_irc_tls_private_key_pem_start");
extern const unsigned char irc_tls_private_key_pem_end[]
    asm("_binary_irc_tls_private_key_pem_end");
#endif

static void lock_state(void) { xSemaphoreTake(state_lock, portMAX_DELAY); }
static void unlock_state(void) { xSemaphoreGive(state_lock); }

typedef struct {
    irc_client_t *client;
    SemaphoreHandle_t tx_lock;
    bool cap_server_time;
} recipient_t;

static void snapshot_client_locked(recipient_t *recipient, irc_client_t *client);

uint32_t irc_server_get_user_count(void)
{
    uint32_t count = 0;
    if (state_lock == NULL) return 0;
    lock_state();
    for (int i = 0; i < IRC_MAX_USERS; ++i) count += clients[i].used;
    unlock_state();
    return count;
}

static int transport_write(irc_client_t *client, const char *data, size_t length)
{
#if CONFIG_IRC_TLS_ENABLED
    if (!client->use_tls)
        return send(client->socket, data, length, 0);
    int result = mbedtls_ssl_write(&client->tls_ssl,
                                   (const unsigned char *)data, length);
    if (result == MBEDTLS_ERR_SSL_WANT_READ ||
        result == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    return result;
#else
    return send(client->socket, data, length, 0);
#endif
}

static int transport_read(irc_client_t *client, char *data, size_t length)
{
#if CONFIG_IRC_TLS_ENABLED
    if (!client->use_tls)
        return recv(client->socket, data, length, 0);
    int result = mbedtls_ssl_read(&client->tls_ssl, (unsigned char *)data,
                                  length);
    if (result == MBEDTLS_ERR_SSL_WANT_READ ||
        result == MBEDTLS_ERR_SSL_WANT_WRITE) return -EAGAIN;
    return result;
#else
    return recv(client->socket, data, length, 0);
#endif
}

static bool send_all(irc_client_t *client, const char *data, size_t length)
{
    while (length > 0) {
        int sent = transport_write(client, data, length);
        if (sent < 0) return false;
        if (sent == 0) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool send_line_to_client(SemaphoreHandle_t tx_lock, irc_client_t *client,
                                const char *line)
{
    char output[IRC_MAX_OUTPUT + 3];
    size_t length = strnlen(line, IRC_MAX_OUTPUT);
    memcpy(output, line, length);
    output[length++] = '\r';
    output[length++] = '\n';
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    bool sent = send_all(client, output, length);
    xSemaphoreGive(tx_lock);
    return sent;
}

static bool send_line(irc_client_t *client, const char *line)
{
    lock_state();
    SemaphoreHandle_t tx_lock = client->tx_lock;
    unlock_state();
    return send_line_to_client(tx_lock, client, line);
}

static void format_timestamp(int64_t seconds, int32_t microseconds,
                             char *output, size_t size)
{
    time_t value = (time_t)seconds;
    struct tm utc;
    gmtime_r(&value, &utc);
    size_t length = strftime(output, size, "%Y-%m-%dT%H:%M:%S", &utc);
    unsigned milliseconds = microseconds >= 0 && microseconds < 1000000
                                ? (unsigned)microseconds / 1000 : 0;
    if (length && length < size)
        snprintf(output + length, size - length, ".%03uZ", milliseconds);
}

static bool parse_timestamp(const char *reference, int64_t *seconds,
                            int32_t *microseconds)
{
    if (!reference || strncmp(reference, "timestamp=", 10) != 0) return false;
    int year, month, day, hour, minute, second, millisecond;
    char tail;
    if (sscanf(reference + 10, "%4d-%2d-%2dT%2d:%2d:%2d.%3dZ%c",
               &year, &month, &day, &hour, &minute, &second,
               &millisecond, &tail) != 7 || year < 1970 || month < 1 ||
        month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 60 || millisecond < 0 || millisecond > 999) return false;
    struct tm utc = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
    };
    time_t parsed = timegm(&utc);
    if (parsed < 0) return false;
    *seconds = (int64_t)parsed;
    *microseconds = millisecond * 1000;
    return true;
}

static void reply(irc_client_t *client, int numeric, const char *text)
{
    char line[IRC_MAX_LINE];
    char nick[IRC_NICK_LEN];
    lock_state();
    strlcpy(nick, client->nick[0] ? client->nick : "*", sizeof(nick));
    unlock_state();
    snprintf(line, sizeof(line), ":%s %03d %s %s", IRC_SERVER_NAME, numeric,
             nick, text);
    send_line(client, line);
}

static void prefix(const irc_client_t *client, char *out, size_t size)
{
    snprintf(out, size, "%s!%s@esp.local", client->nick,
             client->user[0] ? client->user : "unknown");
}

/* Caller holds state_lock. References keep sockets valid after it is released. */
static size_t snapshot_recipients_locked(recipient_t *recipients,
                                         const irc_client_t *except,
                                         int channel_index)
{
    size_t count = 0;
    for (int i = 0; i < IRC_MAX_USERS; ++i) {
        irc_client_t *target = &clients[i];
        if (!target->used || target->closing || target == except) continue;
        if (channel_index >= 0 && !target->joined[channel_index]) continue;
        target->pending_sends++;
        recipients[count++] = (recipient_t) {
            .client = target,
            .tx_lock = target->tx_lock,
            .cap_server_time = target->cap_server_time,
        };
    }
    return count;
}

/* Snapshot each user that shares at least one channel with source, once. */
static size_t snapshot_shared_recipients_locked(recipient_t *recipients,
                                                const irc_client_t *source,
                                                bool include_source)
{
    size_t count = 0;
    for (int i = 0; i < IRC_MAX_USERS; ++i) {
        irc_client_t *target = &clients[i];
        if (!target->used || target->closing || (!include_source && target == source)) continue;
        bool shared = target == source;
        for (int c = 0; !shared && c < IRC_MAX_CHANNELS; ++c)
            shared = source->joined[c] && target->joined[c];
        if (!shared) continue;
        snapshot_client_locked(&recipients[count++], target);
    }
    return count;
}

static void snapshot_client_locked(recipient_t *recipient, irc_client_t *client)
{
    client->pending_sends++;
    *recipient = (recipient_t) {
        .client = client,
        .tx_lock = client->tx_lock,
        .cap_server_time = client->cap_server_time,
    };
}

static void release_recipient(recipient_t *recipient)
{
    lock_state();
    recipient->client->pending_sends--;
    unlock_state();
}

static void send_recipient(recipient_t *recipient, const char *line)
{
    send_line_to_client(recipient->tx_lock, recipient->client, line);
    release_recipient(recipient);
}

static void send_live_message(recipient_t *target, const char *line,
                              int64_t seconds, int32_t microseconds)
{
    if (!target->cap_server_time || seconds == 0) {
        send_recipient(target, line);
        return;
    }
    char timestamp[32], tagged[IRC_MAX_OUTPUT + 1];
    format_timestamp(seconds, microseconds, timestamp, sizeof(timestamp));
    snprintf(tagged, sizeof(tagged), "@time=%s %s", timestamp, line);
    send_recipient(target, tagged);
}

static bool valid_nick(const char *nick)
{
    if (!nick[0] || !(isalpha((unsigned char)nick[0]) || strchr("[]\\`_^{|}", nick[0])))
        return false;
    for (size_t i = 1; nick[i]; ++i) {
        if (!(isalnum((unsigned char)nick[i]) ||
              strchr("-[]\\`_^{|}", nick[i]))) return false;
    }
    return strlen(nick) < IRC_NICK_LEN;
}

/* Caller holds state_lock. */
static int find_channel_locked(const char *name)
{
    for (int i = 0; i < IRC_MAX_CHANNELS; ++i)
        if (channels[i].used && !strcasecmp(channels[i].name, name)) return i;
    return -1;
}

static void send_names(irc_client_t *client, int channel_index)
{
    char names[350] = "";
    lock_state();
    for (int i = 0; i < IRC_MAX_USERS; ++i) {
        if (!clients[i].used || !clients[i].registered || !clients[i].joined[channel_index]) continue;
        if (names[0]) strlcat(names, " ", sizeof(names));
        strlcat(names, clients[i].nick, sizeof(names));
    }
    char channel[IRC_CHANNEL_LEN];
    strlcpy(channel, channels[channel_index].name, sizeof(channel));
    unlock_state();

    char text[IRC_MAX_LINE];
    snprintf(text, sizeof(text), "= %s :%s", channel, names);
    reply(client, 353, text);
    snprintf(text, sizeof(text), "%s :End of /NAMES list", channel);
    reply(client, 366, text);
}

static void send_motd(irc_client_t *client)
{
    char text[IRC_MAX_LINE];
    snprintf(text, sizeof(text), ":- %s Message of the Day -", IRC_SERVER_NAME);
    reply(client, 375, text);
    snprintf(text, sizeof(text), ":- %s", IRC_MOTD);
    reply(client, 372, text);
    reply(client, 376, ":End of /MOTD command");
}

static void complete_registration(irc_client_t *client)
{
    char nick[IRC_NICK_LEN], user[IRC_USER_LEN];
    lock_state();
    if (client->registered || client->cap_negotiating ||
        !client->nick[0] || !client->user[0]) {
        unlock_state();
        return;
    }
    client->registered = true;
    strlcpy(nick, client->nick, sizeof(nick));
    strlcpy(user, client->user, sizeof(user));
    unlock_state();
    char text[IRC_MAX_LINE];
    snprintf(text, sizeof(text), ":Welcome to %s, %s!%s@%s", IRC_SERVER_NAME, nick, user, IRC_SERVER_NAME);
    reply(client, 1, text);
    snprintf(text, sizeof(text), ":Your host is %s, running version 1.0", IRC_SERVER_NAME);
    reply(client, 2, text);
    reply(client, 3, ":This server was created for ESP-IDF");
    snprintf(text, sizeof(text), "%s 1.0 io nt", IRC_SERVER_NAME);
    reply(client, 4, text);
    reply(client, 5, "CHANTYPES=# NICKLEN=23 CHANNELLEN=31 CASEMAPPING=ascii NETWORK=ESPIRC CHATHISTORY=50 MSGREFTYPES=timestamp :are supported by this server");
    send_motd(client);
}

static void handle_nick(irc_client_t *client, char *nick)
{
    if (!nick || !valid_nick(nick)) { reply(client, 432, "* :Erroneous nickname"); return; }
    lock_state();
    for (int i = 0; i < IRC_MAX_USERS; ++i) {
        if (&clients[i] != client && clients[i].used && !strcasecmp(clients[i].nick, nick)) {
            unlock_state(); reply(client, 433, "* :Nickname is already in use"); return;
        }
    }
    char old_prefix[80] = "";
    recipient_t recipients[IRC_MAX_USERS];
    size_t recipient_count = 0;
    if (client->registered) prefix(client, old_prefix, sizeof(old_prefix));
    strlcpy(client->nick, nick, sizeof(client->nick));
    char new_nick[IRC_NICK_LEN];
    strlcpy(new_nick, client->nick, sizeof(new_nick));
    char line[IRC_MAX_LINE];
    if (old_prefix[0]) {
        snprintf(line, sizeof(line), ":%s NICK :%s", old_prefix, new_nick);
        recipient_count = snapshot_shared_recipients_locked(recipients, client, true);
    }
    unlock_state();
    for (size_t i = 0; i < recipient_count; ++i) send_recipient(&recipients[i], line);
    complete_registration(client);
}

static void handle_one_join(irc_client_t *client, const char *name)
{
    if (!name || name[0] != '#' || strlen(name) >= IRC_CHANNEL_LEN) {
        reply(client, 403, "* :No such channel"); return;
    }
    lock_state();
    int index = find_channel_locked(name);
    if (index < 0) {
        for (int i = 0; i < IRC_MAX_CHANNELS; ++i) if (!channels[i].used) {
            index = i; channels[i].used = true; strlcpy(channels[i].name, name, sizeof(channels[i].name)); break;
        }
    }
    if (index < 0) { unlock_state(); reply(client, 405, ":You have joined too many channels"); return; }
    if (client->joined[index]) { unlock_state(); return; }
    client->joined[index] = true;
    recipient_t recipients[IRC_MAX_USERS];
    char pfx[80], line[IRC_MAX_LINE], topic[96], channel[IRC_CHANNEL_LEN];
    prefix(client, pfx, sizeof(pfx));
    strlcpy(channel, channels[index].name, sizeof(channel));
    strlcpy(topic, channels[index].topic, sizeof(topic));
    snprintf(line, sizeof(line), ":%s JOIN :%s", pfx, channel);
    size_t recipient_count = snapshot_recipients_locked(recipients, NULL, index);
    unlock_state();
    for (size_t i = 0; i < recipient_count; ++i) send_recipient(&recipients[i], line);
    if (topic[0]) { snprintf(line, sizeof(line), "%s :%s", channel, topic); reply(client, 332, line); }
    else { snprintf(line, sizeof(line), "%s :No topic is set", channel); reply(client, 331, line); }
    send_names(client, index);
}

static void handle_join(irc_client_t *client, char *names)
{
    if (!names) { reply(client, 461, "JOIN :Not enough parameters"); return; }
    char list[IRC_MAX_LINE];
    strlcpy(list, names, sizeof(list));
    char *save = NULL;
    for (char *name = strtok_r(list, ",", &save); name;
         name = strtok_r(NULL, ",", &save))
        handle_one_join(client, name);
}

static void handle_part(irc_client_t *client, char *name, char *reason)
{
    lock_state();
    int index = name ? find_channel_locked(name) : -1;
    if (index < 0) { unlock_state(); reply(client, 403, "* :No such channel"); return; }
    if (!client->joined[index]) { unlock_state(); reply(client, 442, "* :You're not on that channel"); return; }
    char pfx[80], line[IRC_MAX_LINE]; prefix(client, pfx, sizeof(pfx));
    recipient_t recipients[IRC_MAX_USERS];
    snprintf(line, sizeof(line), ":%s PART %s :%s", pfx, channels[index].name, reason ? reason : "Leaving");
    size_t recipient_count = snapshot_recipients_locked(recipients, NULL, index);
    client->joined[index] = false;
    unlock_state();
    for (size_t i = 0; i < recipient_count; ++i) send_recipient(&recipients[i], line);
}

static void handle_message(irc_client_t *client, char *target, char *message, bool notice)
{
    if (!target || !message) { if (!notice) reply(client, 461, "PRIVMSG :Not enough parameters"); return; }
    char pfx[80], line[IRC_MAX_LINE];
    recipient_t recipients[IRC_MAX_USERS];
    size_t recipient_count = 0;
    lock_state();
    prefix(client, pfx, sizeof(pfx));
    snprintf(line, sizeof(line), ":%s %s %s :%s", pfx, notice ? "NOTICE" : "PRIVMSG", target, message);
    if (target[0] == '#') {
        int index = find_channel_locked(target);
        if (index < 0 || !client->joined[index]) { unlock_state(); if (!notice) reply(client, 404, "* :Cannot send to channel"); return; }
        int64_t timestamp_seconds;
        int32_t timestamp_microseconds;
        clock_sync_now(&timestamp_seconds, &timestamp_microseconds);
        message_store_enqueue(notice ? MESSAGE_STORE_NOTICE : MESSAGE_STORE_PRIVMSG,
                              channels[index].name, client->nick, message,
                              timestamp_seconds, timestamp_microseconds);
        recipient_count = snapshot_recipients_locked(recipients, client, index);
        unlock_state();
        for (size_t i = 0; i < recipient_count; ++i)
            send_live_message(&recipients[i], line, timestamp_seconds, timestamp_microseconds);
        return;
    } else {
        irc_client_t *recipient = NULL;
        for (int i = 0; i < IRC_MAX_USERS; ++i)
            if (clients[i].used && !clients[i].closing &&
                !strcasecmp(clients[i].nick, target)) recipient = &clients[i];
        if (recipient) {
            snapshot_client_locked(&recipients[0], recipient);
            recipient_count = 1;
        }
        else if (!notice) { unlock_state(); reply(client, 401, "* :No such nick"); return; }
    }
    unlock_state();
    if (recipient_count) send_recipient(&recipients[0], line);
}

static void handle_list(irc_client_t *client)
{
    typedef struct { char name[IRC_CHANNEL_LEN]; char topic[96]; int users; } list_entry_t;
    list_entry_t entries[IRC_MAX_CHANNELS];
    size_t count = 0;
    reply(client, 321, "Channel :Users Name");
    lock_state();
    for (int c = 0; c < IRC_MAX_CHANNELS; ++c) if (channels[c].used) {
        int users = 0; for (int i = 0; i < IRC_MAX_USERS; ++i) users += clients[i].used && clients[i].joined[c];
        strlcpy(entries[count].name, channels[c].name, sizeof(entries[count].name));
        strlcpy(entries[count].topic, channels[c].topic, sizeof(entries[count].topic));
        entries[count++].users = users;
    }
    unlock_state();
    for (size_t i = 0; i < count; ++i) {
        char text[IRC_MAX_LINE];
        snprintf(text, sizeof(text), "%s %d :%s", entries[i].name,
                 entries[i].users, entries[i].topic);
        reply(client, 322, text);
    }
    reply(client, 323, ":End of /LIST");
}

static void handle_topic(irc_client_t *client, const char *name, const char *topic)
{
    if (!name) { reply(client, 461, "TOPIC :Not enough parameters"); return; }
    recipient_t recipients[IRC_MAX_USERS];
    size_t recipient_count = 0;
    char channel[IRC_CHANNEL_LEN] = "", saved_topic[96] = "";
    char pfx[80], line[IRC_MAX_LINE];
    lock_state();
    int index = find_channel_locked(name);
    if (index < 0) { unlock_state(); reply(client, 403, "* :No such channel"); return; }
    if (topic && !client->joined[index]) {
        unlock_state(); reply(client, 442, "* :You're not on that channel"); return;
    }
    if (topic) {
        strlcpy(channels[index].topic, topic, sizeof(channels[index].topic));
        prefix(client, pfx, sizeof(pfx));
        snprintf(line, sizeof(line), ":%s TOPIC %s :%s", pfx, channels[index].name,
                 channels[index].topic);
        recipient_count = snapshot_recipients_locked(recipients, NULL, index);
    }
    strlcpy(channel, channels[index].name, sizeof(channel));
    strlcpy(saved_topic, channels[index].topic, sizeof(saved_topic));
    unlock_state();
    if (topic) {
        for (size_t i = 0; i < recipient_count; ++i) send_recipient(&recipients[i], line);
    } else {
        snprintf(line, sizeof(line), "%s :%s", channel,
                 saved_topic[0] ? saved_topic : "No topic is set");
        reply(client, saved_topic[0] ? 332 : 331, line);
    }
}

static void handle_who(irc_client_t *client, const char *mask)
{
    typedef struct { char channel[IRC_CHANNEL_LEN]; char user[IRC_USER_LEN];
        char nick[IRC_NICK_LEN]; char realname[IRC_REALNAME_LEN]; bool away; } who_t;
    who_t entries[IRC_MAX_USERS]; size_t count = 0;
    const char *query = mask && *mask ? mask : "*";
    lock_state();
    int channel = query[0] == '#' ? find_channel_locked(query) : -1;
    for (int i = 0; i < IRC_MAX_USERS; ++i) {
        irc_client_t *target = &clients[i];
        if (!target->used || !target->registered) continue;
        if (query[0] == '#' && (channel < 0 || !target->joined[channel])) continue;
        if (query[0] != '#' && strcmp(query, "*") && strcasecmp(query, target->nick)) continue;
        strlcpy(entries[count].channel, channel >= 0 ? channels[channel].name : "*", sizeof(entries[count].channel));
        strlcpy(entries[count].user, target->user, sizeof(entries[count].user));
        strlcpy(entries[count].nick, target->nick, sizeof(entries[count].nick));
        strlcpy(entries[count].realname, target->realname, sizeof(entries[count].realname));
        entries[count++].away = target->away;
    }
    unlock_state();
    for (size_t i = 0; i < count; ++i) {
        char text[IRC_MAX_LINE];
        snprintf(text, sizeof(text), "%s %s esp.local %s %s %c :0 %s",
                 entries[i].channel, entries[i].user, IRC_SERVER_NAME,
                 entries[i].nick, entries[i].away ? 'G' : 'H', entries[i].realname);
        reply(client, 352, text);
    }
    char end[IRC_MAX_LINE]; snprintf(end, sizeof(end), "%s :End of /WHO list", query);
    reply(client, 315, end);
}

static void handle_whois(irc_client_t *client, const char *nick)
{
    if (!nick) { reply(client, 431, ":No nickname given"); return; }
    char user[IRC_USER_LEN] = "", realname[IRC_REALNAME_LEN] = "", found_nick[IRC_NICK_LEN] = "";
    char away[96] = "", channel_list[IRC_MAX_LINE] = ""; bool is_away = false;
    lock_state();
    irc_client_t *target = NULL;
    for (int i = 0; i < IRC_MAX_USERS; ++i)
        if (clients[i].used && clients[i].registered && !strcasecmp(clients[i].nick, nick)) target = &clients[i];
    if (target) {
        strlcpy(found_nick, target->nick, sizeof(found_nick)); strlcpy(user, target->user, sizeof(user));
        strlcpy(realname, target->realname, sizeof(realname)); strlcpy(away, target->away_message, sizeof(away));
        is_away = target->away;
        for (int c = 0; c < IRC_MAX_CHANNELS; ++c) if (target->joined[c]) {
            if (channel_list[0]) strlcat(channel_list, " ", sizeof(channel_list));
            strlcat(channel_list, channels[c].name, sizeof(channel_list));
        }
    }
    unlock_state();
    if (!target) { char text[80]; snprintf(text, sizeof(text), "%s :No such nick", nick); reply(client, 401, text); return; }
    char text[IRC_MAX_LINE];
    snprintf(text, sizeof(text), "%s %s esp.local * :%s", found_nick, user, realname); reply(client, 311, text);
    snprintf(text, sizeof(text), "%s %s :ESP IRC server", found_nick, IRC_SERVER_NAME); reply(client, 312, text);
    if (channel_list[0]) {
        int available = (int)sizeof(text) - (int)strlen(found_nick) - 3;
        snprintf(text, sizeof(text), "%s :%.*s", found_nick, available,
                 channel_list);
        reply(client, 319, text);
    }
    if (is_away) { snprintf(text, sizeof(text), "%s :%s", found_nick, away); reply(client, 301, text); }
    snprintf(text, sizeof(text), "%s :End of /WHOIS list", found_nick); reply(client, 318, text);
}

static void handle_mode(irc_client_t *client, const char *target, const char *modes)
{
    if (!target) { reply(client, 461, "MODE :Not enough parameters"); return; }
    char own_nick[IRC_NICK_LEN]; lock_state(); strlcpy(own_nick, client->nick, sizeof(own_nick)); unlock_state();
    if (target[0] == '#') {
        lock_state(); int index = find_channel_locked(target); unlock_state();
        if (index < 0) { reply(client, 403, "* :No such channel"); return; }
        if (!modes) { char text[80]; snprintf(text, sizeof(text), "%s +nt", target); reply(client, 324, text); return; }
        /* +n and +t are fixed channel policy; accept idempotent queries/sets only. */
        for (const char *p = modes; *p; ++p) if (*p != '+' && *p != '-' && *p != 'n' && *p != 't') {
            char text[80]; snprintf(text, sizeof(text), "%c :is unknown mode char to me", *p); reply(client, 472, text); return;
        }
        return;
    }
    if (strcasecmp(target, own_nick)) { reply(client, 502, ":Cannot change mode for other users"); return; }
    if (!modes) { char text[80]; snprintf(text, sizeof(text), "%s +i", own_nick); reply(client, 221, text); return; }
    for (const char *p = modes; *p; ++p) if (*p != '+' && *p != '-' && *p != 'i') {
        char text[80]; snprintf(text, sizeof(text), "%c :Unknown MODE flag", *p); reply(client, 501, text); return;
    }
}

static void handle_away(irc_client_t *client, const char *message)
{
    lock_state();
    client->away = message && *message;
    strlcpy(client->away_message, client->away ? message : "", sizeof(client->away_message));
    unlock_state();
    reply(client, message && *message ? 306 : 305,
          message && *message ? ":You have been marked as being away" : ":You are no longer marked as being away");
}

static void handle_cap(irc_client_t *client, char *subcommand, char *arguments)
{
    char target[IRC_NICK_LEN];
    lock_state();
    strlcpy(target, client->nick[0] ? client->nick : "*", sizeof(target));
    unlock_state();
    char line[IRC_MAX_LINE];
    if (!subcommand) return;
    if (!strcasecmp(subcommand, "LS")) {
        lock_state();
        client->cap_negotiating = true;
        unlock_state();
        snprintf(line, sizeof(line), ":%s CAP %s LS :batch draft/chathistory message-tags server-time",
                 IRC_SERVER_NAME, target);
        send_line(client, line);
        return;
    }
    if (!strcasecmp(subcommand, "LIST")) {
        char enabled[96] = "";
        lock_state();
        if (client->cap_batch) strlcat(enabled, "batch ", sizeof(enabled));
        if (client->cap_chathistory) strlcat(enabled, "draft/chathistory ", sizeof(enabled));
        if (client->cap_message_tags) strlcat(enabled, "message-tags ", sizeof(enabled));
        if (client->cap_server_time) strlcat(enabled, "server-time ", sizeof(enabled));
        unlock_state();
        size_t length = strlen(enabled);
        if (length && enabled[length - 1] == ' ') enabled[length - 1] = '\0';
        snprintf(line, sizeof(line), ":%s CAP %s LIST :%s", IRC_SERVER_NAME, target, enabled);
        send_line(client, line);
        return;
    }
    if (!strcasecmp(subcommand, "END")) {
        lock_state();
        client->cap_negotiating = false;
        unlock_state();
        complete_registration(client);
        return;
    }
    if (strcasecmp(subcommand, "REQ") || !arguments || !*arguments) return;

    lock_state();
    bool batch = client->cap_batch;
    bool chathistory = client->cap_chathistory;
    bool message_tags = client->cap_message_tags;
    bool server_time = client->cap_server_time;
    unlock_state();
    char requested[160];
    strlcpy(requested, arguments, sizeof(requested));
    char *save = NULL;
    for (char *capability = strtok_r(requested, " ", &save); capability;
         capability = strtok_r(NULL, " ", &save)) {
        bool enable = capability[0] != '-';
        const char *name = enable ? capability : capability + 1;
        if (!strcmp(name, "batch")) batch = enable;
        else if (!strcmp(name, "draft/chathistory")) chathistory = enable;
        else if (!strcmp(name, "message-tags")) message_tags = enable;
        else if (!strcmp(name, "server-time")) server_time = enable;
        else {
            snprintf(line, sizeof(line), ":%s CAP %s NAK :%s",
                     IRC_SERVER_NAME, target, arguments);
            send_line(client, line);
            return;
        }
    }
    lock_state();
    client->cap_batch = batch;
    client->cap_chathistory = chathistory;
    client->cap_message_tags = message_tags;
    client->cap_server_time = server_time;
    unlock_state();
    snprintf(line, sizeof(line), ":%s CAP %s ACK :%s",
             IRC_SERVER_NAME, target, arguments);
    send_line(client, line);
}

static void history_fail(irc_client_t *client, const char *code,
                         const char *context, const char *description)
{
    char line[IRC_MAX_LINE];
    snprintf(line, sizeof(line), ":%s FAIL CHATHISTORY %s %s :%s",
             IRC_SERVER_NAME, code, context ? context : "*", description);
    send_line(client, line);
}

static void handle_chathistory(irc_client_t *client, char *subcommand, char *arguments)
{
    lock_state();
    bool cap_chathistory = client->cap_chathistory;
    bool cap_batch = client->cap_batch;
    bool cap_server_time = client->cap_server_time;
    unlock_state();
    if (!cap_chathistory) {
        history_fail(client, "INVALID_PARAMS", subcommand, "Capability not negotiated");
        return;
    }
    if (!subcommand || !arguments) {
        history_fail(client, "INVALID_PARAMS", subcommand, "Insufficient parameters");
        return;
    }

    char args[IRC_MAX_LINE];
    strlcpy(args, arguments, sizeof(args));
    char *save = NULL;
    char *target = strtok_r(args, " ", &save);
    char *reference = strtok_r(NULL, " ", &save);
    char *limit_text = strtok_r(NULL, " ", &save);
    char *extra = strtok_r(NULL, " ", &save);
    if (!target || !reference || !limit_text || extra) {
        history_fail(client, "INVALID_PARAMS", subcommand, "Invalid parameters");
        return;
    }

    char *limit_end;
    long requested_limit = strtol(limit_text, &limit_end, 10);
    if (*limit_end || requested_limit <= 0 || requested_limit > IRC_HISTORY_LIMIT) {
        history_fail(client, "INVALID_PARAMS", limit_text, "Invalid message limit");
        return;
    }

    lock_state();
    int channel_index = target[0] == '#' ? find_channel_locked(target) : -1;
    bool allowed = channel_index >= 0 && client->joined[channel_index];
    unlock_state();
    if (!allowed) {
        history_fail(client, "INVALID_TARGET", target, "Messages could not be retrieved");
        return;
    }

    message_store_query_t query;
    int64_t reference_seconds = 0;
    int32_t reference_microseconds = 0;
    if (!strcasecmp(subcommand, "LATEST")) {
        if (!strcmp(reference, "*")) query = MESSAGE_STORE_LATEST;
        else if (parse_timestamp(reference, &reference_seconds, &reference_microseconds))
            query = MESSAGE_STORE_LATEST_AFTER;
        else {
            history_fail(client, "INVALID_MSGREFTYPE", reference,
                         "Only timestamp references are supported");
            return;
        }
    } else if (!strcasecmp(subcommand, "BEFORE") || !strcasecmp(subcommand, "AFTER")) {
        if (!parse_timestamp(reference, &reference_seconds, &reference_microseconds)) {
            history_fail(client, "INVALID_MSGREFTYPE", reference,
                         "Only timestamp references are supported");
            return;
        }
        query = !strcasecmp(subcommand, "BEFORE") ? MESSAGE_STORE_BEFORE : MESSAGE_STORE_AFTER;
    } else {
        history_fail(client, "INVALID_PARAMS", subcommand, "Unknown subcommand");
        return;
    }

    size_t limit = (size_t)requested_limit;
    message_store_record_t *records = calloc(limit, sizeof(*records));
    if (!records) {
        history_fail(client, "MESSAGE_ERROR", target, "Messages could not be retrieved");
        return;
    }
    size_t count = 0;
    bool more_available = false;
    if (!message_store_query(target, query, reference_seconds, reference_microseconds,
                             limit, records, &count, &more_available)) {
        free(records);
        history_fail(client, "MESSAGE_ERROR", target, "Messages could not be retrieved");
        return;
    }

    char batch_id[12] = "";
    char line[IRC_MAX_OUTPUT + 1];
    if (cap_batch) {
        snprintf(batch_id, sizeof(batch_id), "%08lx", (unsigned long)esp_random());
        snprintf(line, sizeof(line), "%s:%s BATCH +%s chathistory %s",
                 more_available ? "" : "@draft/chathistory-end ",
                 IRC_SERVER_NAME, batch_id, target);
        send_line(client, line);
    }
    for (size_t i = 0; i < count; ++i) {
        char tags[96] = "";
        if (cap_batch) snprintf(tags, sizeof(tags), "batch=%s", batch_id);
        if (cap_server_time) {
            char timestamp[32];
            format_timestamp(records[i].timestamp_seconds,
                             records[i].timestamp_microseconds,
                             timestamp, sizeof(timestamp));
            if (tags[0]) strlcat(tags, ";", sizeof(tags));
            strlcat(tags, "time=", sizeof(tags));
            strlcat(tags, timestamp, sizeof(tags));
        }
        snprintf(line, sizeof(line), "%s%s%s:%s!unknown@esp.local %s %s :%s",
                 tags[0] ? "@" : "", tags, tags[0] ? " " : "",
                 records[i].nick,
                 records[i].type == MESSAGE_STORE_NOTICE ? "NOTICE" : "PRIVMSG",
                 records[i].channel, records[i].message);
        send_line(client, line);
    }
    if (cap_batch) {
        snprintf(line, sizeof(line), ":%s BATCH -%s", IRC_SERVER_NAME, batch_id);
        send_line(client, line);
    }
    free(records);
}

static void handle_command(irc_client_t *client, char *line)
{
    bool has_colon_parameter = strstr(line, " :") != NULL;
    char *save = NULL;
    char *command = strtok_r(line, " ", &save);
    if (!command) return;
    char *param = strtok_r(NULL, " ", &save);
    char *trailing = save;
    while (trailing && *trailing == ' ') trailing++;
    if (trailing && *trailing == ':') trailing++;

    if (!strcasecmp(command, "CAP")) { handle_cap(client, param, trailing); return; }
    if (!strcasecmp(command, "NICK")) { handle_nick(client, param); return; }
    if (!strcasecmp(command, "USER")) {
        if (!param) { reply(client, 461, "USER :Not enough parameters"); return; }
        lock_state();
        strlcpy(client->user, param, sizeof(client->user));
        char *colon = trailing ? strchr(trailing, ':') : NULL;
        strlcpy(client->realname, colon ? colon + 1 : (trailing ? trailing : param), sizeof(client->realname));
        unlock_state();
        complete_registration(client); return;
    }
    if (!strcasecmp(command, "PING")) {
        char response[IRC_MAX_LINE]; snprintf(response, sizeof(response), ":%s PONG %s :%s", IRC_SERVER_NAME, IRC_SERVER_NAME, param ? param : IRC_SERVER_NAME);
        send_line(client, response); return;
    }
    if (!strcasecmp(command, "PONG")) return;
    if (!strcasecmp(command, "QUIT")) {
        lock_state();
        strlcpy(client->quit_message, trailing && *trailing ? trailing : "Client Quit",
                sizeof(client->quit_message));
        int socket = client->socket;
        unlock_state();
        shutdown(socket, SHUT_RDWR); return;
    }
    lock_state(); bool registered = client->registered; unlock_state();
    if (!registered) { reply(client, 451, ":You have not registered"); return; }
    if (!strcasecmp(command, "JOIN")) { handle_join(client, param); return; }
    if (!strcasecmp(command, "PART")) { handle_part(client, param, trailing); return; }
    if (!strcasecmp(command, "PRIVMSG")) { handle_message(client, param, trailing, false); return; }
    if (!strcasecmp(command, "NOTICE")) { handle_message(client, param, trailing, true); return; }
    if (!strcasecmp(command, "CHATHISTORY")) { handle_chathistory(client, param, trailing); return; }
    if (!strcasecmp(command, "LIST")) { handle_list(client); return; }
    if (!strcasecmp(command, "NAMES")) {
        lock_state(); int index = param ? find_channel_locked(param) : -1; unlock_state();
        if (index >= 0) send_names(client, index); else reply(client, 366, "* :End of /NAMES list"); return;
    }
    if (!strcasecmp(command, "MODE")) {
        handle_mode(client, param, trailing && *trailing ? trailing : NULL); return;
    }
    if (!strcasecmp(command, "TOPIC")) {
        handle_topic(client, param, has_colon_parameter ? trailing : NULL); return;
    }
    if (!strcasecmp(command, "WHO")) { handle_who(client, param); return; }
    if (!strcasecmp(command, "WHOIS")) { handle_whois(client, param); return; }
    if (!strcasecmp(command, "AWAY")) { handle_away(client, has_colon_parameter ? trailing : NULL); return; }
    if (!strcasecmp(command, "MOTD")) { send_motd(client); return; }
    if (!strcasecmp(command, "VERSION")) {
        char text[IRC_MAX_LINE];
        snprintf(text, sizeof(text), "esp-irc-1.0 %s :ESP-IDF IRC server", IRC_SERVER_NAME);
        reply(client, 351, text);
        return;
    }
    reply(client, 421, ":Unknown command");
}

static void disconnect_client(irc_client_t *client, const char *reason)
{
    recipient_t recipients[IRC_MAX_USERS];
    size_t recipient_count = 0;
    char line[IRC_MAX_LINE] = "";
    lock_state();
    client->closing = true;
    if (client->registered) {
        char pfx[80]; prefix(client, pfx, sizeof(pfx));
        snprintf(line, sizeof(line), ":%s QUIT :%s", pfx, reason);
        recipient_count = snapshot_shared_recipients_locked(recipients, client, false);
    }
    for (int c = 0; c < IRC_MAX_CHANNELS; ++c) if (client->joined[c]) {
        client->joined[c] = false;
    }
    unlock_state();
    for (size_t i = 0; i < recipient_count; ++i) send_recipient(&recipients[i], line);

    while (true) {
        lock_state();
        unsigned pending_sends = client->pending_sends;
        int socket = client->socket;
        SemaphoreHandle_t tx_lock = client->tx_lock;
        unlock_state();
        if (pending_sends == 0) {
            xSemaphoreTake(tx_lock, portMAX_DELAY);
#if CONFIG_IRC_TLS_ENABLED
            if (client->use_tls && client->tls_initialized)
                mbedtls_ssl_close_notify(&client->tls_ssl);
#endif
            shutdown(socket, SHUT_RDWR);
            close(socket);
#if CONFIG_IRC_TLS_ENABLED
            if (client->use_tls && client->tls_initialized) {
                client->tls_net.fd = -1; /* The socket was closed above. */
                mbedtls_ssl_free(&client->tls_ssl);
                mbedtls_net_free(&client->tls_net);
                client->tls_initialized = false;
            }
#endif
            xSemaphoreGive(tx_lock);
            lock_state();
            memset(client, 0, sizeof(*client));
            client->socket = -1;
            client->tx_lock = tx_lock;
            unlock_state();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

#if CONFIG_IRC_TLS_ENABLED
static void log_tls_error(const char *operation, int error)
{
    char detail[128];
    mbedtls_strerror(error, detail, sizeof(detail));
    ESP_LOGE(TAG, "%s failed: -0x%04x (%s)", operation, (unsigned)-error,
             detail);
}

#if MBEDTLS_VERSION_MAJOR < 4
static int tls_random_bytes(void *context, unsigned char *output, size_t length)
{
    (void)context;
    esp_fill_random(output, length);
    return 0;
}
#endif

static bool tls_server_init(void)
{
#if MBEDTLS_VERSION_MAJOR >= 4
    psa_status_t psa_result = psa_crypto_init();
    if (psa_result != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PSA crypto initialization failed: %ld",
                 (long)psa_result);
        return false;
    }
#endif
    mbedtls_ssl_config_init(&tls_config);
    mbedtls_x509_crt_init(&tls_certificate);
    mbedtls_pk_init(&tls_private_key);

    int result = mbedtls_x509_crt_parse(
        &tls_certificate, irc_tls_certificate_pem_start,
        (size_t)(irc_tls_certificate_pem_end - irc_tls_certificate_pem_start));
    if (result != 0) { log_tls_error("TLS certificate parsing", result); return false; }

    result = mbedtls_pk_parse_key(
        &tls_private_key, irc_tls_private_key_pem_start,
        (size_t)(irc_tls_private_key_pem_end - irc_tls_private_key_pem_start),
#if MBEDTLS_VERSION_MAJOR >= 4
        NULL, 0);
#else
        NULL, 0, tls_random_bytes, NULL);
#endif
    if (result != 0) { log_tls_error("TLS private key parsing", result); return false; }

    result = mbedtls_ssl_config_defaults(&tls_config, MBEDTLS_SSL_IS_SERVER,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) { log_tls_error("TLS configuration", result); return false; }
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&tls_config, tls_random_bytes, NULL);
#endif
    mbedtls_ssl_conf_min_tls_version(&tls_config, MBEDTLS_SSL_VERSION_TLS1_2);
    result = mbedtls_ssl_conf_own_cert(&tls_config, &tls_certificate,
                                      &tls_private_key);
    if (result != 0) { log_tls_error("TLS certificate configuration", result); return false; }
    return true;
}

static bool tls_client_handshake(irc_client_t *client)
{
    mbedtls_net_init(&client->tls_net);
    mbedtls_ssl_init(&client->tls_ssl);
    client->tls_net.fd = client->socket;
    client->tls_initialized = true;

    int result = mbedtls_ssl_setup(&client->tls_ssl, &tls_config);
    if (result != 0) { log_tls_error("TLS session setup", result); return false; }
    mbedtls_ssl_set_bio(&client->tls_ssl, &client->tls_net, mbedtls_net_send,
                        mbedtls_net_recv, NULL);

    int flags = fcntl(client->socket, F_GETFL, 0);
    if (flags < 0 || fcntl(client->socket, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "Cannot make TLS client socket nonblocking: errno %d", errno);
        return false;
    }

    int64_t deadline = esp_timer_get_time() +
                       (int64_t)CONFIG_IRC_TLS_HANDSHAKE_TIMEOUT_MS * 1000;
    do {
        result = mbedtls_ssl_handshake(&client->tls_ssl);
        if (result == 0) return true;
        if (result != MBEDTLS_ERR_SSL_WANT_READ &&
            result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_tls_error("TLS handshake", result);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline);

    ESP_LOGW(TAG, "TLS handshake timed out");
    return false;
}
#endif

static void irc_client_task(void *parameter)
{
    irc_client_t *client = parameter;
#if CONFIG_IRC_TLS_ENABLED
    if (client->use_tls && !tls_client_handshake(client)) {
        disconnect_client(client, "TLS handshake failed");
        vTaskDelete(NULL);
        return;
    }
#endif
    char input[IRC_MAX_LINE + 1]; size_t used = 0;
    while (true) {
        xSemaphoreTake(client->tx_lock, portMAX_DELAY);
        int received = transport_read(client, input + used, IRC_MAX_LINE - used);
        xSemaphoreGive(client->tx_lock);
#if CONFIG_IRC_TLS_ENABLED
        if (client->use_tls && received == -EAGAIN) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
#endif
        if (received <= 0) break;
        used += (size_t)received; input[used] = '\0';
        char *start = input;
        while (true) {
            char *newline = memchr(start, '\n', used - (size_t)(start - input));
            if (!newline) break;
            *newline = '\0'; if (newline > start && newline[-1] == '\r') newline[-1] = '\0';
            ESP_LOGD(TAG, "< %s", start); handle_command(client, start); start = newline + 1;
        }
        size_t remaining = used - (size_t)(start - input);
        memmove(input, start, remaining); used = remaining;
        if (used == IRC_MAX_LINE) { reply(client, 417, ":Input line was too long"); break; }
    }
    disconnect_client(client, "Connection closed");
    vTaskDelete(NULL);
}

static int create_listener(uint16_t port)
{
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener < 0) return -1;
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, IRC_MAX_USERS) != 0) {
        close(listener);
        return -1;
    }
    return listener;
}

void irc_server_task(void *parameter)
{
    (void)parameter;
    state_lock = xSemaphoreCreateMutex();
    if (!state_lock) { ESP_LOGE(TAG, "Cannot allocate state mutex"); vTaskDelete(NULL); return; }
    for (int i = 0; i < IRC_MAX_USERS; ++i) {
        clients[i].socket = -1;
        clients[i].tx_lock = xSemaphoreCreateMutex();
        if (!clients[i].tx_lock) {
            ESP_LOGE(TAG, "Cannot allocate client transmit mutex");
            vTaskDelete(NULL);
            return;
        }
    }

#if CONFIG_IRC_TLS_ENABLED
    if (!tls_server_init()) { vTaskDelete(NULL); return; }
#endif

    int listen_socket = create_listener(PORT);
    if (listen_socket < 0) {
        ESP_LOGE(TAG, "Plaintext bind/listen on port %d failed: errno %d",
                 PORT, errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Plaintext IRC listening on port %d", PORT);
#if CONFIG_IRC_TLS_ENABLED
    int tls_listen_socket = create_listener(CONFIG_IRC_TLS_PORT);
    if (tls_listen_socket < 0) {
        ESP_LOGE(TAG, "TLS bind/listen on port %d failed: errno %d",
                 CONFIG_IRC_TLS_PORT, errno);
        close(listen_socket);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TLS IRC listening on port %d", CONFIG_IRC_TLS_PORT);
#endif

    while (true) {
        fd_set listeners;
        FD_ZERO(&listeners);
        FD_SET(listen_socket, &listeners);
        int highest_socket = listen_socket;
#if CONFIG_IRC_TLS_ENABLED
        FD_SET(tls_listen_socket, &listeners);
        if (tls_listen_socket > highest_socket) highest_socket = tls_listen_socket;
#endif
        int selected = select(highest_socket + 1, &listeners, NULL, NULL, NULL);
        if (selected < 0) {
            ESP_LOGE(TAG, "listener select failed: errno %d", errno);
            continue;
        }

        int ready_listener = listen_socket;
        bool connection_uses_tls = false;
#if CONFIG_IRC_TLS_ENABLED
        if (FD_ISSET(tls_listen_socket, &listeners)) {
            ready_listener = tls_listen_socket;
            connection_uses_tls = true;
        }
#endif
        struct sockaddr_in source; socklen_t length = sizeof(source);
        int socket_fd = accept(ready_listener, (struct sockaddr *)&source, &length);
        if (socket_fd < 0) { ESP_LOGE(TAG, "accept failed: errno %d", errno); continue; }
        int keepalive = 1, idle = KEEPALIVE_IDLE, interval = KEEPALIVE_INTERVAL, count = KEEPALIVE_COUNT;
        setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
        setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));

        lock_state(); irc_client_t *client = NULL;
        for (int i = 0; i < IRC_MAX_USERS; ++i) if (!clients[i].used) {
            client = &clients[i];
            SemaphoreHandle_t tx_lock = client->tx_lock;
            memset(client, 0, sizeof(*client));
            client->tx_lock = tx_lock;
            client->used = true;
            client->socket = socket_fd;
#if CONFIG_IRC_TLS_ENABLED
            client->use_tls = connection_uses_tls;
#endif
            break;
        }
        unlock_state();
        if (!client || xTaskCreatePinnedToCore(irc_client_task, "irc_client",
                                               IRC_CLIENT_STACK_SIZE, client,
                                               IRC_CLIENT_PRIORITY, NULL, 0) != pdPASS) {
            if (client) {
                lock_state();
                SemaphoreHandle_t tx_lock = client->tx_lock;
                memset(client, 0, sizeof(*client));
                client->socket = -1;
                client->tx_lock = tx_lock;
                unlock_state();
            }
            if (!connection_uses_tls)
                send(socket_fd, "ERROR :Server is full\r\n", 23, 0);
            close(socket_fd);
        } else {
            char address_text[INET_ADDRSTRLEN];
            inet_ntoa_r(source.sin_addr, address_text, sizeof(address_text));
            ESP_LOGI(TAG, "%s client connected from %s",
                     connection_uses_tls ? "TLS" : "Plaintext", address_text);
        }
    }
}
