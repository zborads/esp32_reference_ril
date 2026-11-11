/*
 * esp32_modem.h
 *
 * Hardware bring-up ("mounting") of the cellular module, equivalent to the
 * power/reset handling that the former iot_usbh_modem component did before
 * it initialized its USB DTE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Configure M_PWRKEY (idle high) and M_VBAT_EN (active high = on). */
void esp32_modem_hw_init(void);

/** Power-cycle the module through M_VBAT_EN (low -> high). */
void esp32_modem_reset_pulse(void);

/** Press M_PWRKEY (high -> low 500 ms -> high) and wait for the module boot. */
void esp32_modem_power_on(void);

/** Turn the module power off through M_VBAT_EN. */
void esp32_modem_power_off(void);

/** Mark the modem bring-up finished (called by the bring-up task). */
void esp32_modem_set_ready(bool ready);

/** Wait until the modem bring-up finished. */
bool esp32_modem_wait_ready(uint32_t timeout_ms);

/** Human readable MCU reset reason, for the boot log. */
const char *esp32_modem_reset_reason_str(void);

#ifdef __cplusplus
}
#endif
