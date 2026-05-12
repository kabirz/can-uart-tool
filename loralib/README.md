# LoRa Gateway SDK 使用指南

**版本**: 1.0.0
**许可证**: Apache-2.0
**平台**: Windows (Winsock2)
**语言**: C99

## 概述

LoRa Gateway SDK 提供 LoRa 网关（USR-LG210-L）的 TCP 数据流通信与 UDP 设备发现/配置功能。SDK 无 Win32 GUI 依赖，所有事件通知通过回调函数传递，可轻松集成到任何 C/C++ 应用程序中。

### 核心功能

| 功能 | 协议 | 说明 |
|------|------|------|
| 数据帧收发 | TCP | 与 LoRa 终端设备进行双向二进制帧通信 |
| 设备发现 | UDP | 局域网广播搜索 USR-LG210-L 网关 |
| 网络参数查询 | UDP | 获取网关 IP / 子网掩码 / 默认网关 |
| AT 指令透传 | UDP | 远程配置 LoRa 模块参数（速率/信道/功率等） |

### 线程模型

```
调用线程 (UI)          SDK 后台线程
    |                      |
    |-- lora_sdk_connect --|---> connect_watcher 线程
    |                      |        |
    |                      |        +-- 连接成功 --> tcp_recv_worker 线程
    |                      |                      |
    |    on_conn_state <---|                      +--> 帧解析 --> on_frame / on_log / on_hex_dump
    |                      |
    |-- lora_sdk_send_at --|---> udp_worker 线程
    |                      |        |
    |    on_at_response <--|        +--> JSON 解析 --> on_device_found / on_net_params / on_at_response
```

- 所有回调在 SDK 后台线程中触发
- 回调中禁止调用 `lora_sdk_connect`、`lora_sdk_disconnect`、`lora_sdk_send_frame` 等阻塞 I/O 函数，否则会死锁
- GUI 应用需将回调数据编组到 UI 线程（如 Win32 `PostMessage`）

---

## 快速开始

### 集成方式

SDK 提供两种集成方式：

#### 方式一：动态链接库（DLL）

将 `lora_gateway_sdk.dll` 和 `lora_sdk.h` 分发给客户。

```c
/* 无需定义任何宏，头文件自动生成 __declspec(dllimport) */
#include "lora_sdk.h"
```

#### 方式二：静态链接库

将 `lora_sdk.lib`（MSVC）或 `liblora_sdk.a`（MinGW）与 `lora_sdk.h` 一起集成到项目中。

```c
/* 必须在包含头文件前定义 LORA_SDK_STATIC */
#define LORA_SDK_STATIC
#include "lora_sdk.h"
```

### 编译依赖

| 库 | 说明 |
|----|------|
| `ws2_32` | Windows Sockets（TCP/UDP） |
| `iphlpapi` | 网卡枚举（UDP 广播） |

### CMake 集成示例

```cmake
# 方式一：子目录集成（静态库）
add_subdirectory(loralib)
target_compile_definitions(myapp PRIVATE LORA_SDK_STATIC)
target_link_libraries(myapp PRIVATE lora_sdk_static)

# 方式二：链接 DLL 导入库
target_link_libraries(myapp PRIVATE lora_gateway_sdk.lib ws2_32 iphlpapi)
```

### 最小示例

完整源码见 `lora_sdk_example.c`，以下为核心代码说明。

#### 回调实现

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lora_sdk.h"

#ifdef _WIN32
#include <windows.h>
static HANDLE g_connEvent      = NULL;
static HANDLE g_searchEvent    = NULL;
static HANDLE g_netParamsEvent = NULL;
#endif

static char g_gatewayIp[64] = "";
static lora_sdk_t *g_sdk = NULL;
static volatile uint32_t g_pendingRssiNid = 0;

/* 连接状态回调 */
static void on_conn_state(void *ud, enum lora_sdk_conn_state state)
{
    const char *names[] = {"DISCONNECTED", "CONNECTING", "CONNECTED"};
    printf("[连接状态] %s\n", names[state]);
#ifdef _WIN32
    if (state == LORA_SDK_CONN_CONNECTED)
        SetEvent(g_connEvent);
#endif
}

/* 帧接收回调 — 按类型分区处理 */
static void on_frame(void *ud, uint32_t nid,
                     const uint8_t *payload, uint16_t len)
{
    if (!payload || len == 0) return;
    uint8_t type = payload[0];
    const uint8_t *body = payload + 1;
    uint16_t body_len = len - 1;

    switch (type) {
    case 0x01: /* 手柄遥测帧 */
        if (body_len == 8 &&
            body[5] == 0xFF && body[6] == 0xFF && body[7] == 0xFF) {
            int16_t x = (int16_t)((uint16_t)body[0] << 8 | body[1]);
            int16_t y = (int16_t)((uint16_t)body[2] << 8 | body[3]);
            uint8_t btn = body[4] & 0x01;
            printf("[手柄] NID=0x%08X  X=%d, Y=%d, 按键=%s\n",
                   nid, x, y, btn ? "松开" : "按下");
        } else if (len >= LORA_SCANNER_FRAME_SIZE) {
            lora_scanner_data_t scan;
            if (lora_scanner_parse(payload, len, &scan) == 0) {
                printf("[扫描仪] 超欠挖=%d 激光=%u X=%d Y=%d Z=%d\n",
                       scan.overbreak, scan.laser,
                       scan.coord_x, scan.coord_y, scan.coord_z);
            }
        }
        break;

    case 0x02: /* 测试帧 — 回显 */
        printf("[测试] NID=0x%08X, len=%u\n", nid, len);
        lora_sdk_send_frame(g_sdk, nid, payload, len); /* echo back */
        break;

    case 0x03: /* RSSI 请求 — 查询信号强度后回复 */
        printf("[RSSI] NID=0x%08X\n", nid);
        g_pendingRssiNid = nid;
        lora_sdk_send_at(g_sdk, "AT+NINFO?");
        break;
    }
}

/* AT 响应回调 — 解析 NINFO 并发送 RSSI 响应 */
static void on_at_response(void *ud, const char *resp)
{
    printf("[AT响应] %s\n", resp);
    if (g_pendingRssiNid != 0 && g_sdk) {
        const char *p = strstr(resp, "+NINFO:");
        if (p) {
            int snr_val = 0, rssi_val = -120;
            int field_idx = 0;
            const char *cur = p + 7;
            while (*cur && *cur != '\r' && *cur != '\n' && field_idx < 5) {
                field_idx++;
                const char *endp = cur;
                while (*endp && *endp != ',' && *endp != '\r' && *endp != '\n')
                    endp++;
                int flen = (int)(endp - cur);
                if (field_idx == 4 && flen > 0) snr_val = atoi(cur);
                if (field_idx == 5 && flen > 0) rssi_val = atoi(cur);
                if (*endp != ',') break;
                cur = endp + 1;
            }
            uint8_t snr_raw  = (uint8_t)(int8_t)snr_val;
            uint8_t rssi_raw = (uint8_t)(int8_t)rssi_val;
            lora_sdk_send_rssi_response(g_sdk, g_pendingRssiNid,
                                        snr_raw, rssi_raw, 0);
            g_pendingRssiNid = 0;
        }
    }
}

/* 网络参数回调 — 自动保存网关 IP 用于 TCP 连接 */
static void on_net_params(void *ud, const char *ip,
                          const char *mask, const char *gw)
{
    printf("[网络参数] IP=%s, 掩码=%s, 网关=%s\n", ip, mask, gw);
    if (gw && gw[0])
        strncpy(g_gatewayIp, gw, sizeof(g_gatewayIp) - 1);
#ifdef _WIN32
    SetEvent(g_netParamsEvent);
#endif
}

static void on_device_found(void *ud, const char *mac,
                            const char *name, const char *sw, const char *ip)
{
    printf("[设备发现] MAC=%s, 设备=%s, SW=%s, IP=%s\n", mac, name, sw, ip);
#ifdef _WIN32
    SetEvent(g_searchEvent);
#endif
}

static void on_log(void *ud, const char *msg, enum lora_sdk_log_source src) { printf("[%s] %s\n", src == LORA_SDK_LOG_TCP ? "TCP" : "UDP", msg); }
static void on_hex_dump(void *ud, const char *prefix,
                        const uint8_t *data, int len) { (void)ud; (void)prefix; (void)data; (void)len; }
static void on_error(void *ud, const char *msg) { fprintf(stderr, "[错误] %s\n", msg); }
```

#### 主程序流程

```c
int main(void)
{
#ifdef _WIN32
    g_connEvent      = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_searchEvent    = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_netParamsEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
#endif

    /* 1. 注册回调 */
    lora_sdk_callbacks_t cbs = {0};
    cbs.on_conn_state   = on_conn_state;
    cbs.on_frame        = on_frame;
    cbs.on_device_found = on_device_found;
    cbs.on_at_response  = on_at_response;
    cbs.on_net_params   = on_net_params;
    cbs.on_log          = on_log;
    cbs.on_hex_dump     = on_hex_dump;
    cbs.on_error        = on_error;

    /* 2. 初始化 SDK */
    lora_sdk_t *sdk = lora_sdk_init(&cbs, NULL);
    g_sdk = sdk;

    /* 3. 搜索网关 → 查询网络参数 → 自动获取网关 IP */
    lora_sdk_search_devices(sdk);
#ifdef _WIN32
    WaitForSingleObject(g_searchEvent, 6000);
    ResetEvent(g_searchEvent);
#endif
    lora_sdk_get_net_params(sdk);
#ifdef _WIN32
    WaitForSingleObject(g_netParamsEvent, 6000);
    ResetEvent(g_netParamsEvent);
#endif

    /* 4. TCP 连接网关（使用查询到的网关 IP） */
    lora_sdk_connect(sdk, g_gatewayIp, 8899);
#ifdef _WIN32
    WaitForSingleObject(g_connEvent, 15000);
    ResetEvent(g_connEvent);
#endif

    /* 5. 发送扫描仪帧 */
    lora_scanner_data_t scan;
    memset(&scan, 0, sizeof(scan));
    scan.overbreak_valid = 1; scan.laser_valid = 1;
    scan.coord_z_valid = 1;   scan.coord_xy_valid = 1;
    scan.overbreak = 42; scan.laser = 12345;
    scan.coord_x = 100; scan.coord_y = -200; scan.coord_z = 300;
    uint8_t buf[LORA_SCANNER_FRAME_SIZE];
    lora_scanner_pack(buf, sizeof(buf), &scan);
    lora_sdk_send_frame(sdk, 0x00000001, buf, sizeof(buf));

    /* 6. UDP 查询 LoRa 参数 */
    lora_sdk_send_at(sdk, "AT+SPD?");
    lora_sdk_send_at(sdk, "AT+CSQ?");

    /* 7. 断开并清理 */
    lora_sdk_disconnect(sdk);
    lora_sdk_cleanup(sdk);
    return 0;
}
```

#### 交互式命令

完整示例程序支持交互式命令行操作：

| 命令 | 说明 |
|------|------|
| `s` | 搜索网关设备 |
| `c` | 自动获取网关 IP 并 TCP 连接 |
| `d` | 断开连接 |
| `f` | 发送测试帧 |
| `r` | 发送扫描仪帧 |
| `n` | 查询网络参数 |
| `a <cmd>` | 发送 AT 指令 |
| `q` | 退出 |

#### 编译运行

示例程序默认使用动态链接（DLL），运行时需将 `lora_gateway_sdk.dll` 放在同目录或 PATH 中：

```bash
# CMake 构建
cmake --build build --target lora_sdk_example

# 手动编译（MSVC）
cl lora_sdk_example.c lora_gateway_sdk.lib ws2_32.lib iphlpapi.lib
```

---

## API 参考

### 数据类型

#### `lora_sdk_t`

SDK 实例不透明句柄，由 `lora_sdk_init()` 创建，`lora_sdk_cleanup()` 销毁。

#### `enum lora_sdk_conn_state`

| 值 | 名称 | 说明 |
|----|------|------|
| 0 | `LORA_SDK_CONN_DISCONNECTED` | 未连接 |
| 1 | `LORA_SDK_CONN_CONNECTING` | 正在连接（TCP 非阻塞握手进行中） |
| 2 | `LORA_SDK_CONN_CONNECTED` | 已连接，可收发数据帧 |

#### `lora_scanner_data_t`

扫描仪合并帧解析结果。合并帧 payload 固定 20 字节：

```
偏移  长度  字段        说明
[0]   1    type        0x01 (LORA_DATA_HANDLER)
[1]   1    flags       有效性位掩码
[2]   2    overbreak   int16_t BE (超欠挖)
[4]   4    laser       uint32_t BE (激光测距)
[8]   4    coord_x     int32_t BE (X 坐标)
[12]  4    coord_y     int32_t BE (Y 坐标)
[16]  4    coord_z     int32_t BE (Z 坐标)
```

**flags 位定义：**

| 位 | 掩码 | 说明 |
|----|------|------|
| 0 | `LORA_SCANNER_F_OVERBREAK` | 超欠挖数据有效 |
| 1 | `LORA_SCANNER_F_LASER` | 激光测距数据有效 |
| 2 | `LORA_SCANNER_F_COORD_Z` | Z 坐标有效 |
| 3 | `LORA_SCANNER_F_COORD_XY` | X/Y 坐标有效 |

### 回调接口

#### `lora_sdk_callbacks_t`

```c
typedef struct {
    void (*on_conn_state)(void *ud, enum lora_sdk_conn_state state);
    void (*on_frame)(void *ud, uint32_t nid,
                     const uint8_t *payload, uint16_t payload_len);
    void (*on_device_found)(void *ud, const char *mac,
                            const char *device_name,
                            const char *sw_version,
                            const char *from_ip);
    void (*on_at_response)(void *ud, const char *at_response);
    void (*on_net_params)(void *ud, const char *ip,
                          const char *mask, const char *gateway);
    void (*on_log)(void *ud, const char *message,
                   enum lora_sdk_log_source source);
    void (*on_hex_dump)(void *ud, const char *prefix,
                        const uint8_t *data, int len);
    void (*on_error)(void *ud, const char *message);
} lora_sdk_callbacks_t;
```

**回调详情：**

| 回调 | 触发时机 | 参数说明 |
|------|---------|---------|
| `on_conn_state` | TCP 连接状态变化 | `state`: 新状态 |
| `on_frame` | 收到有效数据帧（payload 非空） | `nid`: 节点 ID；`payload[0]`: 数据类型（0x01=HANDLER, 0x02=TEST, 0x03=RSSI）；`payload` 仅在回调期间有效 |
| `on_device_found` | UDP 搜索发现设备 | `mac`: MAC 地址；`device_name`: 设备名称；`sw_version`: 固件版本；`from_ip`: 设备 IP |
| `on_at_response` | AT 指令响应 | `at_response`: 原始响应文本 |
| `on_net_params` | 网络参数查询结果 | `ip`/`mask`/`gateway`: 网络配置 |
| `on_log` | 日志消息 | `message`: 文本日志, `source`: `LORA_SDK_LOG_TCP` / `LORA_SDK_LOG_UDP` |
| `on_hex_dump` | 原始收发数据 | `prefix`: "TX"/"RX"；`data`/`len`: 原始字节 |
| `on_error` | 错误通知 | `message`: 错误描述 |

> **重要**：所有回调均可设为 NULL（不注册）。回调在后台线程中触发，回调内禁止调用 SDK 的连接/发送函数。

---

### 生命周期

#### `lora_sdk_init`

```c
lora_sdk_t *lora_sdk_init(const lora_sdk_callbacks_t *callbacks, void *user_data);
```

初始化 SDK 实例。

| 参数 | 说明 |
|------|------|
| `callbacks` | 回调函数集，不可为 NULL |
| `user_data` | 用户数据指针，传递给所有回调的 `ud` 参数 |
| **返回值** | SDK 实例句柄，失败返回 NULL |

> 静态链接时自动初始化 Winsock（引用计数管理，多实例安全）。DLL 版本在 `DllMain` 中初始化。

#### `lora_sdk_cleanup`

```c
void lora_sdk_cleanup(lora_sdk_t *sdk);
```

断开连接、等待后台线程退出、释放所有资源。

> 调用后 `sdk` 指针不可再使用。

---

### TCP 操作

#### `lora_sdk_connect`

```c
void lora_sdk_connect(lora_sdk_t *sdk, const char *ip, int port);
```

非阻塞连接 LoRa 网关。立即返回，连接结果通过 `on_conn_state` 回调通知。

| 参数 | 说明 |
|------|------|
| `ip` | 网关 IP 地址（如 `"192.168.1.254"`） |
| `port` | 网关 TCP 端口（USR-LG210-L 默认 `8899`） |

**流程**：
1. 立即触发 `on_conn_state(CONNECTING)`
2. 后台线程完成 TCP 握手（超时 10 秒）
3. 成功：触发 `on_conn_state(CONNECTED)`，启动接收线程
4. 失败：触发 `on_conn_state(DISCONNECTED)`

#### `lora_sdk_disconnect`

```c
void lora_sdk_disconnect(lora_sdk_t *sdk);
```

断开 TCP 连接，停止接收线程。触发 `on_conn_state(DISCONNECTED)`。

#### `lora_sdk_conn_state`

```c
enum lora_sdk_conn_state lora_sdk_conn_state(lora_sdk_t *sdk);
```

查询当前连接状态（线程安全，原子读取）。

#### `lora_sdk_send_frame`

```c
void lora_sdk_send_frame(lora_sdk_t *sdk, uint32_t nid,
                          const uint8_t *data, uint16_t data_len);
```

向网关发送数据帧。SDK 自动完成组帧（NID + Length + Data + CRC16 + 帧头帧尾封装）。

| 参数 | 说明 |
|------|------|
| `nid` | 目标节点 ID（4 字节，大端序） |
| `data` | 数据载荷（不含 NID 和 Length） |
| `data_len` | 数据长度（字节） |

**TCP 发送帧格式**（SDK 自动封装，调用者无需关心）：

```
[NID 4B][0xAA 0x55][NID 4B][Length 2B][Data NB][CRC16 2B][\r\n]
```

#### `lora_sdk_send_rssi_response`

```c
void lora_sdk_send_rssi_response(lora_sdk_t *sdk, uint32_t nid,
                                  uint8_t snr_raw, uint8_t rssi_raw,
                                  uint8_t test_flag);
```

发送 RSSI 信号强度响应帧（payload 类型 0x03）。

| 参数 | 说明 |
|------|------|
| `nid` | 目标节点 ID |
| `snr_raw` | SNR 原始值（有符号，直接转 uint8_t） |
| `rssi_raw` | RSSI 原始值（有符号，如 -50 → `(uint8_t)(int8_t)-50`） |
| `test_flag` | 测试模式标志（0=正常, 1=测试模式） |

---

### UDP 操作

#### `lora_sdk_search_devices`

```c
void lora_sdk_search_devices(lora_sdk_t *sdk);
```

在所有活跃 IPv4 网卡上广播搜索 USR-LG210-L 网关设备。结果通过 `on_device_found` 回调通知，5 秒超时。

> 搜索成功后 SDK 自动记录设备 MAC 地址，后续 UDP 操作无需再次搜索。

#### `lora_sdk_get_net_params`

```c
void lora_sdk_get_net_params(lora_sdk_t *sdk);
```

查询网关的网络参数（IP / 子网掩码 / 默认网关）。结果通过 `on_net_params` 回调通知。

> **前置条件**：需先调用 `lora_sdk_search_devices()` 成功发现设备。

#### `lora_sdk_send_at`

```c
void lora_sdk_send_at(lora_sdk_t *sdk, const char *at_cmd);
```

向网关发送 AT 指令。SDK 自动判断指令类型：
- 以 `?` 结尾 → 查询指令（GETPARA）
- 其他 → 设置指令（SETPARA）

结果通过 `on_at_response` 回调通知。

| 参数 | 说明 |
|------|------|
| `at_cmd` | AT 指令字符串（如 `"AT+SPD?"`、`"AT+CH1=4700"`） |

> **前置条件**：需先调用 `lora_sdk_search_devices()` 成功发现设备。
> **超时**：5 秒内未收到响应触发日志 `"No response (timeout 5s)"`。

---

### 工具函数

#### `lora_sdk_build_frame`

```c
int lora_sdk_build_frame(uint8_t *out, size_t out_size,
                          uint32_t nid,
                          const uint8_t *data, uint16_t data_len);
```

手动构建统一帧（不含 0xAA 0x55 帧头和 \r\n 帧尾）。

| 参数 | 说明 |
|------|------|
| `out` | 输出缓冲区 |
| `out_size` | 缓冲区大小 |
| `nid` | 节点 ID |
| `data` | 数据载荷 |
| `data_len` | 数据长度 |
| **返回值** | 帧总字节数，失败返回 -1 |

**输出格式**：`[NID 4B BE][Length 2B BE][Data NB][CRC16 2B BE]`

#### `lora_scanner_parse`

```c
int lora_scanner_parse(const uint8_t *payload, uint16_t len,
                       lora_scanner_data_t *out);
```

解析扫描仪合并帧 payload 为结构体（`static inline`，定义在头文件中）。

| 参数 | 说明 |
|------|------|
| `payload` | 帧数据（含类型字节，即 `on_frame` 中的 payload） |
| `len` | payload 长度 |
| `out` | 输出结构体 |
| **返回值** | 0 成功，-1 失败（长度不足或类型不匹配） |

#### `lora_scanner_pack`

```c
int lora_scanner_pack(uint8_t *buf, size_t size,
                      const lora_scanner_data_t *s);
```

将扫描仪结构体打包为 20 字节 payload（`static inline`，定义在头文件中）。

| 参数 | 说明 |
|------|------|
| `buf` | 输出缓冲区（至少 20 字节） |
| `size` | 缓冲区大小 |
| `s` | 扫描仪数据结构体 |
| **返回值** | 写入字节数（20），失败返回 -1 |

#### 字节序辅助函数

```c
void     lora_put_be16(uint8_t *buf, uint16_t val);    /* 写入 16 位大端 */
void     lora_put_be32(uint8_t *buf, uint32_t val);    /* 写入 32 位大端 */
uint16_t lora_get_be16(const uint8_t *buf);            /* 读取 16 位大端 */
uint32_t lora_get_be32(const uint8_t *buf);            /* 读取 32 位大端 */
```

均为 `static inline`，定义在头文件中，无链接依赖。

---

## 帧协议

### 统一帧格式

所有 LoRa 数据帧使用统一格式，多字节字段均为大端序（网络字节序）：

```
┌──────────┬────────────┬──────────┬──────────┐
│ NID 4B   │ Length 2B  │ Data NB  │ CRC16 2B │
│ (BE)     │ (BE)       │          │ (BE)     │
└──────────┴────────────┴──────────┴──────────┘
CRC 覆盖范围: NID + Length + Data
```

| 字段 | 大小 | 说明 |
|------|------|------|
| NID | 4 字节 | 节点 ID，大端序 |
| Length | 2 字节 | Data 字段长度 |
| Data | N 字节 | 载荷数据 |
| CRC16 | 2 字节 | CRC16-CCITT，覆盖 NID + Length + Data |

**TCP 封装**：实际 TCP 传输额外封装帧头帧尾：

```
[0xAA][0x55]  [统一帧内容]  [\r\n]
```

**TCP 发送时额外添加 NID 前缀**（网关协议要求）：

```
[NID 4B]  [0xAA][0x55]  [统一帧]  [\r\n]
```

### 数据类型

Data 首字节为类型标识：

| 类型 | 值 | 方向 | Data 内容 | 说明 |
|------|----|------|----------|------|
| HANDLER | 0x01 | 双向 | 详见下方 | 遥测/扫描仪数据 |
| TEST | 0x02 | 双向 | 测试帧内容 | 测试模式数据 |
| RSSI | 0x03 | 网关→设备 | `[0x03][SNR 1B][RSSI 1B][test_flag 1B]` | 信号强度响应 |

### HANDLER 帧格式（扫描仪合并帧，Data 20 字节）

```
[0x01][flags 1B][overbreak 2B BE][laser 4B BE][coord_x 4B BE][coord_y 4B BE][coord_z 4B BE]
```

### ACK 帧

空载荷帧（Length = 0）为心跳 ACK，SDK 内部处理，不触发 `on_frame` 回调。

---

## 常用 AT 指令

以下 AT 指令通过 `lora_sdk_send_at()` 发送，响应通过 `on_at_response` 回调接收。

### 通信参数

| 指令 | 说明 | 示例 |
|------|------|------|
| `AT+LORAPROT?` | 查询通信协议 | 响应: `+LORAPROT:1` (LG210) |
| `AT+LORAPROT=X` | 设置通信协议 | `AT+LORAPROT=1` (0=NODE, 1=LG210, 2=LG220) |
| `AT+WMODE?` | 查询工作模式 | 响应: `+WMODE:1` (TRANS) |
| `AT+WMODE=X` | 设置工作模式 | `AT+WMODE=1` (0=FP, 1=TRANS, 2=NET) |
| `AT+SPD?` | 查询速率等级 | 响应: `+SPD:7` |
| `AT+SPD=X` | 设置速率等级 | `AT+SPD=7` (范围 4-11) |
| `AT+CH1?` / `AT+CH2?` | 查询通道频率 | 响应: `+CH1:4700` |
| `AT+CH1=X` / `AT+CH2=X` | 设置通道频率 | `AT+CH1=4700` (范围 4100-5100, 步进 100) |
| `AT+PWR?` | 查询发射功率 | 响应: `+PWR:27` |
| `AT+PWR=X` | 设置发射功率 | `AT+PWR=27` (范围 24-30 dBm) |

### 节点/网关 ID

| 指令 | 说明 | 示例 |
|------|------|------|
| `AT+NID?` | 查询节点 ID | 响应: `NID:00000001` |
| `AT+NID=XXXXXXXX` | 设置节点 ID | `AT+NID=00000001` (8 位十六进制) |
| `AT+GWID?` | 查询网关 ID | 响应: `GWID:00000001` |
| `AT+GWID=XXXXXXXX` | 设置网关 ID | `AT+GWID=00000001` (组网模式有效) |
| `AT+UPWID?` | 查询 UPWID 状态 | — |
| `AT+UPWID=ON` / `OFF` | 开关 UPWID | — |

### 网络/系统

| 指令 | 说明 | 示例 |
|------|------|------|
| `AT+VER?` | 查询固件版本 | — |
| `AT+CSQ?` | 查询信号强度 | 响应: `+CSQ:25` |
| `AT+DHCP?` | 查询 DHCP 状态 | 响应: `+DHCP:ON` |
| `AT+DHCP=ON` / `OFF` | 开关 DHCP | — |
| `AT+GWIP?` / `AT+GWIP=x.x.x.x` | 查询/设置 IP | — |
| `AT+MASK?` / `AT+MASK=x.x.x.x` | 查询/设置子网掩码 | — |
| `AT+GW?` / `AT+GW=x.x.x.x` | 查询/设置默认网关 | — |

> 完整 AT 指令集请参考 USR-LG210-L 协议说明书。

---

## Win32 GUI 集成示例

以下展示在 Win32 应用中将 SDK 回调编组到 UI 线程的模式：

```c
/* 自定义窗口消息 */
#define WM_LORA_CONN_STATE   (WM_APP + 10)
#define WM_LORA_FRAME        (WM_APP + 11)
#define WM_LORA_AT_RESPONSE  (WM_APP + 13)

/* 回调中将数据 PostMessage 到窗口 */
static void cb_on_conn_state(void *ud, enum lora_sdk_conn_state state)
{
    HWND hwnd = (HWND)ud;
    PostMessageW(hwnd, WM_LORA_CONN_STATE, (WPARAM)state, 0);
}

static void cb_on_frame(void *ud, uint32_t nid,
                         const uint8_t *payload, uint16_t len)
{
    HWND hwnd = (HWND)ud;
    /* 拷贝到堆内存，接收端负责 free */
    uint8_t *copy = (uint8_t *)malloc(len);
    if (copy) {
        memcpy(copy, payload, len);
        PostMessageW(hwnd, WM_LORA_FRAME, MAKEWPARAM(nid & 0xFFFF, len),
                     (LPARAM)copy);
    }
}

static void cb_on_at_response(void *ud, const char *resp)
{
    HWND hwnd = (HWND)ud;
    char *copy = _strdup(resp);
    if (copy)
        PostMessageW(hwnd, WM_LORA_AT_RESPONSE, 0, (LPARAM)copy);
}

/* WndProc 中处理自定义消息 */
case WM_LORA_CONN_STATE:
    /* wParam = conn_state 枚举值 */
    UpdateConnectionUI((int)wParam);
    break;

case WM_LORA_FRAME:
    /* lParam = 堆分配的 payload 拷贝 */
    {
        uint8_t *data = (uint8_t *)lParam;
        uint16_t len = HIWORD(wParam);
        ProcessFrame(data, len);
        free(data);  /* 必须释放 */
    }
    break;

case WM_LORA_AT_RESPONSE:
    /* lParam = strdup'd 响应字符串 */
    {
        char *resp = (char *)lParam;
        ParseAtResponse(resp);
        free(resp);  /* 必须释放 */
    }
    break;
```

---

## 构建说明

### 从源码构建

```bash
# MinGW
cmake -B build -G "MinGW Makefiles"
cmake --build build

# MSVC
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

### 构建产物

| 目标 | 产物 | 说明 |
|------|------|------|
| `lora_sdk_static` | `lora_sdk.lib` / `liblora_sdk.a` | 静态库，需定义 `LORA_SDK_STATIC` |
| `lora_sdk_shared` | `lora_gateway_sdk.dll` | 动态库，`DllMain` 自动管理 Winsock |

### 分发文件清单（DLL 方式）

```
lora_sdk.h                  -- 公共头文件（唯一需要的头文件）
lora_gateway_sdk.dll        -- 动态链接库
lora_gateway_sdk.lib        -- 导入库（MSVC 链接用）
```

### 依赖库

SDK 内部集成了以下库，客户无需额外获取：
- **cJSON** — JSON 解析（UDP 响应）
- **CRC16-CCITT** — 帧校验

---

## 注意事项

1. **线程安全**：`lora_sdk_conn_state()` 是线程安全的（原子操作）。其余 API 函数应从同一线程调用。
2. **回调禁止阻塞**：回调在 SDK 工作线程中执行，长时间阻塞会影响帧接收和心跳检测。
3. **内存管理**：`on_frame` 中的 `payload` 指针仅在回调期间有效，如需保留请拷贝。
4. **重连**：断开后可直接再次调用 `lora_sdk_connect()`，无需重新初始化 SDK。
5. **多实例**：支持创建多个 `lora_sdk_t` 实例连接不同网关。
6. **DLL 线程安全**：DLL 版本在 `DllMain` 中初始化 Winsock，支持多进程加载。
7. **搜索前置**：UDP 操作（`lora_sdk_get_net_params`、`lora_sdk_send_at`）需要先执行 `lora_sdk_search_devices()` 获取设备 MAC 地址。
8. **网卡枚举**：`lora_sdk_search_devices()` 自动枚举所有活跃非回环 IPv4 网卡并发送广播。
