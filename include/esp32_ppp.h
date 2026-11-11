/*
 * esp32_ppp.h
 *
 * PPP data connection over the modem's USB CDC data interface.
 *
 * The cellular module used by this board exposes (at least) two CDC-ACM
 * interfaces:
 *
 *   - one AT command port (default 3) used by reference-ril
 *   - one AT + PPP data port (default 4) used by this module
 *
 * esp32_ppp dials the modem on the data port, attaches a lwIP PPP netif and
 * bridges the USB CDC byte stream to it. On success ESP32_RIL_EVENT_NET_CONN
 * is posted; on loss ESP32_RIL_EVENT_NET_DISCONN is posted and the connection
 * is re-dialed automatically.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP32_PPP_DEFAULT_APN       "cmnee"
#define ESP32_PPP_DEFAULT_DIAL_CMD  "ATD*99***1#"
#define ESP32_PPP_DEFAULT_RETRY_MS  5000

typedef struct {
    int usb_itf_num;        /*!< USB CDC interface used for PPP data        */
    const char *apn;        /*!< APN, NULL to use ESP32_PPP_DEFAULT_APN      */
    const char *username;   /*!< optional PAP/CHAP user name                */
    const char *password;   /*!< optional PAP/CHAP password                 */
    const char *dial_cmd;   /*!< dial command, NULL for ATD*99***1#         */
    uint32_t retry_ms;      /*!< delay between dial attempts                */
} esp32_ppp_config_t;

#define ESP32_PPP_DEFAULT_CONFIG()                      \
    {                                                   \
        .usb_itf_num = 4,                               \
        .apn = ESP32_PPP_DEFAULT_APN,                    \
        .username = NULL,                               \
        .password = NULL,                               \
        .dial_cmd = ESP32_PPP_DEFAULT_DIAL_CMD,          \
        .retry_ms = ESP32_PPP_DEFAULT_RETRY_MS,          \
    }

/**
 * @brief Start the PPP supervisor task.
 *
 * Requires the USB CDC driver to be available; esp32_ril_init() normally
 * installs it. The data interface must be different from the reference-ril
 * AT interface.
 */
esp_err_t esp32_ppp_start(const esp32_ppp_config_t *config);

/**
 * @brief Stop PPP, hang up the data call and release the netif.
 */
esp_err_t esp32_ppp_stop(void);

/** Whether a PPP session currently has an IP address. */
bool esp32_ppp_is_up(void);

/** Whether the PPP supervisor was started. */
bool esp32_ppp_is_started(void);

#ifdef __cplusplus
}
#endif
