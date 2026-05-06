#include "can_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>

#define PCAN_ERROR_OK           0x00000U
#define PCAN_ERROR_QRCVEMPTY    0x00020U

#define PCAN_NONEBUS            0x00U
#define PCAN_MESSAGE_STANDARD   0x00U
#define PCAN_MESSAGE_RTR        0x01U
#define PCAN_MESSAGE_EXTENDED   0x02U
#define PCAN_MODE_STANDARD      PCAN_MESSAGE_STANDARD
#define PCAN_MODE_EXTENDED      PCAN_MESSAGE_EXTENDED

#define PCAN_BAUD_10K           0x672FU
#define PCAN_BAUD_20K           0x532FU
#define PCAN_BAUD_50K           0x472FU
#define PCAN_BAUD_100K          0x432FU
#define PCAN_BAUD_125K          0x031CU
#define PCAN_BAUD_250K          0x011CU
#define PCAN_BAUD_500K          0x001CU
#define PCAN_BAUD_1M            0x0014U

#pragma pack(push, 1)

typedef struct {
    DWORD ID;
    BYTE  MSGTYPE;
    BYTE  LEN;
    BYTE  DATA[8];
} PCAN_MSG;

typedef struct {
    DWORD millis;
    WORD  millis_overflow;
    WORD  micros;
} PCAN_TIMESTAMP;

#pragma pack(pop)

typedef DWORD (WINAPI *fnCAN_Initialize)(WORD, WORD, BYTE, DWORD, WORD);
typedef DWORD (WINAPI *fnCAN_Uninitialize)(WORD);
typedef DWORD (WINAPI *fnCAN_Write)(WORD, PCAN_MSG*);
typedef DWORD (WINAPI *fnCAN_Read)(WORD, PCAN_MSG*, PCAN_TIMESTAMP*);
typedef DWORD (WINAPI *fnCAN_FilterMessages)(WORD, DWORD, DWORD, BYTE);
typedef DWORD (WINAPI *fnCAN_LookUpChannel)(LPSTR, WORD*);

typedef struct {
    HMODULE             hDll;
    fnCAN_Initialize    pfCAN_Initialize;
    fnCAN_Uninitialize  pfCAN_Uninitialize;
    fnCAN_Write         pfCAN_Write;
    fnCAN_Read          pfCAN_Read;
    fnCAN_FilterMessages pfCAN_FilterMessages;
    fnCAN_LookUpChannel pfCAN_LookUpChannel;
} PcanPriv;

static const WORD pcan_baud_table[CAN_HAL_BAUD_COUNT] = {
    PCAN_BAUD_10K, PCAN_BAUD_20K, PCAN_BAUD_50K, PCAN_BAUD_100K,
    PCAN_BAUD_125K, PCAN_BAUD_250K, PCAN_BAUD_500K, PCAN_BAUD_1M
};

static void pcan_log(CanHal *hal, const char *msg)
{
    if (hal && hal->log_cb)
        hal->log_cb(msg);
}

static int pcan_detect(CanHal *hal, int *channels, int max_count)
{
    PcanPriv *priv = (PcanPriv *)hal->priv;
    if (!priv->hDll || !priv->pfCAN_LookUpChannel) {
        pcan_log(hal, "PCAN: DLL not loaded");
        return 0;
    }

    int count = 0;
    char msg[64];
    for (int i = 0; i < 16 && count < max_count; i++) {
        WORD channel = PCAN_NONEBUS;
        sprintf(msg, "devicetype=pcan_usb,controllernumber=%d", i);
        DWORD result = priv->pfCAN_LookUpChannel(msg, &channel);
        if (result == PCAN_ERROR_OK && channel != PCAN_NONEBUS) {
            channels[count++] = (int)channel;
        }
    }
    char log[64];
    sprintf(log, "PCAN: detected %d device(s)", count);
    pcan_log(hal, log);
    return count;
}

static int pcan_connect(CanHal *hal, int channel, int baud_index)
{
    PcanPriv *priv = (PcanPriv *)hal->priv;
    if (!priv->hDll || !priv->pfCAN_Initialize) return 0;

    if (baud_index < 0 || baud_index >= CAN_HAL_BAUD_COUNT)
        return 0;

    DWORD status = priv->pfCAN_Initialize((WORD)channel,
                                           pcan_baud_table[baud_index],
                                           0, 0, 0);
    if (status != PCAN_ERROR_OK) {
        pcan_log(hal, "PCAN: initialization failed");
        return 0;
    }

    hal->channel = channel;
    hal->connected = 1;

    char log[64];
    sprintf(log, "PCAN: connected (channel=0x%x)", channel);
    pcan_log(hal, log);
    return 1;
}

static void pcan_disconnect(CanHal *hal)
{
    PcanPriv *priv = (PcanPriv *)hal->priv;
    if (hal->channel != CAN_HAL_INVALID_HANDLE && hal->channel != CAN_HAL_VIRTUAL_CHANNEL) {
        if (priv->pfCAN_Uninitialize)
            priv->pfCAN_Uninitialize((WORD)hal->channel);
        char log[64];
        sprintf(log, "PCAN: disconnected (channel=0x%x)", hal->channel);
        pcan_log(hal, log);
    }
    hal->channel = CAN_HAL_INVALID_HANDLE;
    hal->connected = 0;
}

static int pcan_write(CanHal *hal, const CanHalFrame *frame)
{
    PcanPriv *priv = (PcanPriv *)hal->priv;
    if (!hal->connected || !priv->pfCAN_Write) return 0;

    PCAN_MSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.ID = frame->id;
    msg.LEN = (frame->dlc > 8) ? 8 : frame->dlc;
    msg.MSGTYPE = (frame->flags & CAN_HAL_FLAG_EXTENDED) ? PCAN_MESSAGE_EXTENDED
                                                          : PCAN_MESSAGE_STANDARD;
    if (frame->flags & CAN_HAL_FLAG_REMOTE)
        msg.MSGTYPE |= PCAN_MESSAGE_RTR;
    memcpy(msg.DATA, frame->data, msg.LEN);

    DWORD status = priv->pfCAN_Write((WORD)hal->channel, &msg);
    return (status == PCAN_ERROR_OK) ? 1 : 0;
}

static int pcan_read(CanHal *hal, CanHalFrame *frame, int timeout_ms)
{
    PcanPriv *priv = (PcanPriv *)hal->priv;
    if (!hal->connected || !priv->pfCAN_Read) return 0;

    PCAN_MSG msg;
    PCAN_TIMESTAMP ts;
    DWORD start = GetTickCount();

    while ((int)(GetTickCount() - start) < timeout_ms) {
        DWORD status = priv->pfCAN_Read((WORD)hal->channel, &msg, &ts);
        if (status == PCAN_ERROR_OK) {
            frame->id = msg.ID;
            frame->dlc = msg.LEN;
            memcpy(frame->data, msg.DATA, msg.LEN);
            frame->flags = 0;
            if (msg.MSGTYPE & PCAN_MESSAGE_EXTENDED)
                frame->flags |= CAN_HAL_FLAG_EXTENDED;
            if (msg.MSGTYPE & PCAN_MESSAGE_RTR)
                frame->flags |= CAN_HAL_FLAG_REMOTE;
            return 1;
        } else if (status == PCAN_ERROR_QRCVEMPTY) {
            Sleep(1);
        } else {
            Sleep(10);
        }
    }
    return 0;
}

static int pcan_set_filter(CanHal *hal, uint32_t from_id, uint32_t to_id)
{
    PcanPriv *priv = (PcanPriv *)hal->priv;
    if (!hal->connected || !priv->pfCAN_FilterMessages) return 0;
    DWORD status = priv->pfCAN_FilterMessages((WORD)hal->channel,
                                               from_id, to_id, PCAN_MODE_STANDARD);
    return (status == PCAN_ERROR_OK) ? 1 : 0;
}

static void pcan_destroy(CanHal *hal)
{
    if (hal) {
        if (hal->connected)
            pcan_disconnect(hal);
        PcanPriv *priv = (PcanPriv *)hal->priv;
        if (priv) {
            if (priv->hDll)
                FreeLibrary(priv->hDll);
            free(priv);
        }
        free(hal);
    }
}

static int pcan_load_dll(CanHal *hal)
{
    PcanPriv *priv = (PcanPriv *)hal->priv;

    static const wchar_t *dll_names[] = {
        L"PCANBasic.dll",
        NULL
    };

    for (int i = 0; dll_names[i]; i++) {
        priv->hDll = LoadLibraryW(dll_names[i]);
        if (priv->hDll) {
            char log[128];
            WideCharToMultiByte(CP_UTF8, 0, dll_names[i], -1, log + 32, 80, NULL, NULL);
            sprintf(log, "PCAN: loaded %s", log + 32);
            pcan_log(hal, log);
            break;
        }
    }

    if (!priv->hDll) {
        pcan_log(hal, "PCAN: PCANBasic.dll not found, PCAN support unavailable");
        return 0;
    }

    priv->pfCAN_Initialize     = (fnCAN_Initialize)    GetProcAddress(priv->hDll, "CAN_Initialize");
    priv->pfCAN_Uninitialize   = (fnCAN_Uninitialize)  GetProcAddress(priv->hDll, "CAN_Uninitialize");
    priv->pfCAN_Write          = (fnCAN_Write)          GetProcAddress(priv->hDll, "CAN_Write");
    priv->pfCAN_Read           = (fnCAN_Read)           GetProcAddress(priv->hDll, "CAN_Read");
    priv->pfCAN_FilterMessages = (fnCAN_FilterMessages) GetProcAddress(priv->hDll, "CAN_FilterMessages");
    priv->pfCAN_LookUpChannel  = (fnCAN_LookUpChannel)  GetProcAddress(priv->hDll, "CAN_LookUpChannel");

    if (!priv->pfCAN_Initialize || !priv->pfCAN_Read || !priv->pfCAN_Write || !priv->pfCAN_LookUpChannel) {
        pcan_log(hal, "PCAN: required functions not found in DLL");
        FreeLibrary(priv->hDll);
        priv->hDll = NULL;
        return 0;
    }

    return 1;
}

static const CanHalOps pcan_ops = {
    .name       = "PCAN",
    .detect     = pcan_detect,
    .connect    = pcan_connect,
    .disconnect = pcan_disconnect,
    .write      = pcan_write,
    .read       = pcan_read,
    .set_filter = pcan_set_filter,
    .destroy    = pcan_destroy,
};

CanHal *CanHal_CreatePCAN(void)
{
    CanHal *hal = (CanHal *)calloc(1, sizeof(CanHal));
    if (!hal) return NULL;

    PcanPriv *priv = (PcanPriv *)calloc(1, sizeof(PcanPriv));
    if (!priv) {
        free(hal);
        return NULL;
    }

    hal->ops = &pcan_ops;
    hal->channel = CAN_HAL_INVALID_HANDLE;
    hal->connected = 0;
    hal->priv = priv;
    hal->log_cb = NULL;

    pcan_load_dll(hal);

    return hal;
}
