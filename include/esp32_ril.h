/*
 * esp32_ril.h
 *
 * Public interface of the reference-ril based cellular modem stack for the
 * esp32 ESP32-P4 firmware.
 *
 * reference-ril (hardware/ril/reference-ril from AOSP) is compiled into the
 * firmware together with a small porting layer:
 *
 *   - utils/Log.h, cutils/properties.h, cutils/sockets.h shims
 *   - a VFS character device ("/dev/esp32ril0") backed by the USB CDC-ACM
 *     AT channel (espressif/iot_usbh_cdc)
 *   - a RIL_Env implementation that runs RIL timed callbacks on a FreeRTOS
 *     task
 *
 * The application talks to the modem through this API instead of the former
 * espressif/iot_usbh_modem component.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Event base used by esp32_ril_init()/esp32_ril_deinit() notifications. */
ESP_EVENT_DECLARE_BASE(ESP32_RIL_EVENT);

typedef enum {
    ESP32_RIL_EVENT_DTE_CONN = 0,    /*!< USB modem (AT channel) connected      */
    ESP32_RIL_EVENT_DTE_DISCONN,     /*!< USB modem (AT channel) disconnected   */
    ESP32_RIL_EVENT_SIM_CONN,        /*!< SIM card ready                        */
    ESP32_RIL_EVENT_SIM_DISCONN,     /*!< SIM card absent / not ready           */
    ESP32_RIL_EVENT_NET_CONN,        /*!< PPP data connection got an IP         */
    ESP32_RIL_EVENT_NET_DISCONN,     /*!< PPP data connection lost              */
} esp32_ril_event_t;

/** Default USB interface number carrying the modem AT channel. */
#define ESP32_RIL_DEFAULT_AT_ITF         3

/**
 * @brief esp32_ril configuration
 */
typedef struct {
    int usb_itf_num;                /*!< USB CDC interface used as AT channel   */
    size_t rx_buffer_size;          /*!< USB CDC RX ring buffer size            */
    size_t tx_buffer_size;          /*!< USB CDC TX ring buffer size            */
    esp_event_handler_t handler;    /*!< Event handler for ESP32_RIL_EVENT       */
    void *handler_arg;              /*!< User argument passed to the handler    */
} esp32_ril_config_t;

#define ESP32_RIL_DEFAULT_CONFIG()                       \
    {                                                   \
        .usb_itf_num = ESP32_RIL_DEFAULT_AT_ITF,         \
        .rx_buffer_size = 8 * 1024,                     \
        .tx_buffer_size = 4 * 1024,                     \
        .handler = NULL,                                \
        .handler_arg = NULL,                            \
    }

/**
 * @brief Start the USB AT transport and the reference-ril AT/RIL engine.
 *
 * The default esp_event loop must already exist (esp_event_loop_create_default).
 *
 * @param config configuration, NULL to use ESP32_RIL_DEFAULT_CONFIG()
 * @return ESP_OK on success
 */
esp_err_t esp32_ril_init(const esp32_ril_config_t *config);

/**
 * @brief Stop reference-ril and release the USB CDC transport.
 */
esp_err_t esp32_ril_deinit(void);

/**
 * @brief Query signal strength (AT+CSQ).
 *
 * @param rssi 2..30 for -109..-53 dBm, 99 when unknown
 * @param ber  channel bit error rate, 99 when unknown
 */
esp_err_t esp32_ril_get_signal_quality(int *rssi, int *ber);

/**
 * @brief Read the SIM ICCID (AT+CCID / AT+QCCID / AT+CICCID).
 *
 * @param buf     destination buffer
 * @param buf_size destination buffer size
 */
esp_err_t esp32_ril_get_iccid(char *buf, size_t buf_size);

/**
 * @brief Read the operator name (AT+COPS?).
 */
esp_err_t esp32_ril_get_operator(char *buf, size_t buf_size);

/**
 * @brief Check whether the SIM is ready.
 *
 * Returns the cached result of reference-ril's single AT+CPIN? query, which is
 * executed once after the radio is switched on. This function never sends AT
 * commands.
 *
 * @param ready set to true when +CPIN? reported READY
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE when the
 *         AT+CPIN? query has not completed yet
 */
esp_err_t esp32_ril_get_sim_state(bool *ready);

/**
 * @brief Read the 3GPP registration state (AT+CREG?).
 *
 * @param stat 0 not registered, 1 registered home, 2 searching,
 *             3 denied, 4 unknown, 5 registered roaming
 */
esp_err_t esp32_ril_get_registration_state(int *stat);

/**
 * @brief Whether the USB AT channel is currently connected.
 */
bool esp32_ril_is_connected(void);

#ifdef __cplusplus
}
#endif
