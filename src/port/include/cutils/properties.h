/*
 * ESP-IDF port of the Android "cutils/properties.h" shim for reference-ril.
 *
 * reference-ril only reads a couple of Android system properties; a small
 * in-RAM key/value store is enough for the port. Values can still be
 * overridden at runtime with property_set().
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROPERTY_KEY_MAX   32
#define PROPERTY_VALUE_MAX 92

/* Android code in reference-ril also uses the short name from
   sys/system_properties.h. */
#ifndef PROP_VALUE_MAX
#define PROP_VALUE_MAX PROPERTY_VALUE_MAX
#endif

int property_get(const char *key, char *value, const char *default_value);
int property_set(const char *key, const char *value);
int property_get_bool(const char *key, int default_value);
int32_t property_get_int32(const char *key, int32_t default_value);
int64_t property_get_int64(const char *key, int64_t default_value);

#ifdef __cplusplus
}
#endif
