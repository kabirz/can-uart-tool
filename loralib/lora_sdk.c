/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_sdk.c — Lifecycle + public API
 *
 * Static build: WSA init handled in sdk_init/cleanup.
 * DLL build:    WSA init handled in DllMain.
 */

#include "lora_sdk_internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef LORA_SDK_STATIC
/* Reference-counted WSA init for static library */
static LONG g_wsa_init_count = 0;
#endif

/* ================================================================
 * Public API
 * ================================================================ */

LORA_SDK_API lora_sdk_t *lora_sdk_init(const lora_sdk_callbacks_t *callbacks,
                                        void *user_data)
{
    if (!callbacks) return NULL;

#ifdef LORA_SDK_STATIC
    if (InterlockedIncrement(&g_wsa_init_count) == 1) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
#endif

    lora_sdk_t *sdk = (lora_sdk_t *)calloc(1, sizeof(lora_sdk_t));
    if (!sdk) return NULL;

    sdk->cbs = *callbacks;
    sdk->user_data = user_data;
    sdk->tcp_sock = INVALID_SOCKET;
    sdk->serial_handle = INVALID_HANDLE_VALUE;
    sdk->at_transport = LORA_SDK_AT_TRANSPORT_UDP;

    return sdk;
}

LORA_SDK_API void lora_sdk_cleanup(lora_sdk_t *sdk)
{
    if (!sdk) return;

    /* Disconnect and wait for threads */
    sdk_tcp_disconnect(sdk);

    /* Close serial port if open */
    sdk_serial_close(sdk);

    if (sdk->tcp_connect_thread) {
        WaitForSingleObject(sdk->tcp_connect_thread, 3000);
        CloseHandle(sdk->tcp_connect_thread);
    }

    free(sdk);

#ifdef LORA_SDK_STATIC
    if (InterlockedDecrement(&g_wsa_init_count) == 0) {
        WSACleanup();
    }
#endif
}

LORA_SDK_API void lora_sdk_connect(lora_sdk_t *sdk, const char *ip, int port)
{
    if (!sdk) return;
    sdk_tcp_connect(sdk, ip, port);
}

LORA_SDK_API void lora_sdk_disconnect(lora_sdk_t *sdk)
{
    if (!sdk) return;
    sdk_tcp_disconnect(sdk);
}

LORA_SDK_API enum lora_sdk_conn_state lora_sdk_conn_state(lora_sdk_t *sdk)
{
    if (!sdk) return LORA_SDK_CONN_DISCONNECTED;
    return InterlockedCompareExchange(&sdk->connected, 0, 0)
        ? LORA_SDK_CONN_CONNECTED : LORA_SDK_CONN_DISCONNECTED;
}

LORA_SDK_API void lora_sdk_send_frame(lora_sdk_t *sdk, uint32_t nid,
                                       const uint8_t *data, uint16_t data_len)
{
    if (!sdk) return;
    sdk_tcp_send_frame(sdk, nid, data, data_len);
}

LORA_SDK_API void lora_sdk_send_rssi_response(lora_sdk_t *sdk, uint32_t nid,
                                                uint8_t snr_raw,
                                                uint8_t rssi_raw,
                                                uint8_t test_flag)
{
    if (!sdk) return;
    sdk_tcp_send_rssi(sdk, nid, snr_raw, rssi_raw, test_flag);
}

LORA_SDK_API void lora_sdk_search_devices(lora_sdk_t *sdk)
{
    if (!sdk) return;
    sdk_udp_search(sdk);
}

LORA_SDK_API void lora_sdk_get_net_params(lora_sdk_t *sdk)
{
    if (!sdk) return;
    sdk_udp_get_net(sdk);
}

LORA_SDK_API void lora_sdk_send_at(lora_sdk_t *sdk, const char *at_cmd)
{
    if (!sdk) return;
    if (sdk->at_transport == LORA_SDK_AT_TRANSPORT_SERIAL)
        sdk_serial_send_at(sdk, at_cmd);
    else
        sdk_udp_send_at(sdk, at_cmd);
}

LORA_SDK_API void lora_sdk_query_rssi(lora_sdk_t *sdk, uint32_t nid)
{
    if (!sdk) return;
    sdk->pending_rssi_nid = nid;
    sdk_udp_send_at(sdk, "AT+NINFO?\r\n");
}

LORA_SDK_API void lora_sdk_set_test_flag(lora_sdk_t *sdk, int flag)
{
    if (!sdk) return;
    sdk->test_flag = flag ? 1 : 0;
}

LORA_SDK_API int lora_sdk_build_frame(uint8_t *out, size_t out_size,
                                       uint32_t nid,
                                       const uint8_t *data,
                                       uint16_t data_len)
{
    return sdk_build_frame(out, out_size, nid, data, data_len);
}

/* ================================================================
 * Serial operations
 * ================================================================ */

LORA_SDK_API int lora_sdk_serial_open(lora_sdk_t *sdk,
                                       const char *com_port, int baud_rate)
{
    if (!sdk) return -1;
    return sdk_serial_open(sdk, com_port, baud_rate);
}

LORA_SDK_API void lora_sdk_serial_close(lora_sdk_t *sdk)
{
    if (!sdk) return;
    sdk_serial_close(sdk);
}

LORA_SDK_API int lora_sdk_serial_is_open(lora_sdk_t *sdk)
{
    if (!sdk) return 0;
    return InterlockedCompareExchange(&sdk->serial_open, 0, 0) ? 1 : 0;
}

LORA_SDK_API void lora_sdk_set_at_transport(lora_sdk_t *sdk, int transport)
{
    if (!sdk) return;
    sdk->at_transport = transport;
}

LORA_SDK_API int lora_sdk_get_at_transport(lora_sdk_t *sdk)
{
    if (!sdk) return LORA_SDK_AT_TRANSPORT_UDP;
    return sdk->at_transport;
}

/* ================================================================
 * DLL entry point (shared library only)
 * ================================================================ */
#ifndef LORA_SDK_STATIC
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)hModule;
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        break;
    }
    case DLL_PROCESS_DETACH:
        WSACleanup();
        break;
    }
    return TRUE;
}
#endif /* !LORA_SDK_STATIC */
