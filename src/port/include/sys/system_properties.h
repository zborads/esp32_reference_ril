/*
 * ESP-IDF port of the Android "sys/system_properties.h" shim.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROP_NAME_MAX  32
#ifndef PROP_VALUE_MAX
#define PROP_VALUE_MAX 92
#endif

int __system_property_get(const char *key, char *value);
int __system_property_set(const char *key, const char *value);

#ifdef __cplusplus
}
#endif
