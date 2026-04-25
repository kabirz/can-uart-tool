#ifndef CAN_MANAGER_H
#define CAN_MANAGER_H

#include <windows.h>
#include <stdint.h>
#include <PCANBasic.h>

#define MAX_DEVICES 16
#define VIRTUAL_CAN_CHANNEL  ((TPCANHandle)0xFFFF)

#ifdef __cplusplus
extern "C" {
#endif

// CAN ID 定义
#define PLATFORM_RX     0x101    // 发送到板卡的命令
#define PLATFORM_TX     0x102    // 板卡响应
#define FW_DATA_RX      0x103    // 固件数据

// 板卡命令
#define BOARD_START_UPDATE  0
#define BOARD_CONFIRM       1
#define BOARD_VERSION       2
#define BOARD_REBOOT        3

// 固件响应码
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

// 日志回调函数类型
typedef void (*msgCallback)(const char * msg);
// 进度回调函数类型
typedef void (*progressCallback)(int percent);

// CAN 管理器不透明句柄
typedef struct CanManager CanManager;

// 创建/销毁
CanManager* CanManager_Create(void);
void CanManager_Destroy(CanManager* mgr);

// 设置回调函数
void CanManager_SetCallback(CanManager* mgr, msgCallback msg_call);
void CanManager_SetProgressCallback(CanManager* mgr, progressCallback progress_call);

// 操作函数
int CanManager_Connect(CanManager* mgr, TPCANHandle channel, TPCANBaudrate baudrate);
void CanManager_Disconnect(CanManager* mgr);
uint32_t CanManager_GetFirmwareVersion(CanManager* mgr);
int CanManager_BoardReboot(CanManager* mgr);
int CanManager_FirmwareUpgrade(CanManager* mgr, const wchar_t* fileName, int testMode);
int CanManager_DetectDevice(CanManager* mgr, TPCANHandle* channels, int maxCount);

#ifdef __cplusplus
}
#endif

#endif // CAN_MANAGER_H
