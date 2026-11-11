/*
 * PPP data connection over the modem's USB CDC data interface.
 *
 * This is intentionally independent from reference-ril: reference-ril keeps
 * using the module's AT command interface (default USB interface 3) while
 * this module owns the AT+PPP data interface (default 4). This mirrors the
 * "secondary AT port" setup of the former iot_usbh_modem component.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ppp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "iot_usbh_cdc.h"

#include "esp32_modem.h"
#include "esp32_ppp.h"
#include "esp32_ril.h"
#include "ril_transport.h"

static const char *TAG = "esp32_ppp";

#define ESP32_PPP_RX_BUFFER_SIZE   8192
#define ESP32_PPP_TX_BUFFER_SIZE   4096
#define ESP32_PPP_RX_CHUNK         1500
#define ESP32_PPP_AT_BUFFER_SIZE   1024
#define ESP32_PPP_ATD_TIMEOUT_MS   30000
#define ESP32_PPP_DONE_BIT         BIT0

typedef struct {
    esp_netif_driver_base_t base;
    usbh_cdc_handle_t cdc;
    volatile bool running;
} esp32_ppp_driver_t;

static esp32_ppp_config_t s_cfg;
static esp32_ppp_driver_t *s_driver;
static esp_netif_t *s_netif;
static EventGroupHandle_t s_events;
static volatile bool s_started;
static volatile bool s_stop;
static volatile bool s_up;
static volatile bool s_lost_ip;

/* ------------------------------------------------------------------------- */
/* netif driver glue                                                         */
/* ------------------------------------------------------------------------- */

static esp_err_t ppp_transmit(void *h, void *buffer, size_t len)
{
    esp32_ppp_driver_t *drv = (esp32_ppp_driver_t *)h;
    if (drv == NULL || drv->cdc == NULL || !drv->running) {
        return ESP_FAIL;
    }

    const uint8_t *data = (const uint8_t *)buffer;
    size_t written = 0;
    while (written < len) {
        size_t chunk = len - written;
        esp_err_t err = usbh_cdc_write_bytes(drv->cdc, data + written, chunk,
                                             pdMS_TO_TICKS(200));
        if (err != ESP_OK || chunk == 0) {
            return ESP_FAIL;
        }
        written += chunk;
    }
    return ESP_OK;
}

static esp_err_t ppp_post_attach(esp_netif_t *netif, void *args)
{
    esp32_ppp_driver_t *drv = (esp32_ppp_driver_t *)args;
    if (drv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    drv->base.netif = netif;
    const esp_netif_driver_ifconfig_t ifconfig = {
        .handle = drv,
        .transmit = ppp_transmit,
        .driver_free_rx_buffer = NULL,
    };
    return esp_netif_set_driver_config(netif, &ifconfig);
}

static void ppp_status_event_handler(void *arg, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;

    /* Error codes come before the phase offset. NETIF_PPP_ERRORNONE (0) is
       posted on a healthy connect and must not tear the link down. */
    if (event_id > NETIF_PPP_ERRORNONE &&
        event_id < NETIF_PP_PHASE_OFFSET) {
        ESP_LOGW(TAG, "PPP error event %" PRIi32, event_id);
        s_lost_ip = true;
    }
}

static void ppp_ip_event_handler(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;

    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "PPP connected, IP " IPSTR, IP2STR(&event->ip_info.ip));

        esp_netif_set_default_netif(s_netif);

        esp_netif_dns_info_t dns = {0};
        if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.u_addr.ip4.addr == 0) {
            ESP_LOGW(TAG, "No DNS from peer, using 8.8.8.8");
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = esp_netif_ip4_makeu32(8, 8, 8, 8);
            esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
        }

        s_up = true;
        esp_event_post(ESP32_RIL_EVENT, ESP32_RIL_EVENT_NET_CONN, NULL, 0,
                       pdMS_TO_TICKS(100));
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP lost IP");
        s_up = false;
        s_lost_ip = true;
        esp_event_post(ESP32_RIL_EVENT, ESP32_RIL_EVENT_NET_DISCONN, NULL, 0,
                       pdMS_TO_TICKS(100));
    }
}

/* ------------------------------------------------------------------------- */
/* USB CDC helpers                                                           */
/* ------------------------------------------------------------------------- */

static bool cdc_is_open(usbh_cdc_handle_t cdc)
{
    usbh_cdc_state_t state = USBH_CDC_CLOSE;
    if (cdc == NULL) {
        return false;
    }
    return usbh_cdc_get_state(cdc, &state) == ESP_OK && state == USBH_CDC_OPEN;
}

static bool ppp_open_cdc(usbh_cdc_handle_t *out)
{
    /* The Modem-interface handle was pre-claimed by the transport before the
       module finished booting; wait until the USB host opens it. */
    usbh_cdc_handle_t cdc = esp32_usb_cdc_ppp_handle();
    if (cdc == NULL) {
        return false;
    }

    *out = cdc;
    return true;
}

static int cdc_write_raw(usbh_cdc_handle_t cdc, const void *data, size_t len)
{
    esp_err_t err = usbh_cdc_write_bytes(cdc, (const uint8_t *)data, len,
                                         pdMS_TO_TICKS(200));
    return (err == ESP_OK) ? 0 : -1;
}

static int cdc_send_command(usbh_cdc_handle_t cdc, const char *cmd)
{
    if (cdc_write_raw(cdc, cmd, strlen(cmd)) != 0) {
        return -1;
    }
    return cdc_write_raw(cdc, "\r", 1);
}

/*
 * Send an AT command and wait for "expect". Bytes following the match are
 * returned in leftover (needed because the modem can start sending PPP
 * frames right after "CONNECT").
 */
static esp_err_t ppp_expect(usbh_cdc_handle_t cdc, const char *cmd,
                            const char *expect, int timeout_ms,
                            uint8_t *leftover, size_t *leftover_len)
{
    if (leftover_len != NULL) {
        *leftover_len = 0;
    }

    if (cmd != NULL) {
        usbh_cdc_flush_rx_buffer(cdc);
        if (cdc_send_command(cdc, cmd) != 0) {
            return ESP_FAIL;
        }
    }

    char buffer[ESP32_PPP_AT_BUFFER_SIZE];
    size_t used = 0;
    buffer[0] = '\0';

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (s_stop) {
            return ESP_FAIL;
        }
        if (!cdc_is_open(cdc)) {
            return ESP_FAIL;
        }

        size_t want = sizeof(buffer) - 1 - used;
        if (want == 0) {
            /* Keep the tail of the buffer only. */
            memmove(buffer, buffer + used - 64, 64);
            used = 64;
            buffer[used] = '\0';
            want = sizeof(buffer) - 1 - used;
        }

        size_t length = want;
        int read = esp32_usb_cdc_read_available(cdc, (uint8_t *)buffer + used, length);
        if (read > 0) {
            used += (size_t)read;
            buffer[used] = '\0';

            char *match = strstr(buffer, expect);
            if (match != NULL) {
                char *rest = match + strlen(expect);
                while (*rest == '\r' || *rest == '\n') {
                    rest++;
                }
                size_t remain = used - (size_t)(rest - buffer);
                if (leftover != NULL && leftover_len != NULL && remain > 0) {
                    if (remain > 255) {
                        remain = 255;
                    }
                    memcpy(leftover, rest, remain);
                    *leftover_len = remain;
                }
                return ESP_OK;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t ppp_dial(usbh_cdc_handle_t cdc, uint8_t *leftover,
                          size_t *leftover_len)
{
    esp_err_t err;
    char cmd[160];

    err = ppp_expect(cdc, "AT", "OK", 2000, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Modem does not answer AT on interface %d", s_cfg.usb_itf_num);
        return err;
    }

    ppp_expect(cdc, "ATE0", "OK", 2000, NULL, NULL);

    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", s_cfg.apn);
    if (ppp_expect(cdc, cmd, "OK", 5000, NULL, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "AT+CGDCONT failed, trying dial anyway");
    }

    ESP_LOGI(TAG, "Dialing \"%s\" (APN \"%s\")", s_cfg.dial_cmd, s_cfg.apn);
    err = ppp_expect(cdc, s_cfg.dial_cmd, "CONNECT", ESP32_PPP_ATD_TIMEOUT_MS,
                     leftover, leftover_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Dial failed (0x%x)", err);
    }
    return err;
}

static void ppp_hangup(usbh_cdc_handle_t cdc)
{
    if (cdc == NULL) {
        return;
    }

    /* PPP escape sequence: 1s guard, "+++", 1s guard, ATH. */
    vTaskDelay(pdMS_TO_TICKS(1100));
    cdc_write_raw(cdc, "+++", 3);
    vTaskDelay(pdMS_TO_TICKS(1100));
    cdc_write_raw(cdc, "ATH\r", 4);
    vTaskDelay(pdMS_TO_TICKS(500));
}

/* ------------------------------------------------------------------------- */
/* Supervisor task                                                           */
/* ------------------------------------------------------------------------- */

static bool ppp_acquire_cdc(usbh_cdc_handle_t *out, bool *owned)
{
    /* Dual-port mode: PPP has its own pre-claimed USB CDC handle on the
       Modem interface (4), independent from reference-ril's AT interface (3). */
    if (!ppp_open_cdc(out)) {
        return false;
    }
    *owned = false;
    return true;
}

static void ppp_release_cdc(usbh_cdc_handle_t cdc, bool owned, bool data_mode)
{
    if (data_mode) {
        ppp_hangup(cdc);
    }
    if (owned && cdc != NULL) {
        esp32_usb_cdc_delete_device(cdc);
    }
    if (s_driver != NULL) {
        s_driver->cdc = NULL;
    }
}

static void ppp_task(void *arg)
{
    (void)arg;

    uint8_t *rx_buffer = malloc(ESP32_PPP_RX_CHUNK);
    if (rx_buffer == NULL) {
        ESP_LOGE(TAG, "No memory for PPP RX buffer");
        xEventGroupSetBits(s_events, ESP32_PPP_DONE_BIT);
        vTaskDelete(NULL);
        return;
    }

    int retry_log = 0;
    bool wait_logged = false;
    while (!s_stop && !esp32_modem_wait_ready(1000)) {
        if (!wait_logged) {
            ESP_LOGI(TAG, "Waiting for modem bring-up to finish");
            wait_logged = true;
        }
    }
    wait_logged = false;

    while (!s_stop) {
        usbh_cdc_handle_t cdc = NULL;
        bool owned = false;
        if (!ppp_acquire_cdc(&cdc, &owned)) {
            if ((retry_log++ % 5) == 0) {
                ESP_LOGW(TAG, "Waiting for USB modem on PPP interface %d",
                         s_cfg.usb_itf_num);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        retry_log = 0;

        uint8_t leftover[256];
        size_t leftover_len = 0;
        bool data_mode = false;

        if (ppp_dial(cdc, leftover, &leftover_len) == ESP_OK) {
            data_mode = true;
            s_lost_ip = false;

            s_driver->cdc = cdc;
            s_driver->running = true;
            esp_netif_action_start(s_netif, NULL, 0, NULL);
            if (leftover_len > 0) {
                esp_netif_receive(s_netif, leftover, leftover_len, NULL);
            }
            ESP_LOGI(TAG, "PPP bridge running");

            while (!s_stop && !s_lost_ip) {
                int read = esp32_usb_cdc_read_available(cdc, rx_buffer,
                                                       ESP32_PPP_RX_CHUNK);
                if (read > 0) {
                    esp_netif_receive(s_netif, rx_buffer, (size_t)read, NULL);
                } else if (!cdc_is_open(cdc)) {
                    ESP_LOGW(TAG, "USB data interface disconnected");
                    break;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            s_driver->running = false;
            s_up = false;
            esp_netif_action_stop(s_netif, NULL, 0, NULL);
        } else {
            ESP_LOGW(TAG, "PPP dial failed, retrying in %" PRIu32 " ms", s_cfg.retry_ms);
        }

        ppp_release_cdc(cdc, owned, data_mode);

        if (!s_stop) {
            vTaskDelay(pdMS_TO_TICKS(s_cfg.retry_ms));
        }
    }

    free(rx_buffer);
    xEventGroupSetBits(s_events, ESP32_PPP_DONE_BIT);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t esp32_ppp_start(const esp32_ppp_config_t *config)
{
    if (s_started) {
        return ESP_OK;
    }

    if (config != NULL) {
        s_cfg = *config;
    } else {
        esp32_ppp_config_t default_config = ESP32_PPP_DEFAULT_CONFIG();
        s_cfg = default_config;
    }
    if (s_cfg.apn == NULL || s_cfg.apn[0] == '\0') {
        s_cfg.apn = ESP32_PPP_DEFAULT_APN;
    }
    if (s_cfg.dial_cmd == NULL || s_cfg.dial_cmd[0] == '\0') {
        s_cfg.dial_cmd = ESP32_PPP_DEFAULT_DIAL_CMD;
    }
    if (s_cfg.retry_ms == 0) {
        s_cfg.retry_ms = ESP32_PPP_DEFAULT_RETRY_MS;
    }

    /* The USB host/CDC driver is installed by the modem bring-up task after
       the reset/power sequence. Do not install it here: doing so before the
       module reset makes the modem enumerate and then disappear, which the
       application treats as "USB dongle removed" and enters deep sleep. */

    esp_err_t err;

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_driver = calloc(1, sizeof(*s_driver));
    if (s_driver == NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_driver->base.post_attach = ppp_post_attach;

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_PPP();
    s_netif = esp_netif_new(&netif_config);
    if (s_netif == NULL) {
        free(s_driver);
        s_driver = NULL;
        vEventGroupDelete(s_events);
        s_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    err = esp_netif_attach(s_netif, s_driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(err));
        esp_netif_destroy(s_netif);
        s_netif = NULL;
        free(s_driver);
        s_driver = NULL;
        vEventGroupDelete(s_events);
        s_events = NULL;
        return err;
    }

    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                               esp_netif_action_connected, s_netif);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                               esp_netif_action_disconnected, s_netif);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                               ppp_ip_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                               ppp_ip_event_handler, NULL);
    esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                               ppp_status_event_handler, NULL);

    esp_netif_ppp_config_t ppp_params = {
        .ppp_error_event_enabled = true,
        .ppp_phase_event_enabled = false,
    };
    esp_netif_ppp_set_params(s_netif, &ppp_params);

    if (s_cfg.username != NULL && s_cfg.username[0] != '\0') {
        esp_netif_ppp_set_auth(s_netif, NETIF_PPP_AUTHTYPE_PAP,
                               s_cfg.username,
                               s_cfg.password != NULL ? s_cfg.password : "");
    }

    s_stop = false;
    s_up = false;
    s_lost_ip = false;
    s_started = true;

    if (xTaskCreate(ppp_task, "esp32_ppp", 8192, NULL, 5, NULL) != pdPASS) {
        s_started = false;
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP, ppp_ip_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_LOST_IP, ppp_ip_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP, esp_netif_action_connected);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_LOST_IP, esp_netif_action_disconnected);
        esp_event_handler_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, ppp_status_event_handler);
        esp_netif_destroy(s_netif);
        s_netif = NULL;
        free(s_driver);
        s_driver = NULL;
        vEventGroupDelete(s_events);
        s_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t esp32_ppp_stop(void)
{
    if (!s_started) {
        return ESP_OK;
    }

    s_stop = true;
    EventBits_t bits = xEventGroupWaitBits(s_events, ESP32_PPP_DONE_BIT, pdTRUE,
                                           pdTRUE, pdMS_TO_TICKS(35000));
    if ((bits & ESP32_PPP_DONE_BIT) == 0) {
        ESP_LOGE(TAG, "PPP task did not stop in time");
        return ESP_ERR_TIMEOUT;
    }

    s_started = false;

    esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP, ppp_ip_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_LOST_IP, ppp_ip_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP, esp_netif_action_connected);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_LOST_IP, esp_netif_action_disconnected);
    esp_event_handler_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, ppp_status_event_handler);

    if (s_netif != NULL) {
        esp_netif_destroy(s_netif);
        s_netif = NULL;
    }
    free(s_driver);
    s_driver = NULL;

    vEventGroupDelete(s_events);
    s_events = NULL;
    s_up = false;

    return ESP_OK;
}

bool esp32_ppp_is_up(void)
{
    return s_up;
}

bool esp32_ppp_is_started(void)
{
    return s_started;
}
