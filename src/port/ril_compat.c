/*
 * Android compatibility helpers needed by reference-ril when built for
 * ESP-IDF: a tiny property store, sockets helpers and librilutils entry
 * points.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "netdb.h"

#include "cutils/properties.h"
#include "cutils/sockets.h"
#include "sys/system_properties.h"
#include "telephony/librilutils.h"

static const char *TAG = "esp32_ril_compat";

/* ------------------------------------------------------------------------- */
/* Minimal Android property store                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    char key[PROPERTY_KEY_MAX];
    char value[PROPERTY_VALUE_MAX];
} property_entry_t;

static property_entry_t s_properties[8];

static property_entry_t *property_find(const char *key, bool create)
{
    if (key == NULL || key[0] == '\0') {
        return NULL;
    }

    property_entry_t *free_slot = NULL;
    for (size_t i = 0; i < sizeof(s_properties) / sizeof(s_properties[0]); i++) {
        if (s_properties[i].key[0] == '\0') {
            if (free_slot == NULL) {
                free_slot = &s_properties[i];
            }
        } else if (strncmp(s_properties[i].key, key, PROPERTY_KEY_MAX - 1) == 0) {
            return &s_properties[i];
        }
    }

    if (!create || free_slot == NULL) {
        return NULL;
    }

    strncpy(free_slot->key, key, PROPERTY_KEY_MAX - 1);
    free_slot->key[PROPERTY_KEY_MAX - 1] = '\0';
    free_slot->value[0] = '\0';
    return free_slot;
}

int property_get(const char *key, char *value, const char *default_value)
{
    if (value == NULL) {
        return 0;
    }

    property_entry_t *entry = property_find(key, false);
    const char *src = (entry != NULL && entry->value[0] != '\0') ? entry->value : default_value;
    if (src == NULL) {
        value[0] = '\0';
        return 0;
    }

    strncpy(value, src, PROPERTY_VALUE_MAX - 1);
    value[PROPERTY_VALUE_MAX - 1] = '\0';
    return (int)strlen(value);
}

int property_set(const char *key, const char *value)
{
    property_entry_t *entry = property_find(key, true);
    if (entry == NULL) {
        return -1;
    }

    strncpy(entry->value, value ? value : "", PROPERTY_VALUE_MAX - 1);
    entry->value[PROPERTY_VALUE_MAX - 1] = '\0';
    return 0;
}

int property_get_bool(const char *key, int default_value)
{
    char value[PROPERTY_VALUE_MAX];
    if (property_get(key, value, "") == 0) {
        return default_value;
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "on") == 0) {
        return 1;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "off") == 0) {
        return 0;
    }
    return default_value;
}

int32_t property_get_int32(const char *key, int32_t default_value)
{
    char value[PROPERTY_VALUE_MAX];
    if (property_get(key, value, "") == 0) {
        return default_value;
    }
    return (int32_t)strtol(value, NULL, 0);
}

int64_t property_get_int64(const char *key, int64_t default_value)
{
    char value[PROPERTY_VALUE_MAX];
    if (property_get(key, value, "") == 0) {
        return default_value;
    }
    return (int64_t)strtoll(value, NULL, 0);
}

int __system_property_get(const char *key, char *value)
{
    return property_get(key, value, "");
}

int __system_property_set(const char *key, const char *value)
{
    return property_set(key, value);
}

/* ------------------------------------------------------------------------- */
/* Android socket helpers                                                    */
/* ------------------------------------------------------------------------- */

int socket_local_client(const char *name, int namespaceId, int type)
{
    (void)name;
    (void)namespaceId;
    (void)type;
    /* lwIP has no AF_UNIX support; reference-ril only uses this for
       "-s <socket>" which is not used on the ESP32-P4 port. */
    errno = EAFNOSUPPORT;
    return -1;
}

int socket_network_client(const char *host, int port, int type)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char port_str[16];
    int fd = -1;

    if (host == NULL || port <= 0) {
        errno = EINVAL;
        return -1;
    }

    snprintf(port_str, sizeof(port_str), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = type;

    if (getaddrinfo(host, port_str, &hints, &result) != 0 || result == NULL) {
        errno = EHOSTUNREACH;
        return -1;
    }

    for (struct addrinfo *it = result; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

int socket_network_client_timeout(const char *host, int port, int type, int timeout_secs)
{
    (void)timeout_secs;
    return socket_network_client(host, port, type);
}

/* ------------------------------------------------------------------------- */
/* librilutils                                                               */
/* ------------------------------------------------------------------------- */

uint64_t ril_nano_time(void)
{
    return (uint64_t)esp_timer_get_time() * 1000ULL;
}

/* ------------------------------------------------------------------------- */
/* Request names (provided by libril on Android)                             */
/* ------------------------------------------------------------------------- */

const char *requestToString(int request)
{
    static char buf[24];
    snprintf(buf, sizeof(buf), "RIL_REQUEST_%d", request);
    return buf;
}
