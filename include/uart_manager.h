#ifndef UART_MANAGER_H
#define UART_MANAGER_H

#include <windows.h>
#include <stdint.h>

#define MAX_SERIAL_PORTS      32
#define MAX_PORT_NAME_LEN     32

#ifdef __cplusplus
extern "C" {
#endif

// UART 帧格式定义
#define FRAME_HEAD            0xAA
#define FRAME_TAIL            0x55
#define FRAME_TYPE_CMD        0x01
#define FRAME_TYPE_DATA       0x02

// 板卡命令（与 CAN 相同）
#define BOARD_START_UPDATE    0
#define BOARD_CONFIRM         1
#define BOARD_VERSION         2
#define BOARD_REBOOT          3

// 固件响应码（与 CAN 相同）
#define FW_CODE_OFFSET          0
#define FW_CODE_UPDATE_SUCCESS  1
#define FW_CODE_VERSION         2
#define FW_CODE_CONFIRM         3
#define FW_CODE_FLASH_ERROR     4
#define FW_CODE_TRANFER_ERROR   5

// 串口信息结构
typedef struct {
    char portName[MAX_PORT_NAME_LEN];  // 如 "COM3"
    char friendlyName[128];             // 友好名称描述
} SerialPortInfo;

// 日志回调函数类型
typedef void (*UartMsgCallback)(const char* msg);
// 进度回调函数类型
typedef void (*UartProgressCallback)(int percent);

// UART 管理器不透明句柄
typedef struct UartManager UartManager;

// 创建/销毁
UartManager* UartManager_Create(void);
void UartManager_Destroy(UartManager* mgr);

// 设置回调函数
void UartManager_SetCallback(UartManager* mgr, UartMsgCallback msg_call);
void UartManager_SetProgressCallback(UartManager* mgr, UartProgressCallback progress_call);

// 操作函数
int UartManager_Connect(UartManager* mgr, const char* portName, DWORD baudRate);
void UartManager_Disconnect(UartManager* mgr);
uint32_t UartManager_GetFirmwareVersion(UartManager* mgr);
int UartManager_BoardReboot(UartManager* mgr);
int UartManager_FirmwareUpgrade(UartManager* mgr, const wchar_t* fileName, int testMode);
int UartManager_EnumPorts(UartManager* mgr, SerialPortInfo* ports, int maxCount);

#ifdef __cplusplus
}
#endif

#endif // UART_MANAGER_H
