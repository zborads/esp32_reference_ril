/*
 * Cellular module hardware bring-up for the X10 (ESP32-P4) board.
 *
 * From X10 (ESP32-P4) IO definition:
 *
 *   GPIO42 = M_PWRKEY   module power on/off key ("先高后低": idle high,
 *                       low pulse to power on)
 *   GPIO43 = M_VBAT_EN  module power enable, active HIGH
 *
 * Boot sequence:
 *   1. M_VBAT_EN = 1 (power the module)
 *   2. power cycle: M_VBAT_EN = 0 for ~1 s, then 1
 *   3. PWRKEY idle high, pulse low ~500 ms, release
 *   4. wait ~8 s for the module to boot
 */
#include "esp32_modem.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "esp32_modem";

#define ESP32_MODEM_PWRKEY_GPIO   CONFIG_ESP32_MODEM_PWRKEY_GPIO
#define ESP32_MODEM_PWRKEY_MS     CONFIG_ESP32_MODEM_PWRKEY_MS

#define ESP32_MODEM_VBAT_GPIO     CONFIG_ESP32_MODEM_VBAT_GPIO
#define ESP32_MODEM_VBAT_OFF_MS   CONFIG_ESP32_MODEM_VBAT_OFF_MS

#define ESP32_MODEM_BOOT_WAIT_MS  CONFIG_ESP32_MODEM_BOOT_WAIT_MS

static bool s_hw_inited;
static volatile bool s_ready;

void esp32_modem_hw_init(void)
{
    if (s_hw_inited) {
        return;
    }

    gpio_config_t io_config = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (ESP32_MODEM_PWRKEY_GPIO >= 0) {
        io_config.pin_bit_mask |= (1ULL << ESP32_MODEM_PWRKEY_GPIO);
    }
    if (ESP32_MODEM_VBAT_GPIO >= 0) {
        io_config.pin_bit_mask |= (1ULL << ESP32_MODEM_VBAT_GPIO);
    }

    if (io_config.pin_bit_mask != 0) {
        ESP_ERROR_CHECK(gpio_config(&io_config));
    }

    /* M_VBAT_EN active high: power the module.
       M_PWRKEY idle high (M_PWRKEY is "先高后低"). */
    if (ESP32_MODEM_VBAT_GPIO >= 0) {
        gpio_set_level(ESP32_MODEM_VBAT_GPIO, 1);
    }
    if (ESP32_MODEM_PWRKEY_GPIO >= 0) {
        gpio_set_level(ESP32_MODEM_PWRKEY_GPIO, 1);
    }

    s_hw_inited = true;
    ESP_LOGI(TAG, "Modem mount: M_PWRKEY=GPIO%d, M_VBAT_EN=GPIO%d",
             ESP32_MODEM_PWRKEY_GPIO, ESP32_MODEM_VBAT_GPIO);
}

/* Power-cycle the module through M_VBAT_EN (active high). */
void esp32_modem_reset_pulse(void)
{
    if (ESP32_MODEM_VBAT_GPIO < 0) {
        return;
    }

    ESP_LOGI(TAG, "Power-cycling module: GPIO%d low for %d ms",
             ESP32_MODEM_VBAT_GPIO, ESP32_MODEM_VBAT_OFF_MS);

    gpio_set_level(ESP32_MODEM_VBAT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(ESP32_MODEM_VBAT_OFF_MS));
    gpio_set_level(ESP32_MODEM_VBAT_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* Press M_PWRKEY: idle high -> low for PWRKEY_MS -> high, then wait for boot. */
void esp32_modem_power_on(void)
{
    if (ESP32_MODEM_PWRKEY_GPIO >= 0) {
        ESP_LOGI(TAG, "Pressing M_PWRKEY (GPIO%d) for %d ms, wait %d ms",
                 ESP32_MODEM_PWRKEY_GPIO, ESP32_MODEM_PWRKEY_MS,
                 ESP32_MODEM_BOOT_WAIT_MS);

        gpio_set_level(ESP32_MODEM_PWRKEY_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(ESP32_MODEM_PWRKEY_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(ESP32_MODEM_PWRKEY_MS));
        gpio_set_level(ESP32_MODEM_PWRKEY_GPIO, 1);
    }

    vTaskDelay(pdMS_TO_TICKS(ESP32_MODEM_BOOT_WAIT_MS));
}

void esp32_modem_power_off(void)
{
    if (ESP32_MODEM_VBAT_GPIO >= 0) {
        ESP_LOGW(TAG, "Powering module off (M_VBAT_EN low)");
        gpio_set_level(ESP32_MODEM_VBAT_GPIO, 0);
    }
}

void esp32_modem_set_ready(bool ready)
{
    s_ready = ready;
}

bool esp32_modem_wait_ready(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while (s_ready == false) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

const char *esp32_modem_reset_reason_str(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external pin";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "panic/exception";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_DEEPSLEEP: return "deep sleep wakeup";
        case ESP_RST_USB:       return "USB peripheral reset";
        case ESP_RST_JTAG:      return "JTAG reset";
        case ESP_RST_EFUSE:     return "efuse error";
        case ESP_RST_PWR_GLITCH:return "power glitch";
        case ESP_RST_CPU_LOCKUP:return "CPU lockup";
        default:                return "unknown";
    }
}
