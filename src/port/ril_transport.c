/*
 * USB CDC-ACM transport for reference-ril.
 *
 * reference-ril talks to the modem through a POSIX file descriptor. On the
 * ESP32-P4 the modem is a USB device, so we expose the USB CDC AT interface
 * as a small VFS character device. reference-ril can then simply open()
 * "/dev/esp32ril0" and use read()/write() as on Android.
 */
#include <errno.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iot_usbh_cdc.h"
#include "usb/usb_helpers.h"

#include "esp32_ril.h"
#include "ril_transport.h"

#define ESP32_RIL_VFS_PREFIX   "/dev/esp32ril"
#define ESP32_RIL_VFS_PATH     "/dev/esp32ril/0"
#define ESP32_RIL_OPEN_TIMEOUT_MS 10000
#define ESP32_RIL_READ_POLL_MS     200

static const char *TAG = "esp32_ril_usb";

static usbh_cdc_handle_t s_cdc;
static volatile bool s_connected;
static usbh_cdc_handle_t s_ppp_cdc;
static volatile bool s_ppp_connected;
static volatile bool s_driver_installed;
static volatile bool s_vfs_registered;
static volatile bool s_ppp_mode;
static SemaphoreHandle_t s_cdc_mutex;
static esp32_ril_config_t s_cfg;

/* Log the USB interfaces of a newly attached device. This makes it obvious
   which bInterfaceNumber should be used for AT/PPP. */
static void usb_new_dev_cb(usb_device_handle_t usb_dev, void *user_data)
{
    (void)user_data;

    const usb_device_desc_t *device_desc = NULL;
    if (usb_host_get_device_descriptor(usb_dev, &device_desc) == ESP_OK &&
        device_desc != NULL) {
        ESP_LOGI(TAG, "USB modem attached: VID=%04X PID=%04X",
                 device_desc->idVendor, device_desc->idProduct);
    }

    const usb_config_desc_t *config_desc = NULL;
    if (usb_host_get_active_config_descriptor(usb_dev, &config_desc) == ESP_OK &&
        config_desc != NULL) {
        for (int i = 0; i < 8; i++) {
            int offset = 0;
            const usb_intf_desc_t *intf =
                usb_parse_interface_descriptor(config_desc, i, 0, &offset);
            if (intf != NULL) {
                ESP_LOGI(TAG, "  interface %d: class=%02X subclass=%02X protocol=%02X endpoints=%d",
                         intf->bInterfaceNumber, intf->bInterfaceClass,
                         intf->bInterfaceSubClass, intf->bInterfaceProtocol,
                         intf->bNumEndpoints);
                int ep_offset = offset;
                for (int e = 0; e < intf->bNumEndpoints; e++) {
                    const usb_ep_desc_t *ep =
                        usb_parse_endpoint_descriptor_by_index(intf, e,
                                                               config_desc->wTotalLength,
                                                               &ep_offset);
                    if (ep == NULL) {
                        break;
                    }
                    ESP_LOGI(TAG, "    ep 0x%02X type=%d",
                             ep->bEndpointAddress,
                             USB_EP_DESC_GET_XFERTYPE(ep));
                }
            }
        }
    }
}

static void cdc_connect_cb(usbh_cdc_handle_t cdc_handle, void *user_data)
{
    (void)cdc_handle;
    if (user_data != NULL) {
        s_ppp_connected = true;
        ESP_LOGI(TAG, "PPP USB CDC opened (interface %d)", CONFIG_ESP32_PPP_USB_ITF);
    } else {
        s_connected = true;
        ESP_LOGI(TAG, "AT USB CDC opened (interface %d)", s_cfg.usb_itf_num);
    }
}

static void cdc_disconnect_cb(usbh_cdc_handle_t cdc_handle, void *user_data)
{
    (void)cdc_handle;
    if (user_data != NULL) {
        s_ppp_connected = false;
        ESP_LOGW(TAG, "PPP USB CDC disconnected");
    } else {
        s_connected = false;
        ESP_LOGW(TAG, "AT USB CDC disconnected");
    }
}

/*
 * Create a CDC handle before the modem enumerates. iot_usbh_cdc keeps the
 * handle in its device list even when no device is present yet; its USB host
 * event callback opens it when the modem attaches. This is the same pattern
 * iot_usbh_modem used and avoids racing/duplicating usb_host_device_open.
 */
static usbh_cdc_handle_t cdc_claim_early(int itf_num, size_t rx_size,
                                         size_t tx_size, bool ppp)
{
    usbh_cdc_handle_t handle = NULL;
    usbh_cdc_device_config_t dev_config = {
        .vid = CDC_HOST_ANY_VID,
        .pid = CDC_HOST_ANY_PID,
        .itf_num = itf_num,
        .rx_buffer_size = rx_size,
        .tx_buffer_size = tx_size,
        .cbs = {
            .connect = cdc_connect_cb,
            .disconnect = cdc_disconnect_cb,
            .user_data = ppp ? (void *)1 : NULL,
        },
    };

    esp_err_t err = usbh_cdc_create(&dev_config, &handle);
    if (err == ESP_OK && handle != NULL) {
        ESP_LOGI(TAG, "USB CDC interface %d claimed early (device present)",
                 itf_num);
    } else if (handle != NULL) {
        ESP_LOGI(TAG, "USB CDC interface %d queued until modem attaches",
                 itf_num);
    } else {
        ESP_LOGW(TAG, "USB CDC interface %d claim failed: %s",
                 itf_num, esp_err_to_name(err));
    }

    if (ppp) {
        s_ppp_cdc = handle;
        s_ppp_connected = (err == ESP_OK && handle != NULL);
    } else {
        s_cdc = handle;
        s_connected = (err == ESP_OK && handle != NULL);
    }
    return handle;
}

/* Create (or re-create) the CDC device handle. Caller holds s_cdc_mutex. */
static bool cdc_open_locked(void)
{
    if (s_cdc != NULL) {
        /* Early handle: wait for the USB host event callback to open it. */
        return s_connected;
    }

    usbh_cdc_device_config_t dev_config = {
        .vid = CDC_HOST_ANY_VID,
        .pid = CDC_HOST_ANY_PID,
        .itf_num = s_cfg.usb_itf_num,
        .rx_buffer_size = s_cfg.rx_buffer_size,
        .tx_buffer_size = s_cfg.tx_buffer_size,
        .cbs = {
            .connect = cdc_connect_cb,
            .disconnect = cdc_disconnect_cb,
            .user_data = NULL,
        },
    };

    esp_err_t err = usbh_cdc_create(&dev_config, &s_cdc);
    if (err != ESP_OK || s_cdc == NULL) {
        /* usbh_cdc_create() may return an unopened handle on error and it is
           already linked into the driver list; delete it to avoid leaking
           12 KB of ring buffers on every retry. */
        if (s_cdc != NULL) {
            usbh_cdc_delete(s_cdc);
        }
        s_cdc = NULL;
        static int fail_count;
        if ((fail_count++ % 10) == 0) {
            ESP_LOGW(TAG, "usbh_cdc_create(itf=%d) failed: %s",
                     s_cfg.usb_itf_num, esp_err_to_name(err));
        }
        return false;
    }

    /* If the device was already attached, usbh_cdc_create() may have invoked
       the connect callback; make sure the state is consistent. */
    usbh_cdc_state_t state = USBH_CDC_CLOSE;
    if (usbh_cdc_get_state(s_cdc, &state) == ESP_OK && state == USBH_CDC_OPEN) {
        s_connected = true;
    }

    if (s_connected) {
        ESP_LOGI(TAG, "AT USB CDC open: interface %d, handle %p",
                 s_cfg.usb_itf_num, s_cdc);
    }

    return s_connected;
}

bool esp32_ril_transport_wait_ready(uint32_t timeout_ms)
{
    if (s_cdc_mutex == NULL) {
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    uint32_t last_log = 0;

    while ((xTaskGetTickCount() - start) < timeout) {
        xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
        bool ready = cdc_open_locked();
        xSemaphoreGive(s_cdc_mutex);
        if (ready) {
            return true;
        }

        uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - start) *
                                         1000 / configTICK_RATE_HZ);
        if (elapsed_ms >= last_log + 2000) {
            usbh_cdc_state_t state = USBH_CDC_CLOSE;
            if (s_cdc != NULL) {
                usbh_cdc_get_state(s_cdc, &state);
            }
            ESP_LOGW(TAG, "waiting for USB AT interface %d: connected=%d state=%d",
                     s_cfg.usb_itf_num, s_connected, state);
            last_log = elapsed_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    return false;
}

bool esp32_ril_transport_is_connected(void)
{
    return s_connected && s_cdc != NULL;
}

usbh_cdc_handle_t esp32_usb_cdc_ppp_handle(void)
{
    if (s_ppp_cdc == NULL) {
        return NULL;
    }
    usbh_cdc_state_t state = USBH_CDC_CLOSE;
    if (usbh_cdc_get_state(s_ppp_cdc, &state) == ESP_OK &&
        state == USBH_CDC_OPEN) {
        return s_ppp_cdc;
    }
    return NULL;
}

void esp32_ril_transport_set_ppp_mode(bool enabled)
{
    s_ppp_mode = enabled;
}

bool esp32_ril_transport_in_ppp_mode(void)
{
    return s_ppp_mode;
}

usbh_cdc_handle_t esp32_ril_transport_get_cdc(void)
{
    if (s_cdc_mutex == NULL) {
        return NULL;
    }

    xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
    usbh_cdc_handle_t handle = cdc_open_locked() ? s_cdc : NULL;
    xSemaphoreGive(s_cdc_mutex);
    return handle;
}

int esp32_ril_transport_at_command(const char *cmd, char *resp,
                                  size_t resp_size, uint32_t timeout_ms)
{
    if (cmd == NULL || resp == NULL || resp_size == 0) {
        return -1;
    }
    resp[0] = '\0';

    if (!esp32_ril_transport_wait_ready(2000)) {
        return -1;
    }

    usbh_cdc_flush_rx_buffer(s_cdc);

    char command[64];
    snprintf(command, sizeof(command), "%s\r", cmd);
    if (usbh_cdc_write_bytes(s_cdc, (const uint8_t *)command, strlen(command),
                             pdMS_TO_TICKS(500)) != ESP_OK) {
        return -1;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t used = 0;
    bool ok = false;

    while (esp_timer_get_time() < deadline) {
        uint8_t buffer[64];
        int read = esp32_usb_cdc_read_available(s_cdc, buffer, sizeof(buffer));
        if (read > 0) {
            size_t length = (size_t)read;
            if (length > resp_size - 1 - used) {
                length = resp_size - 1 - used;
            }
            memcpy(resp + used, buffer, length);
            used += length;
            resp[used] = '\0';
            if (strstr(resp, "OK") != NULL) {
                ok = true;
                break;
            }
            if (strstr(resp, "ERROR") != NULL) {
                break;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    ESP_LOGI(TAG, "AT probe \"%s\" -> %s", cmd, used ? resp : "<no response>");
    return ok ? 0 : -1;
}

int esp32_ril_transport_get_itf(void)
{
    return s_cfg.usb_itf_num;
}

int esp32_usb_cdc_read_available(usbh_cdc_handle_t handle, uint8_t *buf, size_t cap)
{
    if (handle == NULL || buf == NULL || cap == 0) {
        return 0;
    }

    size_t available = 0;
    if (usbh_cdc_get_rx_buffer_size(handle, &available) != ESP_OK ||
        available == 0) {
        return 0;
    }

    size_t length = cap;
    if (usbh_cdc_read_bytes(handle, buf, &length, 0) != ESP_OK) {
        return 0;
    }
    return (int)length;
}

static bool cdc_at_probe(usbh_cdc_handle_t handle, uint32_t timeout_ms)
{
    if (handle == NULL) {
        return false;
    }

    usbh_cdc_flush_rx_buffer(handle);
    const char command[] = "AT\r";
    if (usbh_cdc_write_bytes(handle, (const uint8_t *)command, sizeof(command) - 1,
                             pdMS_TO_TICKS(300)) != ESP_OK) {
        return false;
    }

    char response[96] = {0};
    size_t used = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        uint8_t buffer[32];
        int read = esp32_usb_cdc_read_available(handle, buffer, sizeof(buffer));
        if (read > 0) {
            size_t length = (size_t)read;
            if (length > sizeof(response) - 1 - used) {
                length = sizeof(response) - 1 - used;
            }
            memcpy(response + used, buffer, length);
            used += length;
            response[used] = '\0';
            if (strstr(response, "OK") != NULL) {
                return true;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    return false;
}

int esp32_ril_transport_probe_interface(int itf)
{
    if (s_cdc_mutex == NULL) {
        return -1;
    }

    /* Close the previous handle so the same interface can be re-opened. */
    xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
    if (s_cdc != NULL) {
        usbh_cdc_delete(s_cdc);
        s_cdc = NULL;
    }
    s_connected = false;
    xSemaphoreGive(s_cdc_mutex);

    usbh_cdc_handle_t handle = NULL;
    if (esp32_usb_cdc_create_device(itf, s_cfg.rx_buffer_size,
                                   s_cfg.tx_buffer_size, &handle) != ESP_OK) {
        return -1;
    }

    bool answered = cdc_at_probe(handle, 1500);
    ESP_LOGI(TAG, "AT scan: interface %d %s", itf,
             answered ? "answered" : "no response");

    if (!answered) {
        esp32_usb_cdc_delete_device(handle);
        return -1;
    }

    xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
    s_cdc = handle;
    s_connected = true;
    s_cfg.usb_itf_num = itf;
    xSemaphoreGive(s_cdc_mutex);
    return itf;
}

int esp32_ril_transport_find_at_interface(int first, int last)
{
    for (int itf = first; itf <= last; itf++) {
        int found = esp32_ril_transport_probe_interface(itf);
        if (found >= 0) {
            return found;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* VFS callbacks                                                             */
/* ------------------------------------------------------------------------- */

static int esp32_ril_vfs_open(const char *path, int flags, int mode)
{
    (void)path;
    (void)flags;
    (void)mode;

    /* While PPP owns the shared USB port, keep reference-ril's reopen
       blocked until the PPP session is over. */
    while (s_ppp_mode) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!esp32_ril_transport_wait_ready(ESP32_RIL_OPEN_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "No USB modem found on AT interface %d", s_cfg.usb_itf_num);
        errno = ENODEV;
        return -1;
    }

    return 0;
}

static int esp32_ril_vfs_close(int fd)
{
    (void)fd;
    return 0;
}

static ssize_t esp32_ril_vfs_read(int fd, void *dst, size_t size)
{
    (void)fd;

    /* PPP owns the port: report EOF so that reference-ril's reader exits and
       its main loop parks in open(). */
    if (s_ppp_mode) {
        return 0;
    }

    while (size > 0) {
        if (s_ppp_mode) {
            return 0;
        }
        if (!s_connected || s_cdc == NULL) {
            /* EOF: reference-ril will close and re-open the AT channel */
            return 0;
        }

        int read = esp32_usb_cdc_read_available(s_cdc, (uint8_t *)dst, size);
        if (read > 0) {
            return read;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return 0;
}

static ssize_t esp32_ril_vfs_write(int fd, const void *src, size_t size)
{
    (void)fd;

    if (s_ppp_mode) {
        errno = EBUSY;
        return -1;
    }

    size_t written = 0;
    while (written < size) {
        if (!s_connected || s_cdc == NULL) {
            errno = EPIPE;
            return -1;
        }

        size_t length = size - written;
        esp_err_t err = usbh_cdc_write_bytes(s_cdc, (const uint8_t *)src + written,
                                             length, pdMS_TO_TICKS(200));
        if (err == ESP_OK && length > 0) {
            written += length;
            continue;
        }
        if (err != ESP_OK && !s_connected) {
            errno = EPIPE;
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return (ssize_t)written;
}

static const esp_vfs_t s_ril_vfs = {
    .flags = ESP_VFS_FLAG_DEFAULT,
    .open = esp32_ril_vfs_open,
    .close = esp32_ril_vfs_close,
    .read = esp32_ril_vfs_read,
    .write = esp32_ril_vfs_write,
};

/* ------------------------------------------------------------------------- */
/* Public transport API                                                      */
/* ------------------------------------------------------------------------- */

const char *esp32_ril_transport_path(void)
{
    return ESP32_RIL_VFS_PATH;
}

esp_err_t esp32_usb_cdc_ensure_driver(void)
{
    if (s_driver_installed) {
        return ESP_OK;
    }

    const usbh_cdc_driver_config_t drv_config = {
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_coreid = -1,
        .skip_init_usb_host_driver = false,
        .new_dev_cb = usb_new_dev_cb,
    };
    esp_err_t err = usbh_cdc_driver_install(&drv_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usbh_cdc_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    s_driver_installed = true;
    return ESP_OK;
}

esp_err_t esp32_usb_cdc_create_device(int itf_num, size_t rx_buffer_size,
                                     size_t tx_buffer_size,
                                     usbh_cdc_handle_t *handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *handle = NULL;

    if (s_cdc_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);

    usbh_cdc_device_config_t dev_config = {
        .vid = CDC_HOST_ANY_VID,
        .pid = CDC_HOST_ANY_PID,
        .itf_num = itf_num,
        .rx_buffer_size = rx_buffer_size,
        .tx_buffer_size = tx_buffer_size,
    };

    ESP_LOGI(TAG, "Opening USB CDC interface %d", itf_num);

    esp_err_t err = usbh_cdc_create(&dev_config, handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "USB CDC interface %d open failed: %s", itf_num,
                 esp_err_to_name(err));
        /* usbh_cdc_create() may return an unopened handle; reclaim it. */
        if (*handle != NULL) {
            usbh_cdc_delete(*handle);
            *handle = NULL;
        }
        xSemaphoreGive(s_cdc_mutex);
        return err;
    }

    usbh_cdc_state_t state = USBH_CDC_CLOSE;
    if (*handle == NULL ||
        usbh_cdc_get_state(*handle, &state) != ESP_OK ||
        state != USBH_CDC_OPEN) {
        if (*handle != NULL) {
            usbh_cdc_delete(*handle);
        }
        *handle = NULL;
        err = ESP_ERR_NOT_FOUND;
        ESP_LOGI(TAG, "USB CDC interface %d did not open", itf_num);
    } else {
        ESP_LOGI(TAG, "USB CDC interface %d opened", itf_num);
    }

    xSemaphoreGive(s_cdc_mutex);
    return err;
}

esp_err_t esp32_usb_cdc_delete_device(usbh_cdc_handle_t handle)
{
    if (handle == NULL) {
        return ESP_OK;
    }
    if (s_cdc_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
    esp_err_t err = usbh_cdc_delete(handle);
    xSemaphoreGive(s_cdc_mutex);
    return err;
}

esp_err_t esp32_ril_transport_init(const esp32_ril_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *config;
    if (s_cfg.usb_itf_num <= 0) {
        s_cfg.usb_itf_num = ESP32_RIL_DEFAULT_AT_ITF;
    }
    if (s_cfg.rx_buffer_size == 0) {
        s_cfg.rx_buffer_size = 8 * 1024;
    }
    if (s_cfg.tx_buffer_size == 0) {
        s_cfg.tx_buffer_size = 4 * 1024;
    }

    if (s_cdc_mutex == NULL) {
        s_cdc_mutex = xSemaphoreCreateMutex();
        if (s_cdc_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_vfs_registered) {
        esp_err_t err = esp_vfs_register(ESP32_RIL_VFS_PREFIX, &s_ril_vfs, NULL);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_vfs_register failed: %s", esp_err_to_name(err));
            return err;
        }
        s_vfs_registered = true;
    }

    if (!s_driver_installed) {
        esp_err_t err = esp32_usb_cdc_ensure_driver();
        if (err != ESP_OK) {
            return err;
        }
    }

    /* Claim both handles before the module finishes booting, exactly like
       iot_usbh_modem's primary + secondary CDC devices. Insert the PPP
       handle first so that the AT interface is at the head of the driver
       list and gets opened first when the modem enumerates. */
    cdc_claim_early(CONFIG_ESP32_PPP_USB_ITF, 8192, 4096, true);
    cdc_claim_early(s_cfg.usb_itf_num, s_cfg.rx_buffer_size,
                    s_cfg.tx_buffer_size, false);

    ESP_LOGI(TAG, "USB CDC transport ready, AT interface %d, PPP interface %d",
             s_cfg.usb_itf_num, CONFIG_ESP32_PPP_USB_ITF);

    return ESP_OK;
}

esp_err_t esp32_ril_transport_deinit(void)
{
    xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
    if (s_cdc != NULL) {
        usbh_cdc_delete(s_cdc);
        s_cdc = NULL;
    }
    s_connected = false;
    xSemaphoreGive(s_cdc_mutex);

    if (s_driver_installed) {
        usbh_cdc_driver_uninstall();
        s_driver_installed = false;
    }

    if (s_vfs_registered) {
        esp_vfs_unregister(ESP32_RIL_VFS_PREFIX);
        s_vfs_registered = false;
    }

    return ESP_OK;
}
