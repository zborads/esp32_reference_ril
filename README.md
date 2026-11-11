# esp32_reference_ril

AOSP `reference-ril` ported to ESP-IDF, packaged as a reusable component.

It provides:

* **AT channel** — `libril`/`reference-ril` talking to the modem's USB CDC-ACM
  AT interface through a VFS character device (`/dev/esp32ril/0`).
* **RIL engine** — `RIL_Init()` + `RIL_Env` implementation (timed callbacks run
  on a dedicated FreeRTOS task, not on the `esp_timer` task).
* **PPP** — dials the modem data interface and bridges it to an `esp_netif`
  PPP netif (dual-port mode: AT on interface 3, data on interface 4).
* **Modem bring-up** — `M_VBAT_EN` / `M_PWRKEY` sequencing, USB host driver
  install and early CDC handle claiming.

The original AOSP sources are kept almost unchanged; everything ESP-specific is
in `src/port/`.

## Layout

```
esp32_reference_ril/
├── CMakeLists.txt
├── Kconfig                     menu "esp32 cellular (reference-ril + PPP)"
├── idf_component.yml           espressif/iot_usbh_cdc dependency
├── include/                    public API
│   ├── esp32_ril.h             RIL + AT channel API, ESP32_RIL_EVENT
│   ├── esp32_ppp.h             PPP API
│   └── esp32_modem.h           modem power/GPIO API
├── src/
│   ├── reference-ril.c         AOSP reference-ril (AT command set tuned for
│   │                           Quectel LTE Standard(E): no voice/USSD/CTEC)
│   ├── atchannel.c             AT channel (semaphore based response wait)
│   ├── at_tok.c  misc.c
│   ├── ril.h  atchannel.h  at_tok.h  misc.h
│   └── port/
│       ├── ril_port.c          RIL_Env, events, bring-up/monitor tasks
│       ├── ril_transport.c     USB CDC early claim + VFS device
│       ├── ril_ppp.c           PPP netif + dial
│       ├── ril_modem.c         M_PWRKEY / M_VBAT_EN
│       ├── ril_compat.c        cutils/telephony shims
│       └── include/            Android header shims (private)
└── docs/
    ├── PORTING.md              design notes
    └── *.pdf                   Quectel USB descriptor / AT command manuals
                                (vendor documentation, not distributed with
                                this repository)
```

## Adding it to another project

1. Copy `esp32_reference_ril/` into the project's `components/` directory
   (IDF discovers it automatically), or keep it elsewhere and add the parent
   directory to `EXTRA_COMPONENT_DIRS`.
2. Build once; the component manager pulls `espressif/iot_usbh_cdc`.
3. Optional defaults in `sdkconfig.defaults`:

```ini
CONFIG_ESP32_RIL_AT_USB_ITF=3
CONFIG_ESP32_PPP_USB_ITF=4
CONFIG_ESP32_PPP_APN="cmnet"
CONFIG_ESP32_MODEM_PWRKEY_GPIO=42
CONFIG_ESP32_MODEM_PWRKEY_MS=500
CONFIG_ESP32_MODEM_VBAT_GPIO=43
CONFIG_ESP32_MODEM_VBAT_OFF_MS=1000
CONFIG_ESP32_MODEM_BOOT_WAIT_MS=8000
CONFIG_ESP32_MODEM_LOG_RESET_REASON=y
CONFIG_ESP32_RIL_AT_LOG=y
```

Set a GPIO to `-1` when the board has no such line.

## Usage

```c
#include "esp_event.h"
#include "esp32_ril.h"
#include "esp32_ppp.h"

static void on_modem_event(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    switch (id) {
    case ESP32_RIL_EVENT_NET_CONN:
        /* PPP got an IP */
        break;
    default:
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp32_ril_config_t ril = ESP32_RIL_DEFAULT_CONFIG();
    ril.usb_itf_num = CONFIG_ESP32_RIL_AT_USB_ITF;
    ril.handler = on_modem_event;
    ESP_ERROR_CHECK(esp32_ril_init(&ril));

    esp32_ppp_config_t ppp = ESP32_PPP_DEFAULT_CONFIG();
    ppp.usb_itf_num = CONFIG_ESP32_PPP_USB_ITF;
    ppp.apn = CONFIG_ESP32_PPP_APN;
    ESP_ERROR_CHECK(esp32_ppp_init(&ppp));   /* starts after bring-up */
}
```

`esp32_ril_init()` runs the modem mount (GPIOs, USB driver, `M_VBAT_EN`
power cycle, PWRKEY, AT probe, `RIL_Init`) in a background task, so it does
not block `app_main()`.

## API summary

| Header | Functions |
| --- | --- |
| `esp32_ril.h` | `esp32_ril_init/deinit`, `esp32_ril_get_signal_quality`, `esp32_ril_get_iccid`, `esp32_ril_get_operator`, `esp32_ril_get_sim_state`, `esp32_ril_get_registration_state`, `esp32_ril_is_connected` |
| `esp32_ppp.h` | `esp32_ppp_init/deinit`, `esp32_ppp_is_connected`, `esp32_ppp_get_ip` |
| `esp32_modem.h` | `esp32_modem_hw_init`, `esp32_modem_reset_pulse`, `esp32_modem_power_on/off`, `esp32_modem_wait_ready` |

Events (`ESP32_RIL_EVENT`): `DTE_CONN/DISCONN`, `SIM_CONN/DISCONN`,
`NET_CONN/DISCONN`.

### SIM state

`AT+CPIN?` is executed **once**, right after the radio is switched on; there is
no polling. The result drives `ESP32_RIL_EVENT_SIM_CONN` / `SIM_DISCONN` and is
available through `esp32_ril_get_sim_state()`.

`+CME ERROR: <err>` is decoded according to the Quectel AT manual (5.3 + 12.5):

| CME | meaning | reported as |
| --- | --- | --- |
| 10 | SIM not inserted | SIM_DISCONN (absent) |
| 13 / 15 | SIM failure / SIM wrong | SIM_DISCONN (unusable) |
| 14 | SIM busy | SIM_DISCONN (not ready) |
| 11 / 12 / 17 / 18 | PIN / PUK required | SIM_DISCONN (locked) |
| 5 / 6 / 7 / 40…47 | network / personalization lock | SIM_DISCONN (locked) |
| other | any other ME error / timeout | SIM_DISCONN (not ready) |

`+CPIN: READY` is the only case that produces `SIM_CONN`.

### SIM hot-plug

`AT+QSIMSTAT=1` is sent during `initializeCallback()` so the module reports
`+QSIMSTAT: <enable>,<inserted_status>` (manual 5.11 / 12.7):

| URC | action |
| --- | --- |
| `+QSIMSTAT: 1,0` (removed) | cached state = not ready → `SIM_DISCONN` |
| `+QSIMSTAT: 1,1` (inserted) | one `AT+CPIN?` is scheduled to verify the card |
| `+QSIMSTAT: 1,2` (unknown) | ignored |
| `+CPIN: NOT READY` / `+CPIN: READY` | reported directly (`SIM_DISCONN` / `SIM_CONN`) |

Only the first query at boot and one per insertion use `AT+CPIN?`; removal never
queries the SIM. `AT+QSIMSTAT=1` must be processed after the initial query, so
URCs received during initialization are ignored (`cached == -1`).

> Some modules additionally require `AT+QSIMDET=1,<insert_level>` (manual 5.10)
> before any `+QSIMSTAT`/`+CPIN` URC is generated. `<insert_level>` must match
> the board's SIM-detect pin (0 = low when the card is inserted), so it is not
> sent by default.

## Notes

* The AT command set in `src/reference-ril.c` is trimmed to what the Quectel
  LTE Standard(E) modules (EG800Z/EC800Z/EC801E) support. Voice, USSD, CDMA
  and TI/Sierra commands were removed; those RIL requests return
  `RIL_E_REQUEST_NOT_SUPPORTED`.
* `pthread_self()` is not called on non-pthread tasks (would assert in
  ESP-IDF), and the AT response wait uses a FreeRTOS semaphore instead of
  `pthread_cond_timedwait()`. See `docs/PORTING.md`.
* The module is assumed to use the Quectel USB layout: if0/1 ECM, if2 DIAG,
  if3 AT, if4 Modem (PPP). Single-port (shared AT/data) mode is not used.
