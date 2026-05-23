/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_sdk_at.c — Shared AT command helpers
 *
 * Common utilities used by both lora_sdk_udp.c and lora_sdk_serial.c.
 */

#include "lora_sdk_internal.h"
#include "lora_sdk_at.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * AT command string helpers
 * ================================================================ */

int sdk_at_ensure_crlf(const char *cmd, char *out, int out_size)
{
    size_t clen = strlen(cmd);
    int need_crlf = 1;

    if (clen >= 2 && cmd[clen - 2] == '\r' && cmd[clen - 1] == '\n')
        need_crlf = 0;

    int total = (int)clen + (need_crlf ? 2 : 0);
    if (total >= out_size)
        return -1;

    memcpy(out, cmd, clen);
    if (need_crlf) {
        out[clen]     = '\r';
        out[clen + 1] = '\n';
    }
    out[total] = '\0';
    return total;
}

int sdk_at_is_query(const char *cmd)
{
    size_t len = strlen(cmd);
    /* Strip trailing \r\n */
    while (len > 0 && (cmd[len - 1] == '\r' || cmd[len - 1] == '\n'))
        len--;
    return (len > 0 && cmd[len - 1] == '?');
}

/* ================================================================
 * AT response helpers
 * ================================================================ */

int sdk_at_trim_response(char *buf, int len)
{
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' ||
           buf[len - 1] == ' '))
        len--;
    buf[len] = '\0';
    return len;
}

void sdk_at_dispatch_response(lora_sdk_t *sdk, const char *response,
                               enum lora_sdk_log_source source)
{
    if (!response || !response[0])
        return;

    /* Fire AT response callback */
    SDK_CALL(sdk, on_at_response, response);

    /* Log the response */
    char log[600];
    snprintf(log, sizeof(log), "RX <- %s", response);
    SDK_CALL(sdk, on_log, log, source);
}

/* ================================================================
 * Worker thread helper
 * ================================================================ */

int sdk_at_launch_worker(LPTHREAD_START_ROUTINE worker,
                           const void *work_data, size_t work_size)
{
    void *work = calloc(1, work_size);
    if (!work)
        return -1;

    memcpy(work, work_data, work_size);
    CloseHandle(CreateThread(NULL, 0, worker, work, 0, NULL));
    return 0;
}

/* ================================================================
 * Gateway reboot (AT+Z)
 * ================================================================ */

typedef struct {
    lora_sdk_t *sdk;
    int         transport;
} sdk_reboot_work_t;

static DWORD WINAPI sdk_reboot_worker(LPVOID param)
{
    sdk_reboot_work_t *work = (sdk_reboot_work_t *)param;
    lora_sdk_t *sdk = work->sdk;
    int transport = work->transport;

    SDK_CALL(sdk, on_log, "Sending AT+Z (gateway reboot)...",
             transport == LORA_SDK_AT_TRANSPORT_SERIAL
                 ? LORA_SDK_LOG_SERIAL : LORA_SDK_LOG_UDP);

    if (transport == LORA_SDK_AT_TRANSPORT_SERIAL)
        sdk_serial_send_at(sdk, "AT+Z");
    else
        sdk_udp_send_at(sdk, "AT+Z");

    Sleep(2000);

    if (transport == LORA_SDK_AT_TRANSPORT_SERIAL) {
        sdk->serial_at_mode = 0;
        SDK_CALL(sdk, on_log, "Gateway rebooted, serial AT mode exited",
                 LORA_SDK_LOG_SERIAL);
    }

    free(work);
    return 0;
}

void sdk_at_reboot(lora_sdk_t *sdk)
{
    if (!sdk) return;

    sdk_reboot_work_t work = { sdk, sdk->at_transport };
    sdk_at_launch_worker(sdk_reboot_worker, &work, sizeof(work));
}
