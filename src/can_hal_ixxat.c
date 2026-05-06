#include "can_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>

#define VCI_MAX_CHANSEG     64

#pragma pack(push, 1)

typedef struct {
    DWORD dwClockFreq;
    BYTE  dwTseg1;
    BYTE  dwTseg2;
    BYTE  dwSJW;
    BYTE  dwBRP;
    BYTE  dwSAM;
} VCI_BT_CONST;

typedef struct {
    DWORD dwBtr0;
    DWORD dwBtr1;
} VCI_BITRATE;

typedef struct {
    DWORD dwErrorCode;
    DWORD dwCanStatus;
    DWORD dwRxErrCount;
    DWORD dwTxErrCount;
} VCI_CAN_ERR_INFO;

typedef struct {
    DWORD dwTime;
    DWORD dwMsgId;
    BYTE  bCtrl;
    BYTE  bDLC;
    BYTE  abData[8];
} VCI_CAN_OBJ;

#pragma pack(pop)

typedef struct {
    DWORD dwProductId;
    DWORD dwHardwareVersion;
    DWORD dwSoftwareVersion;
    char  abVendorName[32];
    char  abDeviceName[32];
} VCI_DEV_DESC;

typedef struct {
    BYTE  bPort;
    BYTE  bIrq;
    DWORD dwBaseAddress;
} VCI_DEV_CAPS;

typedef HRESULT (WINAPI *fnVCI2_InitDevice)(DWORD, DWORD, HANDLE*);
typedef HRESULT (WINAPI *fnVCI2_ReleaseDevice)(HANDLE);
typedef HRESULT (WINAPI *fnVCI2_FindHardware)(DWORD, DWORD, DWORD, VCI_DEV_DESC*, DWORD*);
typedef HRESULT (WINAPI *fnVCI2_OpenPort)(HANDLE, DWORD, HANDLE*);
typedef HRESULT (WINAPI *fnVCI2_ClosePort)(HANDLE);
typedef HRESULT (WINAPI *fnVCI2_InitCan)(HANDLE, HANDLE, DWORD, VCI_BITRATE*);
typedef HRESULT (WINAPI *fnVCI2_ResetCan)(HANDLE, HANDLE);
typedef HRESULT (WINAPI *fnVCI2_ReadCanObj)(HANDLE, HANDLE, DWORD, VCI_CAN_OBJ*, DWORD);
typedef HRESULT (WINAPI *fnVCI2_WriteCanObj)(HANDLE, HANDLE, DWORD, VCI_CAN_OBJ*);

static void ixxat_log(CanHal *hal, const char *msg)
{
    if (hal && hal->log_cb)
        hal->log_cb(msg);
}

typedef struct {
    HMODULE hVciDll;
    HANDLE  hDevice;
    HANDLE  hPort;
    fnVCI2_InitDevice     pfVCI_InitDevice;
    fnVCI2_ReleaseDevice  pfVCI_ReleaseDevice;
    fnVCI2_FindHardware   pfVCI_FindHardware;
    fnVCI2_OpenPort       pfVCI_OpenPort;
    fnVCI2_ClosePort      pfVCI_ClosePort;
    fnVCI2_InitCan        pfVCI_InitCan;
    fnVCI2_ResetCan       pfVCI_ResetCan;
    fnVCI2_ReadCanObj     pfVCI_ReadCanObj;
    fnVCI2_WriteCanObj    pfVCI_WriteCanObj;
} IxxatPriv;

static VCI_BITRATE ixxat_baud_table[CAN_HAL_BAUD_COUNT] = {
    {0x0031, 0x1C},  /* 10K   */
    {0x0093, 0x1C},  /* 20K   */
    {0x00EF, 0x1C},  /* 50K   */
    {0x0031, 0x09},  /* 100K  */
    {0x0018, 0x09},  /* 125K  */
    {0x0031, 0x04},  /* 250K  */
    {0x0018, 0x04},  /* 500K  */
    {0x0009, 0x04},  /* 1M    */
};

static int ixxat_detect(CanHal *hal, int *channels, int max_count)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;
    int count = 0;

    if (!priv->hVciDll) {
        ixxat_log(hal, "IXXAT: VCI DLL not loaded");
        return 0;
    }

    if (!priv->pfVCI_FindHardware) {
        ixxat_log(hal, "IXXAT: VCI_FindHardware not available");
        return 0;
    }

    for (DWORD i = 0; i < 16 && count < max_count; i++) {
        VCI_DEV_DESC desc;
        DWORD dwCount = 1;
        HRESULT hr = priv->pfVCI_FindHardware(0, 0, 0, &desc, &dwCount);
        if (hr == 0 && dwCount > 0) {
            channels[count++] = (int)i;
        } else {
            break;
        }
    }

    char log[64];
    sprintf(log, "IXXAT: detected %d device(s)", count);
    ixxat_log(hal, log);
    return count;
}

static int ixxat_connect(CanHal *hal, int channel, int baud_index)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;

    if (!priv->hVciDll) {
        ixxat_log(hal, "IXXAT: VCI DLL not loaded");
        return 0;
    }

    if (baud_index < 0 || baud_index >= CAN_HAL_BAUD_COUNT)
        return 0;

    HRESULT hr;

    hr = priv->pfVCI_InitDevice(0, 0, &priv->hDevice);
    if (hr != 0) {
        ixxat_log(hal, "IXXAT: InitDevice failed");
        return 0;
    }

    hr = priv->pfVCI_OpenPort(priv->hDevice, 0, &priv->hPort);
    if (hr != 0) {
        ixxat_log(hal, "IXXAT: OpenPort failed");
        priv->pfVCI_ReleaseDevice(priv->hDevice);
        return 0;
    }

    hr = priv->pfVCI_InitCan(priv->hDevice, priv->hPort, 0,
                              &ixxat_baud_table[baud_index]);
    if (hr != 0) {
        ixxat_log(hal, "IXXAT: InitCAN failed");
        priv->pfVCI_ClosePort(priv->hPort);
        priv->pfVCI_ReleaseDevice(priv->hDevice);
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

    if (hal->connected && priv->hVciDll) {
        if (priv->pfVCI_ResetCan)
            priv->pfVCI_ResetCan(priv->hDevice, priv->hPort);
        if (priv->pfVCI_ClosePort)
            priv->pfVCI_ClosePort(priv->hPort);
        if (priv->pfVCI_ReleaseDevice)
            priv->pfVCI_ReleaseDevice(priv->hDevice);
        ixxat_log(hal, "IXXAT: disconnected");
    }
    hal->channel = CAN_HAL_INVALID_HANDLE;
    hal->connected = 0;
}

static int ixxat_write(CanHal *hal, const CanHalFrame *frame)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;
    if (!hal->connected || !priv->pfVCI_WriteCanObj) return 0;

    VCI_CAN_OBJ obj;
    memset(&obj, 0, sizeof(obj));
    obj.dwMsgId = frame->id;
    obj.bDLC = (frame->dlc > 8) ? 8 : frame->dlc;
    obj.bCtrl = 0x00;
    if (frame->flags & CAN_HAL_FLAG_EXTENDED)
        obj.bCtrl |= 0x80;
    if (frame->flags & CAN_HAL_FLAG_REMOTE)
        obj.bCtrl |= 0x40;
    memcpy(obj.abData, frame->data, obj.bDLC);

    HRESULT hr = priv->pfVCI_WriteCanObj(priv->hDevice, priv->hPort, 0, &obj);
    return (hr == 0) ? 1 : 0;
}

static int ixxat_read(CanHal *hal, CanHalFrame *frame, int timeout_ms)
{
    IxxatPriv *priv = (IxxatPriv *)hal->priv;
    if (!hal->connected || !priv->pfVCI_ReadCanObj) return 0;

    VCI_CAN_OBJ obj;
    DWORD dwCount = 1;
    DWORD start = GetTickCount();

    while ((int)(GetTickCount() - start) < timeout_ms) {
        HRESULT hr = priv->pfVCI_ReadCanObj(priv->hDevice, priv->hPort, 0,
                                             &obj, dwCount);
        if (hr == 0) {
            frame->id = obj.dwMsgId;
            frame->dlc = obj.bDLC;
            memcpy(frame->data, obj.abData, obj.bDLC);
            frame->flags = 0;
            if (obj.bCtrl & 0x80)
                frame->flags |= CAN_HAL_FLAG_EXTENDED;
            if (obj.bCtrl & 0x40)
                frame->flags |= CAN_HAL_FLAG_REMOTE;
            return 1;
        }
        Sleep(1);
    }
    return 0;
}

static int ixxat_set_filter(CanHal *hal, uint32_t from_id, uint32_t to_id)
{
    return 1;
}

static void ixxat_destroy(CanHal *hal)
{
    if (hal) {
        if (hal->connected)
            ixxat_disconnect(hal);
        IxxatPriv *priv = (IxxatPriv *)hal->priv;
        if (priv) {
            if (priv->hVciDll)
                FreeLibrary(priv->hVciDll);
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
        L"vcisdk.dll",
        NULL
    };

    for (int i = 0; dll_names[i]; i++) {
        priv->hVciDll = LoadLibraryW(dll_names[i]);
        if (priv->hVciDll) {
            char log[128];
            WideCharToMultiByte(CP_UTF8, 0, dll_names[i], -1, log + 32, 80, NULL, NULL);
            sprintf(log, "IXXAT: loaded %s", log + 32);
            ixxat_log(hal, log);
            break;
        }
    }

    if (!priv->hVciDll) {
        ixxat_log(hal, "IXXAT: vcinpl.dll not found, IXXAT support unavailable");
        return 0;
    }

    priv->pfVCI_InitDevice    = (fnVCI2_InitDevice)   GetProcAddress(priv->hVciDll, "VCI_InitDevice");
    priv->pfVCI_ReleaseDevice = (fnVCI2_ReleaseDevice) GetProcAddress(priv->hVciDll, "VCI_ReleaseDevice");
    priv->pfVCI_FindHardware  = (fnVCI2_FindHardware)  GetProcAddress(priv->hVciDll, "VCI_FindHardware");
    priv->pfVCI_OpenPort      = (fnVCI2_OpenPort)      GetProcAddress(priv->hVciDll, "VCI_OpenPort");
    priv->pfVCI_ClosePort     = (fnVCI2_ClosePort)     GetProcAddress(priv->hVciDll, "VCI_ClosePort");
    priv->pfVCI_InitCan       = (fnVCI2_InitCan)       GetProcAddress(priv->hVciDll, "VCI_InitCan");
    priv->pfVCI_ResetCan      = (fnVCI2_ResetCan)      GetProcAddress(priv->hVciDll, "VCI_ResetCan");
    priv->pfVCI_ReadCanObj    = (fnVCI2_ReadCanObj)    GetProcAddress(priv->hVciDll, "VCI_ReadCanObj");
    priv->pfVCI_WriteCanObj   = (fnVCI2_WriteCanObj)   GetProcAddress(priv->hVciDll, "VCI_WriteCanObj");

    if (!priv->pfVCI_InitDevice || !priv->pfVCI_OpenPort || !priv->pfVCI_InitCan) {
        ixxat_log(hal, "IXXAT: required functions not found in DLL");
        FreeLibrary(priv->hVciDll);
        priv->hVciDll = NULL;
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
