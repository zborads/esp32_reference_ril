# reference-ril 移植说明（ESP32-P4 / ESP-IDF 5.5）

本组件把 AOSP 的 `hardware/ril/reference-ril` 移植到了 ESP-IDF，用它替换原来的
`espressif/iot_usbh_modem` 组件。可以整体拷贝到其他项目的 `components/` 目录复用。

## 目录结构

```
components/esp32_reference_ril/
├── include/                       对外 API
│   ├── esp32_ril.h                RIL 状态 / AT 通道 / ESP32_RIL_EVENT
│   ├── esp32_ppp.h                PPP 数据连接
│   └── esp32_modem.h              模组上电/复位接口
├── src/
│   ├── reference-ril.c / atchannel.c / at_tok.c / misc.c   AOSP 原始实现
│   └── port/
│       ├── include/              Android 兼容头（组件内部使用）
│       │   ├── utils/Log.h         Android RLOGx/ALOGx -> ESP_LOGx
│       │   ├── cutils/properties.h property_get 兼容层
│       │   ├── cutils/sockets.h    socket_local_client / socket_network_client
│       │   ├── sys/system_properties.h
│       │   ├── net/if.h            lwIP 下缺失的接口标志定义
│       │   └── telephony/          AOSP ril.h 依赖的头文件
│       ├── ril_compat.c            property / socket / librilutils 兼容实现
│       ├── ril_transport.c         USB CDC AT 通道 + VFS 字符设备
│       ├── ril_modem.c             模组复位/电源键上电时序
│       ├── ril_ppp.c               PPP 拨号、netif 桥接、自动重连
│       └── ril_port.c              RIL_Env 实现、定时回调、对外 API
├── docs/                           本说明（Quectel 手册不随仓库分发）
├── Kconfig                         menu "esp32 cellular (reference-ril + PPP)"
└── idf_component.yml               依赖 espressif/iot_usbh_cdc
```


## 数据通路

```
应用 (esp32_main_thread.c)
        │  esp32_ril_xxx()
        ▼
reference-ril (AT 引擎 / RIL 请求处理)
        │  open("/dev/esp32ril0") + read()/write()
        ▼
VFS 字符设备 (ril_transport.c)
        │  usbh_cdc_read_bytes()/usbh_cdc_write_bytes()
        ▼
USB 接口 3：AT（Quectel ECx00Z/EC801E/EG800Z）
```

`ril_port.c` 实现 `RIL_Env`：

* `RequestTimedCallback` 由 esp_timer 延迟后投递到 `ril_cb` 任务执行，
  避免在 esp_timer 任务里阻塞地发 AT 命令。
* 引用 `RIL_Init()` 启动 reference-ril 的 mainLoop。

`ril_port.c` 不再轮询 SIM（不发 `AT+CPIN?`）：SIM 状态直接来自
reference-ril 自己初始化后上报的 radio-state / SIM-status 通知，同时发出
`ESP32_RIL_EVENT_*` 事件：

| 事件 | 含义 |
| --- | --- |
| `ESP32_RIL_EVENT_DTE_CONN` / `DISCONN` | USB AT 通道连接 / 断开 |
| `ESP32_RIL_EVENT_SIM_CONN` | reference-ril 初始化完成、SIM 可用 |
| `ESP32_RIL_EVENT_NET_CONN` / `DISCONN` | PPP 获取 / 丢失 IP |

## 配置

AT 接口号通过 `esp32_ril_config_t.usb_itf_num` 指定，默认
`ESP32_RIL_DEFAULT_AT_ITF`（3）。

```c
esp32_ril_config_t config = ESP32_RIL_DEFAULT_CONFIG();
config.usb_itf_num = 4;
config.handler = on_modem_event;
esp32_ril_init(&config);
```

## 构建说明

`main/CMakeLists.txt` 中为 AOSP 源码增加了：

* `_GNU_SOURCE`（asprintf）
* `RIL_SHLIB`（使用 libril 的 `RIL_Env` 模式）

## 模组上电/复位（mount）

`ril_modem.c` 按 X10（ESP32-P4）IO 定义控制模组电源：

| 引脚 | 功能 |
| --- | --- |
| GPIO42 | **M_PWRKEY**：空闲高，拉低约 500 ms 开机 |
| GPIO43 | **M_VBAT_EN**：模块电源使能，**高有效** |

启动顺序（在 `esp32_ril_init` 创建的 `modem_bringup_task` 中）：

1. 配置 GPIO42/43，`M_VBAT_EN=1`，`M_PWRKEY=1`
2. 安装 USB Host/CDC 驱动并提前创建 AT(3)/PPP(4) 句柄
3. 通过 `M_VBAT_EN` 做一次电源循环：拉低 1 s → 拉高
4. 等待最多 5 s：如果模组自动枚举，**不按 PWRKEY**；
   如果未枚举，再给 `M_PWRKEY` 一个高→低 500 ms→高的开机脉冲，等待 8 s
5. 发 `AT` 验证 USB AT 口，然后启动 reference-ril

这些步骤在 `modem_up` 后台任务中执行，不会阻塞 UI。参数可在
menuconfig → “esp32 cellular (reference-ril + PPP)” 中调整。

## PPP 数据联网（双口模式）

Quectel ECx00Z / EC801E / EG800Z 的 USB 接口定义（见官方 USB 描述符文档）：

| 接口 | 功能 |
| --- | --- |
| 0 / 1 | ECM 网卡 |
| 2 | DIAG（Log） |
| **3** | **AT 命令** |
| **4** | **Modem（PPP / AT）** |

因此本移植固定使用**双口模式**：

- reference-ril 使用接口 **3**（AT）
- PPP 使用接口 **4**（Modem），独立 CDC 句柄
- 两者同时打开，互不挂起，PPP 建立期间 AT 命令仍然可用

```
应用 socket/SNTP
      │
lwIP + esp_netif PPP (PPP_DEF)
      │  esp_netif_receive() / transmit
ril_ppp.c（拨号 + CDC<->netif 桥接 + 断线重拨）
      │  usbh_cdc_handle_t（接口 4）
      ▼
USB 接口 4：Modem（PPP）

reference-ril（AT 引擎）
      │  /dev/esp32ril0 → usbh_cdc_handle_t（接口 3）
      ▼
USB 接口 3：AT
```

流程：

1. `AT` / `ATE0` / `AT+CGDCONT=1,"IP","<APN>"`
2. `ATD*99***1#`，等待 `CONNECT`
3. `esp_netif_action_start()` 启动 PPP，收到 `IP_EVENT_PPP_GOT_IP` 后
   设置默认 netif/DNS 并发出 `ESP32_RIL_EVENT_NET_CONN`
4. 断线（`IP_EVENT_PPP_LOST_IP` / PPP 错误事件 / USB 断开）后发送
   `+++` + `ATH` 挂断，延时 `retry_ms` 后自动重拨，并发出
   `ESP32_RIL_EVENT_NET_DISCONN`

配置项（`idf.py menuconfig` → “esp32 cellular (reference-ril + PPP)”）：

| 配置 | 默认 | 说明 |
| --- | --- | --- |
| `ESP32_RIL_AT_USB_ITF` | 3 | reference-ril 的 AT 接口 |
| `ESP32_PPP_USB_ITF` | 4 | PPP 的 Modem 接口（独立 CDC 句柄） |
| `ESP32_PPP_APN` | `cmnee` | APN |
| `ESP32_PPP_USERNAME` / `PASSWORD` | 空 | 可选 PAP 认证 |
| `ESP32_MODEM_POWER_GPIO` | 42 | 电源键 GPIO，-1 关闭 |
| `ESP32_MODEM_RESET_GPIO` | 43 | 复位 GPIO，-1 关闭 |
| `ESP32_RIL_AT_LOG` | n | 打印 AT 收发，确认走的是 USB AT 口 |

也可以在代码里直接填写 `esp32_ppp_config_t`。

## 已知限制

1. 当前按 Quectel ECx00Z / EC801E / EG800Z 的描述符固定使用接口 3（AT）和
   接口 4（PPP）。如果换成其它模组，需要在 menuconfig 中修改这两个接口号。
2. reference-ril 的 Android 专有路径（`/dev/qmi`、`netcfg`、rild socket 等）
   在 ESP32-P4 上不可用，相关代码保留但不参与运行。
3. `setInterfaceState()` 在 `ESP_PLATFORM` 下为空实现（lwIP 不支持
   `SIOCSIFFLAGS`）。

