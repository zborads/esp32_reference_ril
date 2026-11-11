/*
 * reference-ril porting glue for ESP-IDF.
 *
 * - implements the RIL_Env callbacks required by reference-ril.c
 * - runs RIL timed callbacks on a dedicated FreeRTOS task (reference-ril
 *   executes AT commands from those callbacks, so they must not run in the
 *   esp_timer task)
 * - provides the esp32_ril_xxx helpers used by the application
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include "at_tok.h"
#include "atchannel.h"
#include "ril.h"

#include "esp32_modem.h"
#include "esp32_ril.h"
#include "esp32_ril_internal.h"
#include "ril_transport.h"

ESP_EVENT_DEFINE_BASE(ESP32_RIL_EVENT);

static const char *TAG = "esp32_ril";

#define ESP32_RIL_CMD_TIMEOUT_MS   3000
#define ESP32_RIL_ICCID_TIMEOUT_MS 5000
#define ESP32_RIL_MONITOR_PERIOD_MS 5000

typedef struct {
    RIL_TimedCallback callback;
    void *param;
} ril_timer_msg_t;

typedef struct {
    RIL_TimedCallback callback;
    void *param;
    esp_timer_handle_t timer;
} ril_delayed_msg_t;

static esp32_ril_config_t s_config;
static bool s_started;
static QueueHandle_t s_timer_queue;
static TaskHandle_t s_monitor_task;
static TaskHandle_t s_bringup_task;

static void post_event(esp32_ril_event_t event);

/* SIM readiness comes from reference-ril's single AT+CPIN? query
   (esp32_ril_sim_ready_cached()); the port never polls the SIM itself.
   -1 = not queried yet, 0 = not usable, 1 = ready. */
static int s_last_sim_ready = -1;

/* reference-ril cannot talk to the modem while PPP owns the shared USB
   interface; keep the last known values so the UI still has data. */
static char s_cached_iccid[32];
static bool s_has_iccid;
static int s_cached_rssi = 99;
static int s_cached_ber = 99;
static bool s_has_signal;

/* Ignore USB DTE transitions while the module is being mounted (reset and
   power-key pulses make it enumerate/disappear several times). The app
   enters deep sleep on DTE_DISCONN, so a transient disconnect during boot
   would look like a reboot. */
static volatile TickType_t s_dte_ignore_until;

static void ril_timer_dispatch(void *arg)
{
    ril_delayed_msg_t *delayed = (ril_delayed_msg_t *)arg;
    if (delayed == NULL) {
        return;
    }

    ril_timer_msg_t msg = {
        .callback = delayed->callback,
        .param = delayed->param,
    };
    if (s_timer_queue != NULL) {
        xQueueSend(s_timer_queue, &msg, 0);
    }

    /* One-shot timer has expired; reclaim it. */
    esp_timer_delete(delayed->timer);
    free(delayed);
}

static void ril_timer_task(void *arg)
{
    (void)arg;
    ril_timer_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_timer_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.callback != NULL) {
                msg.callback(msg.param);
            }
        }
    }
}

static void ril_on_request_complete(RIL_Token t, RIL_Errno e, void *response, size_t responselen)
{
    (void)t;
    (void)response;
    (void)responselen;
    ESP_LOGD(TAG, "RIL request complete, errno=%d", e);
}

/* Translate the cached AT+CPIN? result into an application event. A SIM that
   is absent / locked / failed is reported as SIM_DISCONN (once). */
static void report_sim_state(void)
{
    int ready = esp32_ril_sim_ready_cached();

    if (ready < 0 || ready == s_last_sim_ready) {
        return;
    }
    s_last_sim_ready = ready;

    if (ready) {
        ESP_LOGI(TAG, "SIM ready");
        post_event(ESP32_RIL_EVENT_SIM_CONN);
    } else {
        ESP_LOGW(TAG, "SIM not ready");
        post_event(ESP32_RIL_EVENT_SIM_DISCONN);
    }
}

static void ril_on_unsolicited_response(int unsolResponse, const void *data, size_t datalen)
{
    (void)data;
    (void)datalen;
    ESP_LOGD(TAG, "RIL unsolicited response %d", unsolResponse);

    switch (unsolResponse) {
    case RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED:
        /* reference-ril sends this after its single AT+CPIN? query. */
        report_sim_state();
        break;

    case RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED:
        /* Posted on every radio state change; SIM readiness is reported
           through SIM_STATUS_CHANGED above. */
        break;

    default:
        break;
    }
}

static void ril_on_request_ack(RIL_Token t)
{
    (void)t;
}

static void ril_request_timed_callback(RIL_TimedCallback callback, void *param,
                                       const struct timeval *relativeTime)
{
    if (callback == NULL || s_timer_queue == NULL) {
        return;
    }

    ril_timer_msg_t *msg = calloc(1, sizeof(*msg));
    if (msg == NULL) {
        return;
    }
    msg->callback = callback;
    msg->param = param;

    uint64_t delay_us = 0;
    if (relativeTime != NULL) {
        delay_us = (uint64_t)relativeTime->tv_sec * 1000000ULL + (uint64_t)relativeTime->tv_usec;
    }

    if (delay_us == 0) {
        /* Run ASAP but never from the caller's (AT reader) context. */
        if (xQueueSend(s_timer_queue, msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "RIL callback queue full");
        }
        free(msg);
        return;
    }

    ril_delayed_msg_t *delayed = calloc(1, sizeof(*delayed));
    if (delayed == NULL) {
        free(msg);
        return;
    }
    delayed->callback = callback;
    delayed->param = param;
    free(msg);

    esp_timer_create_args_t args = {
        .callback = ril_timer_dispatch,
        .arg = delayed,
        .name = "ril_cb",
    };
    if (esp_timer_create(&args, &delayed->timer) != ESP_OK) {
        free(delayed);
        return;
    }
    if (esp_timer_start_once(delayed->timer, delay_us) != ESP_OK) {
        esp_timer_delete(delayed->timer);
        free(delayed);
    }
}

static const struct RIL_Env s_ril_env = {
    .OnRequestComplete = ril_on_request_complete,
    .OnUnsolicitedResponse = ril_on_unsolicited_response,
    .RequestTimedCallback = ril_request_timed_callback,
    .OnRequestAck = ril_on_request_ack,
};

/* ------------------------------------------------------------------------- */
/* AT command helpers                                                        */
/* ------------------------------------------------------------------------- */

static void post_event(esp32_ril_event_t event)
{
    esp_event_post(ESP32_RIL_EVENT, event, NULL, 0, pdMS_TO_TICKS(100));
}

static ATResponse *send_command_singleline(const char *cmd, const char *prefix,
                                           long long timeout_ms)
{
    if (!at_is_open()) {
        return NULL;
    }

    ATResponse *response = NULL;
    int err = at_send_command_singleline_timeout(cmd, prefix, timeout_ms, &response);
    if (err < 0) {
        at_response_free(response);
        return NULL;
    }
    return response;
}

static const char *response_first_line(const ATResponse *response)
{
    if (response == NULL || response->p_intermediates == NULL) {
        return NULL;
    }
    return response->p_intermediates->line;
}

static void trim_inplace(char *str)
{
    if (str == NULL) {
        return;
    }
    while (*str == ' ' || *str == '\t' || *str == '"') {
        memmove(str, str + 1, strlen(str));
    }
    size_t len = strlen(str);
    while (len > 0) {
        char c = str[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '"') {
            break;
        }
        str[--len] = '\0';
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t esp32_ril_get_signal_quality(int *rssi, int *ber)
{
    if (rssi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ATResponse *response = send_command_singleline("AT+CSQ", "+CSQ:",
                                                   ESP32_RIL_CMD_TIMEOUT_MS);
    if (response == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    const char *line = response_first_line(response);
    if (line != NULL) {
        char *p = (char *)line;
        int rssi_value = 99;
        int ber_value = 99;
        if (at_tok_start(&p) == 0 &&
            at_tok_nextint(&p, &rssi_value) == 0 &&
            at_tok_nextint(&p, &ber_value) == 0) {
            *rssi = rssi_value;
            if (ber != NULL) {
                *ber = ber_value;
            }
            s_cached_rssi = rssi_value;
            s_cached_ber = ber_value;
            s_has_signal = true;
            ret = ESP_OK;
        }
    }

    at_response_free(response);

    if (ret != ESP_OK && s_has_signal) {
        *rssi = s_cached_rssi;
        if (ber != NULL) {
            *ber = s_cached_ber;
        }
        ret = ESP_OK;
    }
    return ret;
}

esp_err_t esp32_ril_get_iccid(char *buf, size_t buf_size)
{
    /* AT+QCCID is the ICCID query documented for the Quectel LTE Standard(E)
       series (section 5.7). AT+CCID / AT+CICCID are not in that command set
       and were removed. */
    static const struct {
        const char *cmd;
        const char *prefix;
    } commands[] = {
        { "AT+QCCID", "+QCCID:" },
    };

    if (buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        ATResponse *response = send_command_singleline(commands[i].cmd,
                                                       commands[i].prefix,
                                                       ESP32_RIL_ICCID_TIMEOUT_MS);
        if (response == NULL) {
            continue;
        }

        bool found = false;
        for (ATLine *line = response->p_intermediates; line != NULL; line = line->p_next) {
            char *colon = strchr(line->line, ':');
            if (colon == NULL) {
                continue;
            }
            char tmp[64] = {0};
            strncpy(tmp, colon + 1, sizeof(tmp) - 1);
            trim_inplace(tmp);
            if (tmp[0] != '\0') {
                strncpy(buf, tmp, buf_size - 1);
                buf[buf_size - 1] = '\0';
                strncpy(s_cached_iccid, buf, sizeof(s_cached_iccid) - 1);
                s_cached_iccid[sizeof(s_cached_iccid) - 1] = '\0';
                s_has_iccid = true;
                found = true;
                break;
            }
        }

        at_response_free(response);
        if (found) {
            return ESP_OK;
        }
    }

    if (s_has_iccid) {
        strncpy(buf, s_cached_iccid, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t esp32_ril_get_operator(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    ATResponse *response = send_command_singleline("AT+COPS?", "+COPS:",
                                                   ESP32_RIL_CMD_TIMEOUT_MS);
    if (response == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    const char *line = response_first_line(response);
    if (line != NULL) {
        const char *first_quote = strchr(line, '"');
        const char *last_quote = strrchr(line, '"');
        if (first_quote != NULL && last_quote != NULL && last_quote > first_quote) {
            size_t len = (size_t)(last_quote - first_quote - 1);
            if (len >= buf_size) {
                len = buf_size - 1;
            }
            memcpy(buf, first_quote + 1, len);
            buf[len] = '\0';
            ret = ESP_OK;
        }
    }

    at_response_free(response);
    return ret;
}

esp_err_t esp32_ril_get_sim_state(bool *ready)
{
    int cached;

    if (ready == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ready = false;

    /* Cached result of reference-ril's single AT+CPIN? query. This function
       never sends an AT command itself. */
    cached = esp32_ril_sim_ready_cached();
    if (cached < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    *ready = (cached == 1);
    return ESP_OK;
}

esp_err_t esp32_ril_get_registration_state(int *stat)
{
    if (stat == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *stat = 0;

    ATResponse *response = send_command_singleline("AT+CREG?", "+CREG:",
                                                   ESP32_RIL_CMD_TIMEOUT_MS);
    if (response == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    const char *line = response_first_line(response);
    if (line != NULL) {
        char *p = (char *)line;
        int n = 0;
        int unused = 0;
        if (at_tok_start(&p) == 0 && at_tok_nextint(&p, &n) == 0) {
            int value = n;
            if (at_tok_nextint(&p, &value) == 0) {
                *stat = value;
            } else {
                *stat = n;
            }
            (void)unused;
            ret = ESP_OK;
        }
    }

    at_response_free(response);
    return ret;
}

bool esp32_ril_is_connected(void)
{
    return esp32_ril_transport_is_connected();
}

/* ------------------------------------------------------------------------- */
/* Modem state monitor                                                       */
/* ------------------------------------------------------------------------- */

static void ril_monitor_task(void *arg)
{
    (void)arg;

    bool last_dte = false;
    int disc_count = 0;

    for (;;) {
        bool in_grace = xTaskGetTickCount() < s_dte_ignore_until;
        bool dte = esp32_ril_transport_is_connected();

        if (in_grace) {
            /* Track the state without notifying the application. */
            last_dte = dte;
            disc_count = 0;
        } else if (dte) {
            if (!last_dte) {
                ESP_LOGI(TAG, "DTE connected");
                post_event(ESP32_RIL_EVENT_DTE_CONN);
                last_dte = true;
            }
            disc_count = 0;
        } else if (last_dte) {
            /* Debounce: require two monitor periods (~10 s) before telling
               the app that the modem is gone (it goes to deep sleep). */
            if (++disc_count >= 2) {
                ESP_LOGI(TAG, "DTE disconnected");
                post_event(ESP32_RIL_EVENT_DTE_DISCONN);
                last_dte = false;
                disc_count = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ESP32_RIL_MONITOR_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

static esp_err_t start_reference_ril(void)
{
    /* reference-ril creates pthreads for its main loop and the AT reader.
       Give them enough stack for the AT parsing / asprintf paths. */
    esp_pthread_cfg_t pthread_cfg = esp_pthread_get_default_config();
    pthread_cfg.stack_size = 10240;
    pthread_cfg.thread_name = "ril";
    esp_pthread_set_cfg(&pthread_cfg);

    static char arg0[] = "reference-ril";
    static char arg1[] = "-d";
    char *argv[] = { arg0, arg1, (char *)esp32_ril_transport_path() };

    const RIL_RadioFunctions *functions = RIL_Init(&s_ril_env, 3, argv);
    if (functions == NULL) {
        ESP_LOGE(TAG, "RIL_Init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "reference-ril started (AT device %s)", esp32_ril_transport_path());
    return ESP_OK;
}

/*
 * Modem bring-up task: power/reset sequencing (as iot_usbh_modem did), then
 * install the USB CDC transport and start reference-ril. Runs in the
 * background so that the UI and application tasks are not delayed by the
 * module boot time (Ton-status is typically ~8 s).
 */

static void cache_csq_from_response(const char *resp)
{
    const char *csq = strstr(resp, "+CSQ:");
    int rssi = 99;
    int ber = 99;
    if (csq != NULL && sscanf(csq + 5, "%d,%d", &rssi, &ber) == 2) {
        s_cached_rssi = rssi;
        s_cached_ber = ber;
        s_has_signal = true;
    }
}

static void cache_ccid_from_response(const char *resp)
{
    const char *ccid = strstr(resp, "+CCID:");
    if (ccid == NULL) {
        ccid = strstr(resp, "+QCCID:");
    }
    if (ccid == NULL) {
        ccid = strstr(resp, "+CICCID:");
    }
    if (ccid != NULL) {
        const char *p = strchr(ccid, ':');
        if (p != NULL) {
            p++;
            while (*p == ' ' || *p == '\t' || *p == '"') {
                p++;
            }
            size_t n = 0;
            while (p[n] != '\0' && p[n] != '\r' && p[n] != '\n' &&
                   p[n] != '"' && n < sizeof(s_cached_iccid) - 1) {
                n++;
            }
            if (n > 0u) {
                memcpy(s_cached_iccid, p, n);
                s_cached_iccid[n] = '\0';
                s_has_iccid = true;
            }
        }
    }
}

static void log_usb_bus_devices(void)
{
    uint8_t device_list[8];
    int count = 0;
    if (usb_host_device_addr_list_fill(sizeof(device_list), device_list,
                                       &count) == ESP_OK) {
        ESP_LOGE(TAG, "USB bus device count: %d", count);
    } else {
        ESP_LOGE(TAG, "USB host is not running");
    }
}

static void modem_probe_at_channel(void)
{
    char response[192];

    ESP_LOGI(TAG, "Probing AT interface %d", s_config.usb_itf_num);

    if (!esp32_ril_transport_wait_ready(15000)) {
        ESP_LOGE(TAG, "USB AT interface %d not found", s_config.usb_itf_num);
        log_usb_bus_devices();
        return;
    }

    if (esp32_ril_transport_at_command("AT", response,
                                       sizeof(response), 2000) != 0) {
        ESP_LOGE(TAG, "AT does not respond on interface %d",
                 s_config.usb_itf_num);
        log_usb_bus_devices();
        return;
    }

    ESP_LOGI(TAG, "USB AT channel responds on interface %d",
             esp32_ril_transport_get_itf());

    /* AT+QCCID is the documented ICCID query for the Quectel LTE Standard(E)
       series; AT+CCID / AT+CICCID are not part of this command set. */
    if (esp32_ril_transport_at_command("AT+QCCID", response,
                                      sizeof(response), 3000) == 0) {
        cache_ccid_from_response(response);
    }

    if (esp32_ril_transport_at_command("AT+CSQ", response,
                                      sizeof(response), 3000) == 0) {
        cache_csq_from_response(response);
    }

    /* The SIM state is queried exactly once, by reference-ril itself after
       the radio is switched on (AT+CPIN? -> ESP32_RIL_EVENT_SIM_*). */
}

static void modem_bringup_task(void *arg)
{
    (void)arg;

#if CONFIG_ESP32_MODEM_LOG_RESET_REASON
    ESP_LOGW(TAG, "MCU reset reason: %s (%d)",
             esp32_modem_reset_reason_str(), esp_reset_reason());
#endif

    esp32_modem_hw_init();

    esp_err_t err = esp32_ril_transport_init(&s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AT transport init failed: %s", esp_err_to_name(err));
        esp32_modem_set_ready(true);
        s_bringup_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Power-cycle the module through M_VBAT_EN (GPIO43, active high). */
    esp32_modem_reset_pulse();

    /* M_VBAT_EN usually boots the module automatically. Give it a few seconds
       and only press M_PWRKEY if it did not enumerate. */
    bool enumerated = false;
    for (int i = 0; i < 10; i++) {
        if (esp32_ril_transport_is_connected()) {
            enumerated = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!enumerated) {
        ESP_LOGW(TAG, "Module did not auto-boot, pressing M_PWRKEY");
        esp32_modem_power_on();
        ESP_LOGI(TAG, "Modem power-on sequence finished");
    } else {
        ESP_LOGI(TAG, "Module auto-booted after M_VBAT_EN, PWRKEY skipped");
    }

    /* Verify the USB AT channel and cache status before reference-ril takes
       over the port. */
    modem_probe_at_channel();

    err = start_reference_ril();
    if (err == ESP_OK) {
        /* Wait until the module enumerates and reference-ril opens the AT
           VFS device. */
        for (int i = 0; i < 60 && !esp32_ril_transport_is_connected(); i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (xTaskCreate(ril_monitor_task, "ril_mon", 6144, NULL, 4,
                        &s_monitor_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create RIL monitor task");
        }
    } else {
        ESP_LOGE(TAG, "reference-ril start failed");
    }

    esp32_modem_set_ready(true);
    s_bringup_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t esp32_ril_init(const esp32_ril_config_t *config)
{
    if (s_started) {
        return ESP_OK;
    }

    if (config != NULL) {
        s_config = *config;
    } else {
        esp32_ril_config_t default_config = ESP32_RIL_DEFAULT_CONFIG();
        s_config = default_config;
    }

    s_timer_queue = xQueueCreate(16, sizeof(ril_timer_msg_t));
    if (s_timer_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Give the module time to reset/power up/renumerate before reporting USB
       DTE state to the application. */
    s_dte_ignore_until = xTaskGetTickCount() + pdMS_TO_TICKS(30000);

    if (xTaskCreate(ril_timer_task, "ril_cb", 8192, NULL, 6, NULL) != pdPASS) {
        vQueueDelete(s_timer_queue);
        s_timer_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (s_config.handler != NULL) {
        esp_event_handler_register(ESP32_RIL_EVENT, ESP_EVENT_ANY_ID,
                                   s_config.handler, s_config.handler_arg);
    }

    s_started = true;

    /* Power/reset the module, then start the AT engine in the background. */
    if (xTaskCreate(modem_bringup_task, "modem_up", 6144, NULL, 4,
                    &s_bringup_task) != pdPASS) {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t esp32_ril_deinit(void)
{
    if (!s_started) {
        return ESP_OK;
    }

    if (s_config.handler != NULL) {
        esp_event_handler_unregister(ESP32_RIL_EVENT, ESP_EVENT_ANY_ID, s_config.handler);
    }

    /* Let a running bring-up sequence finish before tearing down. */
    for (int i = 0; i < 200 && s_bringup_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_monitor_task != NULL) {
        vTaskDelete(s_monitor_task);
        s_monitor_task = NULL;
    }

    esp32_ril_transport_deinit();
    s_started = false;
    return ESP_OK;
}
