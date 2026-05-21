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

    SDK_CALL(sdk, on_hex_dump, "TX (serial)", data, len);
    return 0;
}

/* Send a string to serial port. Returns 0 on success, -1 on error. */
static int serial_write_str(lora_sdk_t *sdk, const char *str)
{
    return serial_write(sdk, (const uint8_t *)str, (int)strlen(str));
}

/* Read from serial port with timeout. Stops early on +OK or +ERROR.
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
            SDK_CALL(sdk, on_hex_dump, "RX (serial)", chunk, (int)avail);

            /* Append to output buffer */
            int space = buf_size - total - 1;
            if (space <= 0) break;
            if ((int)avail > space) avail = (DWORD)space;
            memcpy(buf + total, chunk, avail);
            total += (int)avail;
            buf[total] = '\0';

            /* Check for end-of-response markers */
            if (strstr(buf, "+OK") || strstr(buf, "+ERROR"))
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
        SDK_CALL(sdk, on_error, "Serial write failed (+++)");
        return -1;
    }

    n = serial_read_response(sdk, rbuf, sizeof(rbuf), SERIAL_HANDSHAKE_TO_MS);
    if (n <= 0 || !strstr(rbuf, "a")) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "AT mode handshake failed: expected 'a', got '%.*s'", n, rbuf);
        SDK_CALL(sdk, on_error, msg);
        return -1;
    }
    SDK_CALL(sdk, on_log, "AT mode: received 'a'", LORA_SDK_LOG_SERIAL);

    /* Step 2: send "a" (no CR/LF), wait for "+OK" */
    if (serial_write_str(sdk, "a") != 0) {
        SDK_CALL(sdk, on_error, "Serial write failed (a)");
        return -1;
    }

    n = serial_read_response(sdk, rbuf, sizeof(rbuf), SERIAL_HANDSHAKE_TO_MS);
    if (n <= 0 || !strstr(rbuf, "+OK")) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "AT mode handshake failed: expected '+OK', got '%.*s'", n, rbuf);
        SDK_CALL(sdk, on_error, msg);
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

    /* Send AT+EXIT to leave AT mode */
    serial_write_str(sdk, "AT+EXIT\r\n");

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
        SDK_CALL(sdk, on_error, "AT command too long");
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
        SDK_CALL(sdk, on_error, "Serial AT command write failed");
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
        SDK_CALL(sdk, on_error, msg);
        return -1;
    }

    /* Configure buffer sizes */
    SetupComm(h, 4096, 4096);

    /* Configure DCB */
    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        SDK_CALL(sdk, on_error, "GetCommState failed");
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
        SDK_CALL(sdk, on_error, "SetCommState failed");
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
        if (sdk) SDK_CALL(sdk, on_error, "Empty AT command");
        return;
    }

    if (!InterlockedCompareExchange(&sdk->serial_open, 0, 0)) {
        SDK_CALL(sdk, on_error, "Serial port not open");
        return;
    }

    serial_at_work_t work_init = { sdk };
    strncpy(work_init.cmd, cmd, sizeof(work_init.cmd) - 1);
    work_init.cmd[sizeof(work_init.cmd) - 1] = '\0';

    sdk_at_launch_worker(serial_at_worker, &work_init, sizeof(work_init));
}
