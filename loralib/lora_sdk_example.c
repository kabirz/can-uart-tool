/*
 * lora_sdk_example.c -- LoRa Gateway SDK 示例程序
 *
 * 演示功能：
 *   1. 初始化 SDK 并注册回调
 *   2. UDP 搜索网关设备
 *   3. TCP 连接网关
 *   4. 收发数据帧（遥测 + 扫描仪）
 *   5. UDP / 串口 AT 指令
 *   6. 查询网络参数
 *   7. 交互式命令行
 *
 * 编译（动态链接 DLL）：
 *   cl lora_sdk_example.c lora_gateway_sdk.lib ws2_32.lib iphlpapi.lib setupapi.lib
 *   运行时需要 lora_gateway_sdk.dll 在同目录或 PATH 中
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lora_sdk.h"

/* ------------------------------------------------------------------ */
/*  Synchronization: wait for async events in main thread             */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#include <windows.h>
static HANDLE g_connEvent     = NULL;
static HANDLE g_searchEvent   = NULL;
static HANDLE g_netParamsEvent = NULL;
static volatile int g_connected = 0;
#endif

static char g_gatewayIp[64] = "";
static lora_sdk_t *g_sdk_for_echo = NULL;

/* ------------------------------------------------------------------ */
/*  Callbacks                                                          */
/* ------------------------------------------------------------------ */

static void on_conn_state(void *ud, enum lora_sdk_conn_state state)
{
    (void)ud;
    const char *names[] = {"DISCONNECTED", "CONNECTING", "CONNECTED"};
    printf("[连接状态] %s\n", names[state]);

#ifdef _WIN32
    if (state == LORA_SDK_CONN_CONNECTED) {
        g_connected = 1;
        SetEvent(g_connEvent);
    } else if (state == LORA_SDK_CONN_DISCONNECTED) {
        g_connected = 0;
    }
#endif
}

static void print_hex(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
        printf(" %02X", data[i]);
}

static void on_frame(void *ud, uint32_t nid,
                     const uint8_t *payload, uint16_t len)
{
    (void)ud;
    if (!payload || len == 0) return;

    uint8_t type = payload[0];
    const uint8_t *body = payload + 1;
    uint16_t body_len = len - 1;

    switch (type) {

    case LORA_DATA_HANDLER: {
        printf("[手柄] NID=0x%08X, len=%u\n", nid, len);

        if (body_len == 8 &&
            body[5] == 0xFF && body[6] == 0xFF && body[7] == 0xFF) {
            int16_t x = (int16_t)((uint16_t)body[0] << 8 | body[1]);
            int16_t y = (int16_t)((uint16_t)body[2] << 8 | body[3]);
            uint8_t btn = body[4] & 0x01;
            printf("  遥测: X=%d, Y=%d, 按键=%s\n",
                   x, y, btn ? "松开" : "按下");
            // TODO 这里需要下发模拟扫描仪合并帧 结构体为 lora_scanner_data_t，打包函数为 lora_scanner_pack，发送函数为 lora_sdk_send_frame
        }
        break;
    }

    case LORA_DATA_TEST: {
        printf("[测试] NID=0x%08X, len=%u\n", nid, len);

        if (body_len >= 6) {
            uint16_t idx = (uint16_t)((uint16_t)body[0] << 8 | body[1]);
            uint32_t ts  = (uint32_t)((uint32_t)body[2] << 24 |
                                      (uint32_t)body[3] << 16 |
                                      (uint32_t)body[4] << 8 | body[5]);
            printf("  idx=%u, ts=%u ms\n", idx, ts);
        } else if (body_len >= 2) {
            uint16_t idx = (uint16_t)((uint16_t)body[0] << 8 | body[1]);
            printf("  idx=%u\n", idx);
        } else {
            printf("  短帧数据:");
            print_hex(payload, len);
            printf("\n");
        }

        if (g_sdk_for_echo) {
            lora_sdk_send_frame(g_sdk_for_echo, nid, payload, len);
            printf("  -> 已回显\n");
        }
        break;
    }

    case LORA_DATA_RSSI: {
        printf("[RSSI] NID=0x%08X, len=%u\n", nid, len);
        lora_sdk_query_rssi(g_sdk_for_echo, nid);
        printf("  -> 已发送 AT+NINFO? 查询信号强度\n");
        break;
    }

    default: {
        printf("[未知 0x%02X] NID=0x%08X, len=%u, 数据:", type, nid, len);
        print_hex(payload, len);
        printf("\n");
        break;
    }
    }
}

static void on_device_found(void *ud, const char *mac,
                            const char *name, const char *sw, const char *ip)
{
    (void)ud;
    printf("[设备发现] MAC=%s, 设备=%s, SW=%s, IP=%s\n", mac, name, sw, ip);

#ifdef _WIN32
    SetEvent(g_searchEvent);
#endif
}

static void on_at_response(void *ud, const char *resp)
{
    (void)ud;
    printf("[AT响应] %s\n", resp);
}

static void on_net_params(void *ud, const char *ip,
                          const char *mask, const char *gw)
{
    (void)ud;
    printf("[网络参数] IP=%s, 掩码=%s, 网关=%s\n", ip, mask, gw);
    if (gw && gw[0]) {
        strncpy(g_gatewayIp, gw, sizeof(g_gatewayIp) - 1);
        printf("[自动] 已保存网关 IP: %s，后续连接将使用此地址\n", g_gatewayIp);
    }
#ifdef _WIN32
    SetEvent(g_netParamsEvent);
#endif
}

static void on_log(void *ud, const char *msg,
                   enum lora_sdk_log_source source)
{
    (void)ud;
    const char *src_names[] = {"TCP", "UDP", "SERIAL"};
    printf("[%s] %s\n", src_names[source], msg);
}

static void on_hex_dump(void *ud, const char *prefix,
                        const uint8_t *data, int len)
{
    (void)ud;
    printf("[HEX] %s (%d bytes):", prefix, len);
    for (int i = 0; i < len && i < 32; i++)
        printf(" %02X", data[i]);
    if (len > 32) printf(" ...");
    printf("\n");
}

static void on_error(void *ud, const char *msg)
{
    (void)ud;
    fprintf(stderr, "[错误] %s\n", msg);
}

/* ------------------------------------------------------------------ */
/*  Interactive command loop                                           */
/* ------------------------------------------------------------------ */

static void print_help(void)
{
    printf(
        "\n=== LoRa SDK 示例程序 ===\n"
        "--- UDP 操作 ---\n"
        "  s              搜索网关设备\n"
        "  c              TCP 连接网关 (自动获取 IP)\n"
        "  d              断开 TCP 连接\n"
        "  n              查询网络参数\n"
        "--- 数据帧 ---\n"
        "  f              发送测试帧\n"
        "  r              发送扫描仪帧\n"
        "--- AT 指令 ---\n"
        "  a <cmd>        发送 AT 指令 (UDP)\n"
        "--- 串口 AT 指令 ---\n"
        "  o <COM> [baud] 打开串口 (如: o COM3 115200)\n"
        "  x              关闭串口\n"
        "  t <mode>       切换 AT 传输方式 (t udp / t serial)\n"
        "--- 其他 ---\n"
        "  h              显示帮助\n"
        "  q              退出\n"
        "\n");
}

static void do_send_test_frame(lora_sdk_t *sdk)
{
    uint8_t data[] = {
        LORA_DATA_TEST,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
    };
    lora_sdk_send_frame(sdk, 0x00000001, data, sizeof(data));
    printf("已发送测试帧\n");
}

static void do_send_scanner(lora_sdk_t *sdk)
{
    lora_scanner_data_t scan;
    memset(&scan, 0, sizeof(scan));
    scan.overbreak_valid = 1;
    scan.laser_valid     = 1;
    scan.coord_z_valid   = 1;
    scan.coord_xy_valid  = 1;
    scan.overbreak = 42;
    scan.laser     = 12345;
    scan.coord_x   = 100;
    scan.coord_y   = -200;
    scan.coord_z   = 300;
    uint8_t buf[LORA_SCANNER_FRAME_SIZE];
    lora_scanner_pack(buf, sizeof(buf), &scan);
    lora_sdk_send_frame(sdk, 0x00000001, buf, sizeof(buf));
    printf("已发送扫描仪帧\n");
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    g_connEvent      = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_searchEvent    = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_netParamsEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
#endif

    lora_sdk_callbacks_t cbs = {0};
    cbs.on_conn_state   = on_conn_state;
    cbs.on_frame        = on_frame;
    cbs.on_device_found = on_device_found;
    cbs.on_at_response  = on_at_response;
    cbs.on_net_params   = on_net_params;
    cbs.on_log          = on_log;
    cbs.on_hex_dump     = on_hex_dump;
    cbs.on_error        = on_error;

    lora_sdk_t *sdk = lora_sdk_init(&cbs, NULL);
    if (!sdk) {
        fprintf(stderr, "SDK 初始化失败\n");
        return 1;
    }
    g_sdk_for_echo = sdk;

    printf("LoRa Gateway SDK 示例程序已启动\n");

    char line[256];
    for (;;) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;

        char cmd = line[0];
        switch (cmd) {
        case 's':
            printf("正在搜索设备...\n");
            lora_sdk_search_devices(sdk);
#ifdef _WIN32
            WaitForSingleObject(g_searchEvent, 6000);
            ResetEvent(g_searchEvent);
#endif
            break;

        case 'c': {
            if (!g_gatewayIp[0]) {
                printf("尚未获取网关 IP，正在搜索设备并查询网络参数...\n");
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
            }
            if (!g_gatewayIp[0]) {
                printf("未能获取网关 IP，请检查设备连接\n");
                break;
            }
            int port = 8899;
            printf("连接 %s:%d ...\n", g_gatewayIp, port);
            lora_sdk_connect(sdk, g_gatewayIp, port);
#ifdef _WIN32
            WaitForSingleObject(g_connEvent, 15000);
            ResetEvent(g_connEvent);
#endif
            break;
        }

        case 'd':
            lora_sdk_disconnect(sdk);
            printf("已断开\n");
            break;

        case 'f':
            do_send_test_frame(sdk);
            break;

        case 'r':
            do_send_scanner(sdk);
            break;

        case 'n':
            lora_sdk_get_net_params(sdk);
            break;

        case 'a': {
            char *atcmd = line + 1;
            while (*atcmd == ' ') atcmd++;
            if (*atcmd) {
                size_t l = strlen(atcmd);
                if (l > 0 && atcmd[l - 1] == '\n') atcmd[l - 1] = '\0';
                printf("发送 AT: %s\n", atcmd);
                lora_sdk_send_at(sdk, atcmd);
            } else {
                printf("用法: a <AT指令>\n");
            }
            break;
        }

        /* Serial port commands */
        case 'o': {
            char arg[256] = "";
            int baud = 115200;
            sscanf(line + 1, "%255s %d", arg, &baud);
            if (arg[0]) {
                printf("打开串口: %s (%d baud)\n", arg, baud);
                int ret = lora_sdk_serial_open(sdk, arg, baud);
                if (ret == 0) {
                    lora_sdk_set_at_transport(sdk, LORA_SDK_AT_TRANSPORT_SERIAL);
                    printf("串口已打开，AT 传输已切换为串口模式\n");
                }
            } else {
                printf("用法: o <COM口> [波特率]\n");
                printf("示例: o COM3 115200\n");
            }
            break;
        }

        case 'x':
            lora_sdk_serial_close(sdk);
            lora_sdk_set_at_transport(sdk, LORA_SDK_AT_TRANSPORT_UDP);
            printf("串口已关闭，AT 传输已切换为 UDP 模式\n");
            break;

        case 't': {
            char arg[32] = "";
            sscanf(line + 1, "%31s", arg);
            if (strcmp(arg, "serial") == 0 || strcmp(arg, "s") == 0) {
                if (lora_sdk_serial_is_open(sdk)) {
                    lora_sdk_set_at_transport(sdk, LORA_SDK_AT_TRANSPORT_SERIAL);
                    printf("AT 传输方式: 串口\n");
                } else {
                    printf("错误: 请先打开串口 (o 命令)\n");
                }
            } else if (strcmp(arg, "udp") == 0 || strcmp(arg, "u") == 0) {
                lora_sdk_set_at_transport(sdk, LORA_SDK_AT_TRANSPORT_UDP);
                printf("AT 传输方式: UDP\n");
            } else {
                printf("用法: t udp / t serial\n");
                printf("当前传输: %s\n",
                       lora_sdk_get_at_transport(sdk) == LORA_SDK_AT_TRANSPORT_SERIAL
                           ? "串口" : "UDP");
            }
            break;
        }

        case 'h':
            print_help();
            break;

        case 'q':
            goto quit;

        case '\n':
        case '\r':
            break;

        default:
            printf("未知命令 '%c'，输入 h 查看帮助\n", cmd);
            break;
        }
    }

quit:
    lora_sdk_disconnect(sdk);
    lora_sdk_cleanup(sdk);

#ifdef _WIN32
    if (g_connEvent)      CloseHandle(g_connEvent);
    if (g_searchEvent)    CloseHandle(g_searchEvent);
    if (g_netParamsEvent) CloseHandle(g_netParamsEvent);
#endif

    printf("已退出\n");
    return 0;
}
