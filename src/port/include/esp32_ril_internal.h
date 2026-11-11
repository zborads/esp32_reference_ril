/*
 * Private interface between the reference-ril engine and the ESP-IDF port.
 * Not part of the public component API.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Cached result of reference-ril's single AT+CPIN? query.
 *
 * @return  1  SIM is ready (+CPIN: READY)
 *          0  SIM is not usable (absent / locked / failure, or the query
 *             failed; +CME ERROR is decoded per the AT manual 12.5)
 *         -1  AT+CPIN? has not been executed yet
 */
int esp32_ril_sim_ready_cached(void);

#ifdef __cplusplus
}
#endif
