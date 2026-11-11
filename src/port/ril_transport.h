/*
 * Internal transport interface shared by ril_port.c and ril_transport.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "iot_usbh_cdc.h"

#include "esp32_ril.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp32_ril_transport_init(const esp32_ril_config_t *config);
esp_err_t esp32_ril_transport_deinit(void);

/** Install the shared USB CDC driver if it is not installed yet. */
esp_err_t esp32_usb_cdc_ensure_driver(void);

/**
 * Create/delete a USB CDC device handle. All creations and deletions are
 * serialized because the reference-ril transport and the PPP task may race.
 */
esp_err_t esp32_usb_cdc_create_device(int itf_num, size_t rx_buffer_size,
                                     size_t tx_buffer_size,
                                     usbh_cdc_handle_t *handle);
esp_err_t esp32_usb_cdc_delete_device(usbh_cdc_handle_t handle);

/** Pre-claimed PPP (Modem interface) handle, NULL until it is open. */
usbh_cdc_handle_t esp32_usb_cdc_ppp_handle(void);

/**
 * Send a raw AT command on the USB CDC handle and collect the response.
 * Only valid before reference-ril starts (exclusive access).
 *
 * @return 0 on OK, -1 on timeout/error
 */
int esp32_ril_transport_at_command(const char *cmd, char *resp,
                                  size_t resp_size, uint32_t timeout_ms);

/** Configured AT USB interface number. */
int esp32_ril_transport_get_itf(void);

/** Non-blocking read from a CDC handle; returns bytes read or 0. */
int esp32_usb_cdc_read_available(usbh_cdc_handle_t handle, uint8_t *buf,
                                size_t cap);

/** Wait until a USB modem is present on the configured AT interface. */
bool esp32_ril_transport_wait_ready(uint32_t timeout_ms);
bool esp32_ril_transport_is_connected(void);

/** VFS path used as the reference-ril AT device ("-d" argument). */
const char *esp32_ril_transport_path(void);

#ifdef __cplusplus
}
#endif
