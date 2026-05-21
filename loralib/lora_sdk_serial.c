/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_sdk_serial.c — Serial (UART) AT command transport
 *
 * USR-LG210-L / WH-L101-L gateway: serial port AT mode configuration.
 * Direct AT command send/receive without JSON/USR1566 wrapping.
 */

#include "lora_sdk_internal.h"
#include "lora_sdk_at.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Constants
 * ================================================================ */

#define SERIAL_RX_BUF_SIZE    4096
#define SERIAL_AT_TIMEOUT_MS  3000
#define SERIAL_AT_POLL_MS     50
#define SERIAL_HANDSHAKE_TO_MS 500

/* ================================================================
 * Internal helpers — serial port low-level I/O
 * ================================================================ */

/* Send raw bytes to serial port. Returns 0 on success, -1 on error. */
static int serial_write(lora_sdk_t *sdk, const uint8_t *data, int len)
{
    DWORD written = 0;
    if (!WriteFile(sdk->serial_handle, data, (DWORD)len, &written, NULL))
        return -1;
    if ((int)written != len)
        return -1;

    return 0;
}

/* Send a string to serial port. Returns 0 on success, -1 on error. */
static int serial_write_str(lora_sdk_t *sdk, const char *str)
{
    return serial_write(sdk, (const uint8_t *)str, (int)strlen(str));
}

/* Read from serial port with timeout. Stops early on OK or ERR-.
 * Returns number of bytes read (may be 0 on timeout), -1 on error.
 * Response is null-terminated in 'buf'. */
static int serial_read_response(lora_sdk_t *sdk, char *buf, int buf_size,
                                 DWORD timeout_ms)
{
    if (buf_size < 2) return -1;
    buf[0] = '\0';

    DWORD start = GetTickCount();
    int total = 0;

    while (1) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeout_ms) break;

        DWORD avail = 0;
        uint8_t chunk[256];

        /* Non-blocking read of available data */
        if (!ReadFile(sdk->serial_handle, chunk, sizeof(chunk), &avail, NULL))
            return -1;

        if (avail > 0) {
            /* Append to output buffer */
            int space = buf_size - total - 1;
            if (space <= 0) break;
            if ((int)avail > space) avail = (DWORD)space;
            memcpy(buf + total, chunk, avail);
            total += (int)avail;
            buf[total] = '\0';

            /* Check for end-of-response markers */
            if (strstr(buf, "OK") || strstr(buf, "ERR-"))
                break;
        }

        Sleep(SERIAL_AT_POLL_MS);
    }

    return total;
}

/* Purge serial receive buffer */
static void serial_purge_rx(lora_sdk_t *sdk)
{
    PurgeComm(sdk->serial_handle, PURGE_RXCLEAR);
}

/* ================================================================
 * AT mode management
 * ================================================================ */

static int serial_enter_at_mode(lora_sdk_t *sdk)
{
    if (sdk->serial_at_mode)
        return 0; /* already in AT mode */

    char rbuf[256];
    int n;

    /* Purge before starting */
    PurgeComm(sdk->serial_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    /* Guard time — device needs silence before +++ */
    Sleep(100);

    /* Step 1: send "+++" (no CR/LF), wait for "a" */
    SDK_CALL(sdk, on_log, "Entering AT mode: sending +++", LORA_SDK_LOG_SERIAL);
    if (serial_write_str(sdk, "+++") != 0) {
        SDK_CALL(sdk, on_error, "Serial write failed (+++)", LORA_SDK_LOG_SERIAL);
        return -1;
    }

    n = serial_read_response(sdk, rbuf, sizeof(rbuf), SERIAL_HANDSHAKE_TO_MS);
    if (n <= 0 || !strstr(rbuf, "a")) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "AT mode handshake failed: expected 'a', got '%.*s'", n, rbuf);
        SDK_CALL(sdk, on_error, msg, LORA_SDK_LOG_SERIAL);
        return -1;
    }
    SDK_CALL(sdk, on_log, "AT mode: received 'a'", LORA_SDK_LOG_SERIAL);

    /* Step 2: send "a" (no CR/LF, within 3s), wait for "+OK" */
    if (serial_write_str(sdk, "a") != 0) {
        SDK_CALL(sdk, on_error, "Serial write failed (a)", LORA_SDK_LOG_SERIAL);
        return -1;
    }

    n = serial_read_response(sdk, rbuf, sizeof(rbuf), SERIAL_HANDSHAKE_TO_MS);
    if (n <= 0 || !strstr(rbuf, "+OK")) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "AT mode handshake failed: expected '+OK', got '%.*s'", n, rbuf);
        SDK_CALL(sdk, on_error, msg, LORA_SDK_LOG_SERIAL);
        return -1;
    }

    sdk->serial_at_mode = 1;
    SDK_CALL(sdk, on_log, "AT mode entered successfully", LORA_SDK_LOG_SERIAL);
    return 0;
}

static void serial_exit_at_mode(lora_sdk_t *sdk)
{
    if (!sdk->serial_at_mode)
        return;

    SDK_CALL(sdk, on_log, "Exiting AT mode", LORA_SDK_LOG_SERIAL);

    /* Send AT+ENTM to leave AT mode */
    serial_write_str(sdk, "AT+ENTM\r\n");

    /* Brief wait for response, then purge */
    Sleep(200);
    PurgeComm(sdk->serial_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    sdk->serial_at_mode = 0;
}

/* ================================================================
 * Serial AT command worker — runs in background thread
 * ================================================================ */

typedef struct {
    lora_sdk_t *sdk;
    char        cmd[512];
} serial_at_work_t;

static DWORD WINAPI serial_at_worker(LPVOID param)
{
    serial_at_work_t *work = (serial_at_work_t *)param;
    lora_sdk_t *sdk = work->sdk;

    /* Ensure AT mode */
    if (serial_enter_at_mode(sdk) != 0) {
        free(work);
        return 1;
    }

    /* Build full command with \r\n termination */
    char full_cmd[520];
    if (sdk_at_ensure_crlf(work->cmd, full_cmd, sizeof(full_cmd)) < 0) {
        SDK_CALL(sdk, on_error, "AT command too long", LORA_SDK_LOG_SERIAL);
        free(work);
        return 1;
    }

    /* Purge RX before sending */
    serial_purge_rx(sdk);

    /* Send */
    {
        char log[600];
        snprintf(log, sizeof(log), "TX -> %s", work->cmd);
        SDK_CALL(sdk, on_log, log, LORA_SDK_LOG_SERIAL);
    }

    if (serial_write_str(sdk, full_cmd) != 0) {
        SDK_CALL(sdk, on_error, "Serial AT command write failed", LORA_SDK_LOG_SERIAL);
        free(work);
        return 1;
    }

    /* Read response */
    char rbuf[SERIAL_RX_BUF_SIZE];
    int n = serial_read_response(sdk, rbuf, sizeof(rbuf), SERIAL_AT_TIMEOUT_MS);

    if (n > 0) {
        sdk_at_trim_response(rbuf, n);
        sdk_at_dispatch_response(sdk, rbuf, LORA_SDK_LOG_SERIAL);
    } else {
        SDK_CALL(sdk, on_log, "AT command timeout (no response)", LORA_SDK_LOG_SERIAL);
    }

    free(work);
    return 0;
}

/* ================================================================
 * Response parsing helpers
 * ================================================================ */

/* Strip trailing "\r\nOK" or "OK" from AT command response.
 * AT command responses have format: "+CMD:value\r\nOK\r\n"
 * After sdk_at_trim_response() they become: "+CMD:value\r\nOK"
 * This strips the remaining "OK" (and preceding \r\n). */
static void serial_strip_trailing_ok(char *buf)
{
    int len = (int)strlen(buf);

    /* Remove trailing "OK" */
    if (len >= 2 && strcmp(buf + len - 2, "OK") == 0) {
        len -= 2;
        buf[len] = '\0';
    }

    /* Remove any remaining trailing \r\n */
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
        buf[--len] = '\0';
    }
}

/* Extract value from "+PREFIX:value" format response.
 * Returns value length, or -1 if prefix not found. */
static int serial_extract_value(const char *response, const char *prefix,
                                 char *out, int out_size)
{
    const char *p = strstr(response, prefix);
    if (!p) return -1;

    p += strlen(prefix);
    if (*p == ':') p++; /* skip colon if present in prefix */

    int i = 0;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && i < out_size - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i;
}

/* ================================================================
 * Serial device info / network params — worker threads
 * ================================================================ */

/* Send one AT command synchronously (called from worker thread).
 * Returns response in buf (null-terminated, with trailing OK stripped),
 * or NULL on failure. */
static const char *serial_at_sync(lora_sdk_t *sdk, const char *cmd,
                                   char *buf, int buf_size)
{
    char full_cmd[520];
    if (sdk_at_ensure_crlf(cmd, full_cmd, sizeof(full_cmd)) < 0)
        return NULL;

    serial_purge_rx(sdk);

    {
        char log[600];
        snprintf(log, sizeof(log), "TX -> %s", cmd);
        SDK_CALL(sdk, on_log, log, LORA_SDK_LOG_SERIAL);
    }

    if (serial_write_str(sdk, full_cmd) != 0)
        return NULL;

    int n = serial_read_response(sdk, buf, buf_size, SERIAL_AT_TIMEOUT_MS);
    if (n <= 0) {
        SDK_CALL(sdk, on_log, "AT command timeout", LORA_SDK_LOG_SERIAL);
        return NULL;
    }

    sdk_at_trim_response(buf, n);
    serial_strip_trailing_ok(buf);

    {
        char log[600];
        snprintf(log, sizeof(log), "RX <- %s", buf);
        SDK_CALL(sdk, on_log, log, LORA_SDK_LOG_SERIAL);
    }

    return buf;
}

typedef struct {
    lora_sdk_t *sdk;
} serial_info_work_t;

/* Worker: query device info via AT commands */
static DWORD WINAPI serial_device_info_worker(LPVOID param)
{
    serial_info_work_t *work = (serial_info_work_t *)param;
    lora_sdk_t *sdk = work->sdk;

    if (serial_enter_at_mode(sdk) != 0) {
        SDK_CALL(sdk, on_error, "AT mode entry failed", LORA_SDK_LOG_SERIAL);
        free(work);
        return 1;
    }

    char rbuf[SERIAL_RX_BUF_SIZE];
    const char *resp;
    char value[128];

    /* AT+INMDL? — device model → dev_name */
    resp = serial_at_sync(sdk, "AT+INMDL?", rbuf, sizeof(rbuf));
    if (resp && serial_extract_value(resp, "+INMDL:", value, sizeof(value)) > 0) {
        snprintf(sdk->dev_name, sizeof(sdk->dev_name), "%s", value);
    }

    /* AT+VER? — firmware version → dev_sw */
    resp = serial_at_sync(sdk, "AT+VER?", rbuf, sizeof(rbuf));
    if (resp && serial_extract_value(resp, "+VER:", value, sizeof(value)) > 0) {
        snprintf(sdk->dev_sw, sizeof(sdk->dev_sw), "%s", value);
    }

    /* AT+MAC? — MAC address → dev_mac */
    resp = serial_at_sync(sdk, "AT+MAC?", rbuf, sizeof(rbuf));
    if (resp && serial_extract_value(resp, "+MAC:", value, sizeof(value)) > 0) {
        snprintf(sdk->dev_mac, sizeof(sdk->dev_mac), "%s", value);
    }

    /* Also store local serial "address" for reference */
    snprintf(sdk->dev_addr, sizeof(sdk->dev_addr), "SERIAL");

    SDK_CALL(sdk, on_log, "Device info queried via serial", LORA_SDK_LOG_SERIAL);
    SDK_CALL(sdk, on_device_found,
             sdk->dev_mac, sdk->dev_name, sdk->dev_sw, "SERIAL");

    free(work);
    return 0;
}

/* Worker: query network params via AT commands */
static DWORD WINAPI serial_net_params_worker(LPVOID param)
{
    serial_info_work_t *work = (serial_info_work_t *)param;
    lora_sdk_t *sdk = work->sdk;

    if (serial_enter_at_mode(sdk) != 0) {
        SDK_CALL(sdk, on_error, "AT mode entry failed", LORA_SDK_LOG_SERIAL);
        free(work);
        return 1;
    }

    char rbuf[SERIAL_RX_BUF_SIZE];
    const char *resp = serial_at_sync(sdk, "AT+WANN?", rbuf, sizeof(rbuf));

    if (resp) {
        /* Response format: +WANN:<mode,address,mask,gateway>
         * e.g.: +WANN:STATIC,192.168.1.100,255.255.255.0,192.168.1.1
         *   or: +WANN:DHCP,192.168.1.100,255.255.255.0,192.168.1.1 */
        char wan_val[128];
        if (serial_extract_value(resp, "+WANN:", wan_val, sizeof(wan_val)) > 0) {
            char mode[16] = "", ip[64] = "", mask[64] = "", gw[64] = "";
            sscanf(wan_val, "%15[^,],%63[^,],%63[^,],%63s", mode, ip, mask, gw);

            snprintf(sdk->dev_ip, sizeof(sdk->dev_ip), "%s", ip);
            snprintf(sdk->dev_sm, sizeof(sdk->dev_sm), "%s", mask);
            snprintf(sdk->dev_gw, sizeof(sdk->dev_gw), "%s", gw);

            {
                char log[256];
                snprintf(log, sizeof(log), "WAN: mode=%s IP=%s mask=%s GW=%s",
                         mode, ip, mask, gw);
                SDK_CALL(sdk, on_log, log, LORA_SDK_LOG_SERIAL);
            }
        }
    }

    SDK_CALL(sdk, on_net_params, sdk->dev_ip, sdk->dev_sm, sdk->dev_gw);

    free(work);
    return 0;
}

/* Launch a device info query over serial */
void sdk_serial_query_device_info(lora_sdk_t *sdk)
{
    if (!sdk || !InterlockedCompareExchange(&sdk->serial_open, 0, 0)) {
        if (sdk) SDK_CALL(sdk, on_error, "Serial port not open", LORA_SDK_LOG_SERIAL);
        return;
    }

    serial_info_work_t work_init = { sdk };
    sdk_at_launch_worker(serial_device_info_worker, &work_init,
                          sizeof(work_init));
}

/* Launch a network params query over serial */
void sdk_serial_query_net_params(lora_sdk_t *sdk)
{
    if (!sdk || !InterlockedCompareExchange(&sdk->serial_open, 0, 0)) {
        if (sdk) SDK_CALL(sdk, on_error, "Serial port not open", LORA_SDK_LOG_SERIAL);
        return;
    }

    serial_info_work_t work_init = { sdk };
    sdk_at_launch_worker(serial_net_params_worker, &work_init,
                          sizeof(work_init));
}

/* ================================================================
 * Public serial functions
 * ================================================================ */

int sdk_serial_open(lora_sdk_t *sdk, const char *com_port, int baud_rate)
{
    if (!sdk || !com_port || !com_port[0])
        return -1;

    /* Already open? */
    if (InterlockedCompareExchange(&sdk->serial_open, 0, 0))
        return 0;

    if (baud_rate <= 0)
        baud_rate = 115200;

    /* Build COM port path: \\.\COMx */
    char port_path[32];
    if (strncmp(com_port, "\\\\.\\", 4) == 0)
        snprintf(port_path, sizeof(port_path), "%s", com_port);
    else
        snprintf(port_path, sizeof(port_path), "\\\\.\\%s", com_port);

    /* Open serial port */
    HANDLE h = CreateFileA(
        port_path,
        GENERIC_READ | GENERIC_WRITE,
        0,          /* exclusive access */
        NULL,       /* no security attributes */
        OPEN_EXISTING,
        0,          /* non-overlapped */
        NULL);

    if (h == INVALID_HANDLE_VALUE) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Failed to open %s (error %lu)",
                 port_path, GetLastError());
        SDK_CALL(sdk, on_error, msg, LORA_SDK_LOG_SERIAL);
        return -1;
    }

    /* Configure buffer sizes */
    SetupComm(h, 4096, 4096);

    /* Configure DCB */
    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        SDK_CALL(sdk, on_error, "GetCommState failed", LORA_SDK_LOG_SERIAL);
        return -1;
    }

    dcb.BaudRate = (DWORD)baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;

    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        SDK_CALL(sdk, on_error, "SetCommState failed", LORA_SDK_LOG_SERIAL);
        return -1;
    }

    /* Configure timeouts — non-blocking reads */
    COMMTIMEOUTS cto = { 0 };
    cto.ReadIntervalTimeout         = MAXDWORD;
    cto.ReadTotalTimeoutMultiplier  = 0;
    cto.ReadTotalTimeoutConstant    = 0;
    cto.WriteTotalTimeoutMultiplier = 0;
    cto.WriteTotalTimeoutConstant   = 2000;
    SetCommTimeouts(h, &cto);

    sdk->serial_handle  = h;
    sdk->serial_at_mode = 0;
    InterlockedExchange(&sdk->serial_open, 1);

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "Serial port %s opened (%d baud)",
                 port_path, baud_rate);
        SDK_CALL(sdk, on_log, msg, LORA_SDK_LOG_SERIAL);
    }

    return 0;
}

void sdk_serial_close(lora_sdk_t *sdk)
{
    if (!sdk) return;
    if (!InterlockedCompareExchange(&sdk->serial_open, 0, 0))
        return;

    /* Exit AT mode if active */
    if (sdk->serial_at_mode)
        serial_exit_at_mode(sdk);

    if (sdk->serial_handle != INVALID_HANDLE_VALUE &&
        sdk->serial_handle != NULL) {
        CloseHandle(sdk->serial_handle);
        sdk->serial_handle = INVALID_HANDLE_VALUE;
    }

    InterlockedExchange(&sdk->serial_open, 0);

    SDK_CALL(sdk, on_log, "Serial port closed", LORA_SDK_LOG_SERIAL);
}

void sdk_serial_send_at(lora_sdk_t *sdk, const char *cmd)
{
    if (!sdk || !cmd || !cmd[0]) {
        if (sdk) SDK_CALL(sdk, on_error, "Empty AT command", LORA_SDK_LOG_SERIAL);
        return;
    }

    if (!InterlockedCompareExchange(&sdk->serial_open, 0, 0)) {
        SDK_CALL(sdk, on_error, "Serial port not open", LORA_SDK_LOG_SERIAL);
        return;
    }

    serial_at_work_t work_init = { sdk };
    strncpy(work_init.cmd, cmd, sizeof(work_init.cmd) - 1);
    work_init.cmd[sizeof(work_init.cmd) - 1] = '\0';

    sdk_at_launch_worker(serial_at_worker, &work_init, sizeof(work_init));
}
