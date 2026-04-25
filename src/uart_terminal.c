#include "uart_terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#include <devpkey.h>

#pragma comment(lib, "setupapi.lib")

struct UartTerminal {
    HANDLE hSerial;
    char portName[32];
    DWORD baudRate;
    HANDLE hRecvThread;
    volatile int recvRunning;
    UartRecvCallback recvCallback;
    void* recvCallbackCtx;
    int initialized;
};

/* ---------- recv thread ---------- */

static DWORD WINAPI RecvThread(LPVOID param) {
    UartTerminal* ut = (UartTerminal*)param;
    char buffer[512];

    while (ut->recvRunning) {
        DWORD bytesRead = 0;
        if (ReadFile(ut->hSerial, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
            if (bytesRead > 0 && ut->recvCallback) {
                ut->recvCallback(buffer, (int)bytesRead, ut->recvCallbackCtx);
            }
        }
        Sleep(1);
    }
    return 0;
}

/* ---------- create / destroy ---------- */

UartTerminal* UartTerminal_Create(void) {
    UartTerminal* ut = (UartTerminal*)calloc(1, sizeof(UartTerminal));
    if (!ut) return NULL;

    ut->hSerial = INVALID_HANDLE_VALUE;
    ut->hRecvThread = NULL;
    ut->recvRunning = 0;
    ut->recvCallback = NULL;
    ut->recvCallbackCtx = NULL;
    ut->initialized = 1;

    return ut;
}

void UartTerminal_Destroy(UartTerminal* ut) {
    if (!ut) return;
    UartTerminal_Disconnect(ut);
    free(ut);
}

/* ---------- connect / disconnect ---------- */

int UartTerminal_Connect(UartTerminal* ut, const char* port, DWORD baudrate) {
    if (!ut || !ut->initialized) return 0;
    if (ut->hSerial != INVALID_HANDLE_VALUE) return 1; /* already connected */

    /* Build device name: "\\.\COMx" for ports >= 10 */
    char deviceName[64];
    int portNum = 0;
    if (strncmp(port, "COM", 3) == 0) {
        portNum = atoi(port + 3);
    }
    if (portNum >= 10) {
        sprintf(deviceName, "\\\\.\\%s", port);
    } else {
        strncpy(deviceName, port, sizeof(deviceName) - 1);
        deviceName[sizeof(deviceName) - 1] = '\0';
    }

    /* Open serial port */
    ut->hSerial = CreateFileA(deviceName,
                               GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
    if (ut->hSerial == INVALID_HANDLE_VALUE) {
        return 0;
    }

    /* Configure DCB: 8N1, no flow control, no parity */
    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(ut->hSerial, &dcb)) {
        CloseHandle(ut->hSerial);
        ut->hSerial = INVALID_HANDLE_VALUE;
        return 0;
    }

    dcb.BaudRate = baudrate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(ut->hSerial, &dcb)) {
        CloseHandle(ut->hSerial);
        ut->hSerial = INVALID_HANDLE_VALUE;
        return 0;
    }

    /* Set COMMTIMEOUTS: non-blocking read */
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 5000;

    if (!SetCommTimeouts(ut->hSerial, &timeouts)) {
        CloseHandle(ut->hSerial);
        ut->hSerial = INVALID_HANDLE_VALUE;
        return 0;
    }

    /* Clear buffers */
    PurgeComm(ut->hSerial,
              PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);

    strncpy(ut->portName, port, sizeof(ut->portName) - 1);
    ut->portName[sizeof(ut->portName) - 1] = '\0';
    ut->baudRate = baudrate;

    return 1;
}

void UartTerminal_Disconnect(UartTerminal* ut) {
    if (!ut || !ut->initialized) return;

    UartTerminal_StopRecv(ut);

    if (ut->hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(ut->hSerial);
        ut->hSerial = INVALID_HANDLE_VALUE;
    }
}

int UartTerminal_IsConnected(UartTerminal* ut) {
    if (!ut) return 0;
    return (ut->hSerial != INVALID_HANDLE_VALUE) ? 1 : 0;
}

/* ---------- send ---------- */

int UartTerminal_Send(UartTerminal* ut, const char* data, int len) {
    if (!ut || ut->hSerial == INVALID_HANDLE_VALUE) return 0;

    DWORD bytesWritten = 0;
    if (!WriteFile(ut->hSerial, data, len, &bytesWritten, NULL)) {
        return 0;
    }
    return (int)bytesWritten;
}

/* ---------- recv callback ---------- */

void UartTerminal_SetRecvCallback(UartTerminal* ut, UartRecvCallback cb, void* ctx) {
    if (!ut) return;
    ut->recvCallback = cb;
    ut->recvCallbackCtx = ctx;
}

/* ---------- recv thread control ---------- */

void UartTerminal_StartRecv(UartTerminal* ut) {
    if (!ut || ut->hSerial == INVALID_HANDLE_VALUE) return;
    if (ut->recvRunning) return;  /* already running */

    ut->recvRunning = 1;
    ut->hRecvThread = CreateThread(NULL, 0, RecvThread, ut, 0, NULL);
}

void UartTerminal_StopRecv(UartTerminal* ut) {
    if (!ut) return;
    if (!ut->recvRunning) return;

    ut->recvRunning = 0;

    if (ut->hRecvThread != NULL) {
        WaitForSingleObject(ut->hRecvThread, 2000);
        CloseHandle(ut->hRecvThread);
        ut->hRecvThread = NULL;
    }
}

/* ---------- enumerate ports ---------- */

int UartTerminal_EnumPorts(char ports[][32], int max_count) {
    int count = 0;
    HDEVINFO deviceInfoSet;
    SP_DEVINFO_DATA deviceInfoData;
    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, 0, 0, DIGCF_PRESENT);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return 0;
    }

    for (DWORD i = 0; i < 128 && count < max_count; i++) {
        if (!SetupDiEnumDeviceInfo(deviceInfoSet, i, &deviceInfoData)) {
            break;
        }

        /* Get friendly name (Unicode) */
        WCHAR descW[256] = {0};
        DWORD dataSize = 0;
        DWORD dataType = 0;

        if (!SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &deviceInfoData,
                                                SPDRP_FRIENDLYNAME, &dataType,
                                                (PBYTE)descW, sizeof(descW), &dataSize)) {
            if (!SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &deviceInfoData,
                                                    SPDRP_DEVICEDESC, &dataType,
                                                    (PBYTE)descW, sizeof(descW), &dataSize)) {
                continue;
            }
        }

        /* Filter out Bluetooth ports */
        WCHAR descLower[256] = {0};
        for (int j = 0; descW[j] && j < 255; j++) {
            descLower[j] = (WCHAR)tolower(descW[j]);
        }
        if (wcsstr(descLower, L"bluetooth") || wcsstr(descLower, L"蓝牙")) {
            continue;
        }

        /* Extract COM port number from "(COMx)" pattern */
        WCHAR* comStart = wcsstr(descW, L"(COM");
        if (comStart) {
            comStart += 4;  /* skip "(COM" */
            int portNum = _wtoi(comStart);
            if (portNum > 0 && portNum <= 256) {
                sprintf(ports[count], "COM%d", portNum);
                count++;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return count;
}
