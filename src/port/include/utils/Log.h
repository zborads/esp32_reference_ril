/*
 * ESP-IDF port of the Android "utils/Log.h" shim for reference-ril.
 *
 * The upstream reference-ril sources use RLOGx/ALOGx macros with LOG_TAG.
 * Map them onto ESP-IDF logging so the code compiles unchanged.
 */
#pragma once

#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"

#ifndef LOG_TAG
#define LOG_TAG "RIL"
#endif

#if CONFIG_ESP32_RIL_AT_LOG
#define RLOGV(fmt, ...) ESP_LOGV(LOG_TAG, fmt, ##__VA_ARGS__)
#define RLOGD(fmt, ...) ESP_LOGI(LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define RLOGV(fmt, ...) ESP_LOGV(LOG_TAG, fmt, ##__VA_ARGS__)
#define RLOGD(fmt, ...) ESP_LOGD(LOG_TAG, fmt, ##__VA_ARGS__)
#endif
#define RLOGI(fmt, ...) ESP_LOGI(LOG_TAG, fmt, ##__VA_ARGS__)
#define RLOGW(fmt, ...) ESP_LOGW(LOG_TAG, fmt, ##__VA_ARGS__)
#define RLOGE(fmt, ...) ESP_LOGE(LOG_TAG, fmt, ##__VA_ARGS__)

#define ALOGV(fmt, ...) RLOGV(fmt, ##__VA_ARGS__)
#define ALOGD(fmt, ...) RLOGD(fmt, ##__VA_ARGS__)
#define ALOGI(fmt, ...) RLOGI(fmt, ##__VA_ARGS__)
#define ALOGW(fmt, ...) RLOGW(fmt, ##__VA_ARGS__)
#define ALOGE(fmt, ...) RLOGE(fmt, ##__VA_ARGS__)

#define LOG_ALWAYS_FATAL(...)                                                    \
    do {                                                                         \
        ESP_LOGE(LOG_TAG, __VA_ARGS__);                                          \
        abort();                                                                 \
    } while (0)
