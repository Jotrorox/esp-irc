#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include <lwip/netdb.h>

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "irc_server.h"
#include "message_store.h"

#define IRC_MAX_USERS          16
#define IRC_MAX_CHANNELS       8
#define IRC_MAX_LINE           512
#define IRC_NICK_LEN           24
#define IRC_USER_LEN           24
#define IRC_REALNAME_LEN       48
#define IRC_CHANNEL_LEN        32
#define IRC_CLIENT_STACK_SIZE  6144
#define IRC_CLIENT_PRIORITY    5
#define IRC_SERVER_NAME        "esp-irc"

typedef struct irc_client {
    int socket;
    bool used;
    bool registered;
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

static void lock_state(void) { xSemaphoreTake(state_lock, portMAX_DELAY); }
static void unlock_state(void) { xSemaphoreGive(state_lock); }

uint32_t irc_server_get_user_count(void)
{
    uint32_t count = 0;
    if (state_lock == NULL) return 0;
    lock_state();
    for (int i = 0; i < IRC_MAX_USERS; ++i) count += clients[i].used;
    unlock_state();
    return count;
}

static bool send_all(int socket, const char *data, size_t length)
{
    while (length > 0) {
        int sent = send(socket, data, length, 0);
        if (sent <= 0) return false;
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool send_line(irc_client_t *client, const char *line)
{
    char output[IRC_MAX_LINE + 3];
    size_t length = strnlen(line, IRC_MAX_LINE);
    memcpy(output, line, length);
    output[length++] = '\r';
    output[length++] = '\n';
    return send_all(client->socket, output, length);
}

static void reply(irc_client_t *client, int numeric, const char *text)
{
    char line[IRC_MAX_LINE];
    snprintf(line, sizeof(line), ":%s %03d %s %s", IRC_SERVER_NAME, numeric,
             client->nick[0] ? client->nick : "*", text);
    send_line(client, line);
}

static void prefix(const irc_client_t *client, char *out, size_t size)
{
    snprintf(out, size, "%s!%s@esp.local", client->nick,
             client->user[0] ? client->user : "unknown");
}

/* Caller holds state_lock. */
static void broadcast_locked(const char *line, const irc_client_t *except,
                             int channel_index)
{
    for (int i = 0; i < IRC_MAX_USERS; ++i) {
        irc_client_t *target = &clients[i];
        if (!target->used || target == except) continue;
        if (channel_index >= 0 && !target->joined[channel_index]) continue;
        send_line(target, line);
    }
}

static bool valid_nick(const char *nick)
{
    if (!nick[0] || !(isalpha((unsigned char)nick[0]) || strchr("[]\\`_^{|}", nick[0])))
        return false;
    for (size_t i = 1; nick[i]; ++i)
        if (!isalnum((unsigned char)nick[i]) && !strchr("-[]\\`_^{|}", nick[i])) return false;
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

static void complete_registration(irc_client_t *client)
{
    if (client->registered || !client->nick[0] || !client->user[0]) return;
    client->registered = true;
    char text[IRC_MAX_LINE];
    snprintf(text, sizeof(text), ":Welcome to ESP IRC, %s!%s@esp.local", client->nick, client->user);
    reply(client, 1, text);
    reply(client, 2, ":Your host is esp-irc, running version 1.0");
    reply(client, 3, ":This server was created for ESP-IDF");
    reply(client, 4, "esp-irc 1.0 io nt");
    reply(client, 5, "CHANTYPES=# NICKLEN=23 CHANNELLEN=31 CASEMAPPING=ascii NETWORK=ESPIRC :are supported by this server");
    reply(client, 375, ":- esp-irc Message of the Day -");
    reply(client, 372, ":- A tiny IRC server running on an ESP32.");
    reply(client, 376, ":End of /MOTD command");
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
    if (client->registered) prefix(client, old_prefix, sizeof(old_prefix));
    strlcpy(client->nick, nick, sizeof(client->nick));
    if (old_prefix[0]) {
        char line[IRC_MAX_LINE];
        snprintf(line, sizeof(line), ":%s NICK :%s", old_prefix, client->nick);
        broadcast_locked(line, NULL, -1);
    }
    unlock_state();
    complete_registration(client);
}

static void handle_join(irc_client_t *client, char *name)
{
    if (!name || name[0] != '#' || strlen(name) >= IRC_CHANNEL_LEN) {
        reply(client, 403, "* :No such channel"); return;
    }
    char *comma = strchr(name, ','); if (comma) *comma = '\0';
    lock_state();
    int index = find_channel_locked(name);
    if (index < 0) {
        for (int i = 0; i < IRC_MAX_CHANNELS; ++i) if (!channels[i].used) {
            index = i; channels[i].used = true; strlcpy(channels[i].name, name, sizeof(channels[i].name)); break;
        }
    }
    if (index < 0) { unlock_state(); reply(client, 405, ":You have joined too many channels"); return; }
    client->joined[index] = true;
    char pfx[80], line[IRC_MAX_LINE], topic[96], channel[IRC_CHANNEL_LEN];
    prefix(client, pfx, sizeof(pfx));
    strlcpy(channel, channels[index].name, sizeof(channel));
    strlcpy(topic, channels[index].topic, sizeof(topic));
    snprintf(line, sizeof(line), ":%s JOIN :%s", pfx, channel);
    broadcast_locked(line, NULL, index);
    unlock_state();
    if (topic[0]) { snprintf(line, sizeof(line), "%s :%s", channel, topic); reply(client, 332, line); }
    else { snprintf(line, sizeof(line), "%s :No topic is set", channel); reply(client, 331, line); }
    send_names(client, index);
}

static void handle_part(irc_client_t *client, char *name, char *reason)
{
    lock_state();
    int index = name ? find_channel_locked(name) : -1;
    if (index < 0) { unlock_state(); reply(client, 403, "* :No such channel"); return; }
    if (!client->joined[index]) { unlock_state(); reply(client, 442, "* :You're not on that channel"); return; }
    char pfx[80], line[IRC_MAX_LINE]; prefix(client, pfx, sizeof(pfx));
    snprintf(line, sizeof(line), ":%s PART %s :%s", pfx, channels[index].name, reason ? reason : "Leaving");
    broadcast_locked(line, NULL, index);
    client->joined[index] = false;
    bool occupied = false;
    for (int i = 0; i < IRC_MAX_USERS; ++i) occupied |= clients[i].used && clients[i].joined[index];
    if (!occupied) memset(&channels[index], 0, sizeof(channels[index]));
    unlock_state();
}

static void handle_message(irc_client_t *client, char *target, char *message, bool notice)
{
    if (!target || !message) { if (!notice) reply(client, 461, "PRIVMSG :Not enough parameters"); return; }
    char pfx[80], line[IRC_MAX_LINE]; prefix(client, pfx, sizeof(pfx));
    snprintf(line, sizeof(line), ":%s %s %s :%s", pfx, notice ? "NOTICE" : "PRIVMSG", target, message);
    lock_state();
    if (target[0] == '#') {
        int index = find_channel_locked(target);
        if (index < 0 || !client->joined[index]) { unlock_state(); if (!notice) reply(client, 404, "* :Cannot send to channel"); return; }
        message_store_enqueue(notice ? MESSAGE_STORE_NOTICE : MESSAGE_STORE_PRIVMSG,
                              channels[index].name, client->nick, message);
        broadcast_locked(line, client, index);
    } else {
        irc_client_t *recipient = NULL;
        for (int i = 0; i < IRC_MAX_USERS; ++i)
            if (clients[i].used && !strcasecmp(clients[i].nick, target)) recipient = &clients[i];
        if (recipient) send_line(recipient, line);
        else if (!notice) { unlock_state(); reply(client, 401, "* :No such nick"); return; }
    }
    unlock_state();
}

static void handle_list(irc_client_t *client)
{
    reply(client, 321, "Channel :Users Name");
    lock_state();
    for (int c = 0; c < IRC_MAX_CHANNELS; ++c) if (channels[c].used) {
        int users = 0; for (int i = 0; i < IRC_MAX_USERS; ++i) users += clients[i].used && clients[i].joined[c];
        char text[IRC_MAX_LINE]; snprintf(text, sizeof(text), "%s %d :%s", channels[c].name, users, channels[c].topic);
        reply(client, 322, text);
    }
    unlock_state();
    reply(client, 323, ":End of /LIST");
}

static void handle_command(irc_client_t *client, char *line)
{
    char *save = NULL;
    char *command = strtok_r(line, " ", &save);
    if (!command) return;
    char *param = strtok_r(NULL, " ", &save);
    char *trailing = save;
    while (trailing && *trailing == ' ') trailing++;
    if (trailing && *trailing == ':') trailing++;

    if (!strcasecmp(command, "CAP")) {
        if (param && !strcasecmp(param, "LS")) send_line(client, ":esp-irc CAP * LS :");
        else if (param && !strcasecmp(param, "REQ")) send_line(client, ":esp-irc CAP * NAK :");
        return;
    }
    if (!strcasecmp(command, "NICK")) { handle_nick(client, param); return; }
    if (!strcasecmp(command, "USER")) {
        if (!param) { reply(client, 461, "USER :Not enough parameters"); return; }
        strlcpy(client->user, param, sizeof(client->user));
        char *colon = trailing ? strchr(trailing, ':') : NULL;
        strlcpy(client->realname, colon ? colon + 1 : (trailing ? trailing : param), sizeof(client->realname));
        complete_registration(client); return;
    }
    if (!strcasecmp(command, "PING")) {
        char response[IRC_MAX_LINE]; snprintf(response, sizeof(response), ":%s PONG %s :%s", IRC_SERVER_NAME, IRC_SERVER_NAME, param ? param : IRC_SERVER_NAME);
        send_line(client, response); return;
    }
    if (!strcasecmp(command, "PONG")) return;
    if (!strcasecmp(command, "QUIT")) { shutdown(client->socket, SHUT_RDWR); return; }
    if (!client->registered) { reply(client, 451, ":You have not registered"); return; }
    if (!strcasecmp(command, "JOIN")) { handle_join(client, param); return; }
    if (!strcasecmp(command, "PART")) { handle_part(client, param, trailing); return; }
    if (!strcasecmp(command, "PRIVMSG")) { handle_message(client, param, trailing, false); return; }
    if (!strcasecmp(command, "NOTICE")) { handle_message(client, param, trailing, true); return; }
    if (!strcasecmp(command, "LIST")) { handle_list(client); return; }
    if (!strcasecmp(command, "NAMES")) {
        lock_state(); int index = param ? find_channel_locked(param) : -1; unlock_state();
        if (index >= 0) send_names(client, index); else reply(client, 366, "* :End of /NAMES list"); return;
    }
    if (!strcasecmp(command, "MODE")) {
        char text[80]; snprintf(text, sizeof(text), "%s +nt", param ? param : client->nick); reply(client, 324, text); return;
    }
    if (!strcasecmp(command, "TOPIC")) {
        lock_state(); int index = param ? find_channel_locked(param) : -1;
        if (index >= 0 && trailing && *trailing) strlcpy(channels[index].topic, trailing, sizeof(channels[index].topic));
        char topic[96] = ""; if (index >= 0) strlcpy(topic, channels[index].topic, sizeof(topic)); unlock_state();
        if (index < 0) reply(client, 403, "* :No such channel"); else { char text[150]; snprintf(text, sizeof(text), "%s :%s", param, topic); reply(client, topic[0] ? 332 : 331, text); } return;
    }
    if (!strcasecmp(command, "WHO")) { reply(client, 315, "* :End of /WHO list"); return; }
    if (!strcasecmp(command, "WHOIS")) { reply(client, 401, "* :No such nick"); return; }
    if (!strcasecmp(command, "MOTD")) { reply(client, 372, ":- A tiny IRC server running on an ESP32."); reply(client, 376, ":End of /MOTD command"); return; }
    if (!strcasecmp(command, "VERSION")) { reply(client, 351, "esp-irc-1.0 esp-irc :ESP-IDF IRC server"); return; }
    reply(client, 421, ":Unknown command");
}

static void disconnect_client(irc_client_t *client, const char *reason)
{
    lock_state();
    if (client->registered) {
        char pfx[80], line[IRC_MAX_LINE]; prefix(client, pfx, sizeof(pfx));
        snprintf(line, sizeof(line), ":%s QUIT :%s", pfx, reason);
        broadcast_locked(line, client, -1);
    }
    for (int c = 0; c < IRC_MAX_CHANNELS; ++c) if (client->joined[c]) {
        bool occupied = false;
        for (int i = 0; i < IRC_MAX_USERS; ++i) occupied |= (&clients[i] != client && clients[i].used && clients[i].joined[c]);
        if (!occupied) memset(&channels[c], 0, sizeof(channels[c]));
    }
    int socket = client->socket;
    memset(client, 0, sizeof(*client)); client->socket = -1;
    unlock_state();
    shutdown(socket, SHUT_RDWR); close(socket);
}

static void irc_client_task(void *parameter)
{
    irc_client_t *client = parameter;
    char input[IRC_MAX_LINE + 1]; size_t used = 0;
    while (true) {
        int received = recv(client->socket, input + used, IRC_MAX_LINE - used, 0);
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

void irc_server_task(void *parameter)
{
    (void)parameter;
    state_lock = xSemaphoreCreateMutex();
    if (!state_lock) { ESP_LOGE(TAG, "Cannot allocate state mutex"); vTaskDelete(NULL); return; }
    for (int i = 0; i < IRC_MAX_USERS; ++i) clients[i].socket = -1;

    int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_socket < 0) { ESP_LOGE(TAG, "socket failed: errno %d", errno); vTaskDelete(NULL); return; }
    int yes = 1; setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(listen_socket, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listen_socket, IRC_MAX_USERS) != 0) {
        ESP_LOGE(TAG, "bind/listen failed: errno %d", errno); close(listen_socket); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "IRC server listening on port %d", PORT);

    while (true) {
        struct sockaddr_in source; socklen_t length = sizeof(source);
        int socket_fd = accept(listen_socket, (struct sockaddr *)&source, &length);
        if (socket_fd < 0) { ESP_LOGE(TAG, "accept failed: errno %d", errno); continue; }
        int keepalive = 1, idle = KEEPALIVE_IDLE, interval = KEEPALIVE_INTERVAL, count = KEEPALIVE_COUNT;
        setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
        setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));

        lock_state(); irc_client_t *client = NULL;
        for (int i = 0; i < IRC_MAX_USERS; ++i) if (!clients[i].used) { client = &clients[i]; memset(client, 0, sizeof(*client)); client->used = true; client->socket = socket_fd; break; }
        unlock_state();
        if (!client || xTaskCreate(irc_client_task, "irc_client", IRC_CLIENT_STACK_SIZE, client, IRC_CLIENT_PRIORITY, NULL) != pdPASS) {
            if (client) { lock_state(); memset(client, 0, sizeof(*client)); client->socket = -1; unlock_state(); }
            send_all(socket_fd, "ERROR :Server is full\r\n", 23); close(socket_fd);
        } else {
            char address_text[INET_ADDRSTRLEN];
            inet_ntoa_r(source.sin_addr, address_text, sizeof(address_text));
            ESP_LOGI(TAG, "Client connected from %s", address_text);
        }
    }
}
