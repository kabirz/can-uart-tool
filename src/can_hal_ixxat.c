#include "can_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>

#pragma pack(push, 1)

typedef struct {
    DWORD dwTime;
    DWORD dlc;
    BYTE  data[64];
    BYTE  data_len;
} VCI4_CAN_MSG;

#pragma pack(pop)

typedef HRESULT (WINAPI *fnVciInitialize)(void);typedef int    (WINAPI *fnVciEnumDeviceOpen)(HANDLE*);
typedef int    (WINAPI *fnVciEnumDeviceNext)(HANDLE, void*);
typedef int    (WINAPI *fnVciEnumDeviceClose)(HANDLE);
typedef int    (WINAPI *fnVciDeviceOpen)(DWORD, HANDLE*);
typedef int    (WINAPI *fnVciDeviceClose)(HANDLE);
typedef int    (WINAPI *fnVciDeviceGetInfo)(HANDLE, void*);
typedef int    (WINAPI *fnCanControlOpen)(HANDLE, DWORD, HANDLE*);
typedef int    (WINAPI *fnCanControlClose)(HANDLE);
typedef int    (WINAPI *fnCanControlInitialize)(HANDLE, BYTE, BYTE, BYTE, BYTE);
typedef int    (WINAPI *fnCanControlStart)(HANDLE, BYTE);
typedef int    (WINAPI *fnCanControlReset)(HANDLE);
typedef int    (WINAPI *fnCanControlAddFilterIds)(HANDLE, DWORD, DWORD, BYTE);
typedef int    (WINAPI *fnCanChannelOpen)(HANDLE, DWORD, BYTE, HANDLE*);
typedef int    (WINAPI *fnCanChannelClose)(HANDLE);
typedef int    (WINAPI *fnCanChannelInitialize)(HANDLE, WORD, WORD);
typedef int    (WINAPI *fnCanChannelActivate)(HANDLE, BYTE);
typedef int    (WINAPI *fnCanChannelSendMessage)(HANDLE, int, void*);
typedef int    (WINAPI *fnCanChannelReadMessage)(HANDLE, int, void*);
typedef int    (WINAPI *fnCanChannelGetStatus)(HANDLE, void*);

typedef struct {
    HMODULE hDll;
    HANDLE  hDevice;
    HANDLE  hControl;
    HANDLE  hChannel;

    fnVciInitialize        pfVciInitialize;
    fnVciEnumDeviceOpen    pfVciEnumDeviceOpen;
    fnVciEnumDeviceNext    pfVciEnumDeviceNext;
    fnVciEnumDeviceClose   pfVciEnumDeviceClose;
    fnVciDeviceOpen        pfVciDeviceOpen;
    fnVciDeviceClose       pfVciDeviceClose;
    fnVciDeviceGetInfo     pfVciDeviceGetInfo;
    fnCanControlOpen       pfCanControlOpen;
    fnCanControlClose      pfCanControlClose;
    fnCanControlInitialize pfCanControlInitialize;
    fnCanControlStart      pfCanControlStart;
    fnCanControlReset      pfCanControlReset;
    fnCanControlAddFilterIds pfCanControlAddFilterIds;
    fnCanChannelOpen       pfCanChannelOpen;
    fnCanChannelClose      pfCanChannelClose;
    fnCanChannelInitialize pfCanChannelInitialize;
    fnCanChannelActivate   pfCanChannelActivate;
    fnCanChannelSendMessage pfCanChannelSendMessage;
    fnCanChannelReadMessage pfCanChannelReadMessage;
} IxxatPriv;

static void ixxat_log(CanHal *hal, const char *msg)
{
    if (hal && hal->log_cb)
        hal->log_cb(msg);
}

#pragma pack(push, 1)

typedef struct {
    BYTE  bMode;
    BYTE  bBtr0;
    BYTE  bBtr1;
    BYTE  bOpMode;
    DWORD dwId;
    DWORD dwIdMask;
} VCI4_INIT_LINE;

typedef struct {
    DWORD fdf;
    BYTE  id;
    BYTE  dlc;
    BYTE  data[64];
} VCI4_CAN_MSG2;

#pragma pack(pop)

static const struct {
    BYTE btr0;
    BYTE btr1;
} ixxat_baud_table[CAN_HAL_BAUD_COUNT] = {
    {0x31, 0x1C},
    {0x18, 0x1C},
    {0x09, 0x1C},
    {0x04, 0x1C},
    {0x03, 0x1C},
    {0x01, 0x1C},
    {0x00, 0x1C},
    {0x00, 0x14},
};

static int ixxat_detect(CanHal *hal, int *channels, int max_count)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;

    if (!priv->hDll) {
        ixxat_log(hal, "IXXAT: VCI DLL not loaded");
        return 0;
    }

    if (!priv->pfVciEnumDeviceOpen || !priv->pfVciEnumDeviceNext) {
        ixxat_log(hal, "IXXAT: enum functions not available");
        return 0;
    }

    if (priv->pfVciInitialize) {
        HRESULT hr = priv->pfVciInitialize();
        if (hr != 0) {
            ixxat_log(hal, "IXXAT: vciInitialize failed - no IXXAT hardware found");
            return 0;
        }
    }

    int count = 0;
    HANDLE hEnum = NULL;
    int rc = priv->pfVciEnumDeviceOpen(&hEnum);

    if (rc != 0) {
        ixxat_log(hal, "IXXAT: no IXXAT devices found");
        return 0;
    }

    while (count < max_count) {
        BYTE devInfo[256];
        rc = priv->pfVciEnumDeviceNext(hEnum, devInfo);
        if (rc != 0)
            break;
        channels[count++] = count;
    }

    priv->pfVciEnumDeviceClose(hEnum);

    char log[64];
    sprintf(log, "IXXAT: detected %d device(s)", count);
    ixxat_log(hal, log);
    return count;
}

static int ixxat_connect(CanHal *hal, int channel, int baud_index)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;

    if (!priv->hDll) {
        ixxat_log(hal, "IXXAT: VCI DLL not loaded");
        return 0;
    }

    if (baud_index < 0 || baud_index >= CAN_HAL_BAUD_COUNT)
        return 0;

    if (priv->pfVciInitialize)
        priv->pfVciInitialize();

    int rc = priv->pfVciDeviceOpen(0, &priv->hDevice);
    if (rc != 0) {
        char log[64];
        sprintf(log, "IXXAT: device open failed (rc=%d)", rc);
        ixxat_log(hal, log);
        return 0;
    }

    rc = priv->pfCanControlOpen(priv->hDevice, 0, &priv->hControl);
    if (rc != 0) {
        char log[64];
        sprintf(log, "IXXAT: control open failed (rc=%d)", rc);
        ixxat_log(hal, log);
        priv->pfVciDeviceClose(priv->hDevice);
        priv->hDevice = NULL;
        return 0;
    }

    BYTE btr0 = ixxat_baud_table[baud_index].btr0;
    BYTE btr1 = ixxat_baud_table[baud_index].btr1;
    rc = priv->pfCanControlInitialize(priv->hControl, 0, btr0, btr1, 0);
    if (rc != 0) {
        char log[64];
        sprintf(log, "IXXAT: control init failed (rc=%d)", rc);
        ixxat_log(hal, log);
        priv->pfCanControlClose(priv->hControl);
        priv->pfVciDeviceClose(priv->hDevice);
        priv->hControl = NULL;
        priv->hDevice = NULL;
        return 0;
    }

    rc = priv->pfCanChannelOpen(priv->hDevice, 0, 0, &priv->hChannel);
    if (rc != 0) {
        char log[64];
        sprintf(log, "IXXAT: channel open failed (rc=%d)", rc);
        ixxat_log(hal, log);
        priv->pfCanControlClose(priv->hControl);
        priv->pfVciDeviceClose(priv->hDevice);
        priv->hControl = NULL;
        priv->hDevice = NULL;
        return 0;
    }

    rc = priv->pfCanChannelInitialize(priv->hChannel, 1024, 1024);
    if (rc != 0) {
        char log[64];
        sprintf(log, "IXXAT: channel init failed (rc=%d)", rc);
        ixxat_log(hal, log);
        priv->pfCanChannelClose(priv->hChannel);
        priv->pfCanControlClose(priv->hControl);
        priv->pfVciDeviceClose(priv->hDevice);
        priv->hChannel = NULL;
        priv->hControl = NULL;
        priv->hDevice = NULL;
        return 0;
    }

    rc = priv->pfCanChannelActivate(priv->hChannel, 1);
    if (rc != 0) {
        char log[64];
        sprintf(log, "IXXAT: channel activate failed (rc=%d)", rc);
        ixxat_log(hal, log);
        priv->pfCanChannelClose(priv->hChannel);
        priv->pfCanControlClose(priv->hControl);
        priv->pfVciDeviceClose(priv->hDevice);
        priv->hChannel = NULL;
        priv->hControl = NULL;
        priv->hDevice = NULL;
        return 0;
    }

    rc = priv->pfCanControlStart(priv->hControl, 1);
    if (rc != 0) {
        char log[64];
        sprintf(log, "IXXAT: control start failed (rc=%d)", rc);
        ixxat_log(hal, log);
        priv->pfCanChannelClose(priv->hChannel);
        priv->pfCanControlClose(priv->hControl);
        priv->pfVciDeviceClose(priv->hDevice);
        priv->hChannel = NULL;
        priv->hControl = NULL;
        priv->hDevice = NULL;
        return 0;
    }

    hal->channel = channel;
    hal->connected = 1;

    char log[64];
    sprintf(log, "IXXAT: connected (channel=%d)", channel);
    ixxat_log(hal, log);
    return 1;
}

static void ixxat_disconnect(CanHal *hal)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;

    if (hal->connected && priv->hDll) {
        if (priv->hChannel) {
            priv->pfCanChannelActivate(priv->hChannel, 0);
            priv->pfCanChannelClose(priv->hChannel);
            priv->hChannel = NULL;
        }
        if (priv->hControl) {
            priv->pfCanControlReset(priv->hControl);
            priv->pfCanControlClose(priv->hControl);
            priv->hControl = NULL;
        }
        if (priv->hDevice) {
            priv->pfVciDeviceClose(priv->hDevice);
            priv->hDevice = NULL;
        }
        ixxat_log(hal, "IXXAT: disconnected");
    }
    hal->channel = CAN_HAL_INVALID_HANDLE;
    hal->connected = 0;
}

static int ixxat_write(CanHal *hal, const CanHalFrame *frame)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;
    if (!hal->connected || !priv->pfCanChannelSendMessage) return 0;

    BYTE msg[80];
    memset(msg, 0, sizeof(msg));

    DWORD *pId = (DWORD *)msg;
    *pId = frame->id;

    if (frame->flags & CAN_HAL_FLAG_EXTENDED) {
        DWORD *pMask = (DWORD *)(msg + 4);
        *pMask = 0x20000000;
    }
    if (frame->flags & CAN_HAL_FLAG_REMOTE) {
        DWORD *pMask = (DWORD *)(msg + 4);
        *pMask |= 0x00000001;
    }

    BYTE *pDlc = msg + 8;
    *pDlc = (frame->dlc > 8) ? 8 : frame->dlc;

    memcpy(msg + 12, frame->data, *pDlc);

    int rc = priv->pfCanChannelSendMessage(priv->hChannel, 100, msg);
    return (rc == 0) ? 1 : 0;
}

static int ixxat_read(CanHal *hal, CanHalFrame *frame, int timeout_ms)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;
    if (!hal->connected || !priv->pfCanChannelReadMessage) return 0;

    DWORD start = GetTickCount();

    while ((int)(GetTickCount() - start) < timeout_ms) {
        BYTE msg[256];
        memset(msg, 0, sizeof(msg));
        int rc = priv->pfCanChannelReadMessage(priv->hChannel, 10, msg);
        if (rc == 0) {
            DWORD *pId = (DWORD *)msg;
            frame->id = *pId;

            BYTE *pDlc = msg + 8;
            frame->dlc = *pDlc;
            if (frame->dlc > 8) frame->dlc = 8;

            memcpy(frame->data, msg + 12, frame->dlc);

            frame->flags = 0;
            if (msg[4] & 0x20)
                frame->flags |= CAN_HAL_FLAG_EXTENDED;
            if (msg[4] & 0x01)
                frame->flags |= CAN_HAL_FLAG_REMOTE;
            return 1;
        }
    }
    return 0;
}

static int ixxat_set_filter(CanHal *hal, uint32_t from_id, uint32_t to_id)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;
    if (!hal->connected || !priv->pfCanControlAddFilterIds) return 0;

    priv->pfCanControlAddFilterIds(priv->hControl, from_id, to_id, 0);
    return 1;
}

static void ixxat_destroy(CanHal *hal)
{
    if (hal) {
        if (hal->connected)
            ixxat_disconnect(hal);
        IxxatPriv *priv = (IxxatPriv *)hal->priv;
        if (priv) {
            if (priv->hDll)
                FreeLibrary(priv->hDll);
            free(priv);
        }
        free(hal);
    }
}

static int ixxat_load_dll(CanHal *hal)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;

    static const wchar_t *dll_names[] = {
        L"vcinpl.dll",
        L"vcinpl2.dll",
        L"vci11.dll",
        L"vci11um.dll",
        NULL
    };

    for (int i = 0; dll_names[i]; i++) {
        SetLastError(0);
        priv->hDll = LoadLibraryW(dll_names[i]);
        if (priv->hDll) {
            char log[128];
            char name[64];
            WideCharToMultiByte(CP_UTF8, 0, dll_names[i], -1, name, 64, NULL, NULL);
            sprintf(log, "IXXAT: loaded %s", name);
            ixxat_log(hal, log);
            break;
        } else {
            DWORD err = GetLastError();
            char log[128];
            char name[64];
            WideCharToMultiByte(CP_UTF8, 0, dll_names[i], -1, name, 64, NULL, NULL);
            if (err == 126) {
                sprintf(log, "IXXAT: %s found but missing dependencies (err=%lu)", name, err);
            } else if (err == 193) {
                sprintf(log, "IXXAT: %s wrong architecture (err=%lu)", name, err);
            } else {
                sprintf(log, "IXXAT: %s load failed (err=%lu)", name, err);
            }
            ixxat_log(hal, log);
        }
    }

    if (!priv->hDll) {
        ixxat_log(hal, "IXXAT: no compatible VCI DLL found");
        return 0;
    }

    static const struct {
        const char *name;
        size_t offset;
    } func_table[] = {
        { "vciInitialize",            offsetof(IxxatPriv, pfVciInitialize) },
        { "vciEnumDeviceOpen",        offsetof(IxxatPriv, pfVciEnumDeviceOpen) },
        { "vciEnumDeviceNext",        offsetof(IxxatPriv, pfVciEnumDeviceNext) },
        { "vciEnumDeviceClose",       offsetof(IxxatPriv, pfVciEnumDeviceClose) },
        { "vciDeviceOpen",            offsetof(IxxatPriv, pfVciDeviceOpen) },
        { "vciDeviceClose",           offsetof(IxxatPriv, pfVciDeviceClose) },
        { "vciDeviceGetInfo",         offsetof(IxxatPriv, pfVciDeviceGetInfo) },
        { "canControlOpen",           offsetof(IxxatPriv, pfCanControlOpen) },
        { "canControlClose",          offsetof(IxxatPriv, pfCanControlClose) },
        { "canControlInitialize",     offsetof(IxxatPriv, pfCanControlInitialize) },
        { "canControlStart",          offsetof(IxxatPriv, pfCanControlStart) },
        { "canControlReset",          offsetof(IxxatPriv, pfCanControlReset) },
        { "canControlAddFilterIds",   offsetof(IxxatPriv, pfCanControlAddFilterIds) },
        { "canChannelOpen",           offsetof(IxxatPriv, pfCanChannelOpen) },
        { "canChannelClose",          offsetof(IxxatPriv, pfCanChannelClose) },
        { "canChannelInitialize",     offsetof(IxxatPriv, pfCanChannelInitialize) },
        { "canChannelActivate",       offsetof(IxxatPriv, pfCanChannelActivate) },
        { "canChannelSendMessage",    offsetof(IxxatPriv, pfCanChannelSendMessage) },
        { "canChannelReadMessage",    offsetof(IxxatPriv, pfCanChannelReadMessage) },
        { NULL, 0 }
    };

    int missing = 0;
    for (int i = 0; func_table[i].name; i++) {
        FARPROC proc = GetProcAddress(priv->hDll, func_table[i].name);
        if (proc) {
            *(FARPROC*)((char*)priv + func_table[i].offset) = proc;
        } else {
            char log[128];
            sprintf(log, "IXXAT: function %s not found", func_table[i].name);
            ixxat_log(hal, log);
            missing++;
        }
    }

    if (!priv->pfVciDeviceOpen || !priv->pfCanControlOpen || !priv->pfCanControlInitialize || !priv->pfCanChannelOpen) {
        char log[64];
        sprintf(log, "IXXAT: %d required functions missing", missing);
        ixxat_log(hal, log);
        FreeLibrary(priv->hDll);
        priv->hDll = NULL;
        return 0;
    }

    return 1;
}

static const CanHalOps ixxat_ops = {
    .name       = "IXXAT",
    .detect     = ixxat_detect,
    .connect    = ixxat_connect,
    .disconnect = ixxat_disconnect,
    .write      = ixxat_write,
    .read       = ixxat_read,
    .set_filter = ixxat_set_filter,
    .destroy    = ixxat_destroy,
};

CanHal *CanHal_CreateIXXAT(void)
{
    CanHal *hal = (CanHal *)calloc(1, sizeof(CanHal));
    if (!hal) return NULL;

    IxxatPriv *priv = (IxxatPriv *)calloc(1, sizeof(IxxatPriv));
    if (!priv) {
        free(hal);
        return NULL;
    }

    hal->ops = &ixxat_ops;
    hal->channel = CAN_HAL_INVALID_HANDLE;
    hal->connected = 0;
    hal->priv = priv;
    hal->log_cb = NULL;

    ixxat_load_dll(hal);

    return hal;
}
