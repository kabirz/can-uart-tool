#ifndef CAN_MANAGER_H
#define CAN_MANAGER_H

#include <windows.h>
#include <stdint.h>
#include "can_hal.h"

#define MAX_DEVICES 16

#ifdef __cplusplus
extern "C" {
#endif

#define PLATFORM_RX     0x101
#define PLATFORM_TX     0x102
#define FW_DATA_RX      0x103

#define BOARD_START_UPDATE  0
#define BOARD_CONFIRM       1
#define BOARD_VERSION       2
#define BOARD_REBOOT        3

#define FW_CODE_OFFSET          0
#define FW_CODE_UPDATE_SUCCESS  1
#define FW_CODE_VERSION         2
#define FW_CODE_CONFIRM         3
#define FW_CODE_FLASH_ERROR     4
#define FW_CODE_TRANFER_ERROR   5

typedef struct {
    uint32_t code;
    uint32_t val;
} can_frame_t;

typedef struct CanDispatcher CanDispatcher;

typedef void (*msgCallback)(const char *msg, void *ctx);
typedef void (*progressCallback)(int percent, void *ctx);

typedef struct CanManager CanManager;

CanManager* CanManager_Create(CanHal *hal, CanDispatcher *disp);
void CanManager_Destroy(CanManager* mgr);

void CanManager_SetCallback(CanManager* mgr, msgCallback msg_call, void *ctx);
void CanManager_SetProgressCallback(CanManager* mgr, progressCallback progress_call, void *ctx);

int CanManager_Connect(CanManager* mgr, int channel, int baud_index);
void CanManager_Disconnect(CanManager* mgr);
uint32_t CanManager_GetFirmwareVersion(CanManager* mgr);
int CanManager_BoardReboot(CanManager* mgr);
int CanManager_FirmwareUpgrade(CanManager* mgr, const wchar_t* fileName, int testMode);
int CanManager_DetectDevice(CanManager* mgr, int* channels, int maxCount);
int CanManager_IsVirtualChannel(int channel);
CanHal *CanManager_GetHal(CanManager* mgr);
CanDispatcher *CanManager_GetDisp(CanManager* mgr);

/* Adapter switch: replace HAL + dispatcher (must be disconnected) */
void CanManager_ReplaceHal(CanManager *mgr, CanHal *newHal, CanDispatcher *newDisp);

#ifdef __cplusplus
}
#endif

#endif
