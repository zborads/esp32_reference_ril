# esp32_reference_ril
#
# AOSP reference-ril + ESP-IDF porting layer (USB CDC AT channel, RIL_Env,
# PPP over the modem data interface).
#
# The component is built as a normal ESP-IDF component. reference-ril is
# Android code: it uses GNU extensions (asprintf) and is compiled with the
# RIL_SHLIB flavour of ril.h, exactly like libril on Android.

idf_component_register(
    SRCS
        "src/reference-ril.c"
        "src/atchannel.c"
        "src/at_tok.c"
        "src/misc.c"
        "src/port/ril_compat.c"
        "src/port/ril_modem.c"
        "src/port/ril_transport.c"
        "src/port/ril_port.c"
        "src/port/ril_ppp.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
        "src/port"
        "src/port/include"
    REQUIRES
        esp_event
    PRIV_REQUIRES
        esp_driver_gpio
        esp_netif
        esp_system
        esp_timer
        espressif__iot_usbh_cdc
        lwip
        pthread
        usb
        vfs
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    _GNU_SOURCE
    RIL_SHLIB
)
