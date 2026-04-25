#include "can_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 内部结构定义
struct CanManager {
    CRITICAL_SECTION criticalSection;
    TPCANHandle channel;
    msgCallback msgCallback;
    progressCallback progressCallback;
    int initialized;
};

// 创建 CAN 管理器
CanManager* CanManager_Create(void) {
    CanManager* mgr = (CanManager*)malloc(sizeof(CanManager));
    if (!mgr) return NULL;

    InitializeCriticalSection(&mgr->criticalSection);
    mgr->channel = PCAN_NONEBUS;
    mgr->msgCallback = NULL;
    mgr->progressCallback = NULL;
    mgr->initialized = 1;

    return mgr;
}

// 销毁 CAN 管理器
void CanManager_Destroy(CanManager* mgr) {
    if (!mgr) return;
    if (mgr->initialized) {
        DeleteCriticalSection(&mgr->criticalSection);
    }
    free(mgr);
}

// 设置日志回调函数
void CanManager_SetCallback(CanManager* mgr, msgCallback msg_call) {
    if (mgr) {
        mgr->msgCallback = msg_call;
    }
}

// 设置进度回调函数
void CanManager_SetProgressCallback(CanManager* mgr, progressCallback progress_call) {
    if (mgr) {
        mgr->progressCallback = progress_call;
    }
}

// 添加日志
static void appendLog(CanManager* mgr, const char* msg) {
    if (mgr && mgr->msgCallback) {
        mgr->msgCallback(msg);
    }
}

// 连接 CAN 设备
int CanManager_Connect(CanManager* mgr, TPCANHandle channel, TPCANBaudrate baudrate) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->channel != PCAN_NONEBUS) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN 连接已存在, 请勿重复连接");
        return 1;
    }

    // 检查是否为虚拟 CAN
    if (channel == VIRTUAL_CAN_CHANNEL) {
        mgr->channel = channel;
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "虚拟 CAN 连接成功 (测试模式)");
        return 1;
    }

    TPCANStatus status = CAN_Initialize(channel, baudrate, 0, 0, 0);
    if (status != PCAN_ERROR_OK) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN 初始化失败");
        return 0;
    } else {
        mgr->channel = channel;
        LeaveCriticalSection(&mgr->criticalSection);
        char logMsg[32];
        sprintf(logMsg, "CAN(id=%xh) 连接成功", channel);
        appendLog(mgr, logMsg);
        CAN_FilterMessages(mgr->channel, PLATFORM_TX, PLATFORM_TX, PCAN_MODE_STANDARD);
        return 1;
    }
}

// 断开 CAN 连接
void CanManager_Disconnect(CanManager* mgr) {
    if (!mgr || !mgr->initialized) return;

    EnterCriticalSection(&mgr->criticalSection);

    char logMsg[64];
    if (mgr->channel == VIRTUAL_CAN_CHANNEL) {
        sprintf(logMsg, "虚拟 CAN 连接已断开");
    } else {
        sprintf(logMsg, "CAN(id=%xh) 连接已断开", mgr->channel);
        CAN_Uninitialize(mgr->channel);
    }
    mgr->channel = PCAN_NONEBUS;
    LeaveCriticalSection(&mgr->criticalSection);
    appendLog(mgr, logMsg);
}

// 等待 CAN 响应
static int CAN_WaitForResponse(CanManager* mgr, uint32_t* code, uint32_t* param, int timeoutMs) {
    TPCANMsg msg;
    TPCANTimestamp timestamp;
    DWORD startTime = GetTickCount();

    while ((int)(GetTickCount() - startTime) < timeoutMs) {
        TPCANStatus status = CAN_Read(mgr->channel, &msg, &timestamp);
        if (status == PCAN_ERROR_OK && msg.ID == PLATFORM_TX) {
            can_frame_t* frame = (can_frame_t*)msg.DATA;
            *code = frame->code;
            *param = frame->val;
            return 1;
        } else if (status == PCAN_ERROR_QRCVEMPTY) {
            Sleep(1);
        }
    }
    return 0;
}

// 获取固件版本
uint32_t CanManager_GetFirmwareVersion(CanManager* mgr) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->channel == PCAN_NONEBUS) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN已断开连接, 请重新连接");
        return 0;
    }

    // 虚拟 CAN 模式：返回模拟版本
    if (mgr->channel == VIRTUAL_CAN_CHANNEL) {
        appendLog(mgr, "固件版本: v1.0.0 (虚拟 CAN)");
        LeaveCriticalSection(&mgr->criticalSection);
        return 0x01000000;  // v1.0.0
    }

    TPCANMsg msg;
    msg.ID = PLATFORM_RX;
    msg.MSGTYPE = PCAN_MODE_STANDARD;
    msg.LEN = 8;
    memset(msg.DATA, 0, 8);

    can_frame_t* frame = (can_frame_t*)msg.DATA;
    frame->code = BOARD_VERSION;

    TPCANStatus status = CAN_Write(mgr->channel, &msg);
    if (status != PCAN_ERROR_OK) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN 发送失败");
        return 0;
    }

    uint32_t code, version;
    if (!CAN_WaitForResponse(mgr, &code, &version, 5000)) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN 读取失败，超时！！");
        return 0;
    }

    if (code == FW_CODE_VERSION) {
        char buf[64];
        sprintf(buf, "固件版本: v%u.%u.%u", (version >> 24) & 0xFF,
                (version >> 16) & 0xFF, (version >> 8) & 0xFF);
        appendLog(mgr, buf);
        LeaveCriticalSection(&mgr->criticalSection);
        return version;
    }

    LeaveCriticalSection(&mgr->criticalSection);
    appendLog(mgr, "CAN 读取失败，数据错误！！");
    return 0;
}

// 重启板卡
int CanManager_BoardReboot(CanManager* mgr) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->channel == PCAN_NONEBUS) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN已断开连接, 请重新连接");
        return 0;
    }

    // 虚拟 CAN 模式：模拟重启
    if (mgr->channel == VIRTUAL_CAN_CHANNEL) {
        appendLog(mgr, "虚拟板卡重启成功");
        LeaveCriticalSection(&mgr->criticalSection);
        return 1;
    }

    TPCANMsg msg;
    msg.ID = PLATFORM_RX;
    msg.MSGTYPE = PCAN_MODE_STANDARD;
    msg.LEN = 8;
    memset(msg.DATA, 0, 8);

    can_frame_t* frame = (can_frame_t*)msg.DATA;
    frame->code = BOARD_REBOOT;

    TPCANStatus status = CAN_Write(mgr->channel, &msg);
    LeaveCriticalSection(&mgr->criticalSection);

    if (status != PCAN_ERROR_OK) {
        appendLog(mgr, "CAN 发送失败");
        return 0;
    }

    return 1;
}

// 虚拟 CAN 固件升级
static int VirtualCAN_FirmwareUpgrade(CanManager* mgr, const wchar_t* fileName) {
    char logMsg[256];
    appendLog(mgr, "虚拟 CAN 模式：模拟固件升级...");

    // 打开源文件
    HANDLE hSrcFile = CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSrcFile == INVALID_HANDLE_VALUE) {
        appendLog(mgr, "无法打开源固件文件");
        return 0;
    }

    // 创建输出文件
    char outputFileName[256];
    GetModuleFileNameA(NULL, outputFileName, 256);
    char* lastSlash = strrchr(outputFileName, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    }
    strcat(outputFileName, "virtual_firmware.bin");

    HANDLE hDstFile = CreateFileA(outputFileName, GENERIC_WRITE, 0, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDstFile == INVALID_HANDLE_VALUE) {
        CloseHandle(hSrcFile);
        appendLog(mgr, "无法创建输出文件");
        return 0;
    }

    DWORD fileSize = GetFileSize(hSrcFile, NULL);
    sprintf(logMsg, "开始固件升级, 固件大小: %u 字节", fileSize);
    appendLog(mgr, logMsg);
    appendLog(mgr, "输出文件: virtual_firmware.bin");

    // 模拟 Flash 擦除
    Sleep(500);
    appendLog(mgr, "Flash 擦除完成");

    // 复制文件并模拟进度
    BYTE buffer[4096];
    DWORD bytesRead, bytesWritten;
    DWORD totalBytes = 0;

    while (ReadFile(hSrcFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        WriteFile(hDstFile, buffer, bytesRead, &bytesWritten, NULL);
        totalBytes += bytesRead;

        // 每 64 字节更新一次进度
        if (totalBytes % 64 == 0 || totalBytes == fileSize) {
            if (mgr->progressCallback) {
                int percent = (int)(totalBytes * 100 / fileSize);
                mgr->progressCallback(percent);
            }
            // 模拟一些延迟
            if (totalBytes % 1024 == 0) {
                Sleep(10);
            }
        }
    }

    CloseHandle(hSrcFile);
    CloseHandle(hDstFile);

    Sleep(200);
    appendLog(mgr, "固件发送完成");
    Sleep(200);
    appendLog(mgr, "固件确认完成");

    sprintf(logMsg, "虚拟固件已保存到: %s", outputFileName);
    appendLog(mgr, logMsg);

    return 1;
}

// PCAN 固件升级
static int PCAN_FirmwareUpgrade(CanManager* mgr, const wchar_t* fileName, int testMode) {
    char logMsg[256];

    HANDLE hFile = CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        // 将宽字符文件名转换为 UTF-8 用于日志输出
        WideCharToMultiByte(CP_UTF8, 0, fileName, -1, logMsg + 20, 200, NULL, NULL);
        sprintf(logMsg, "无法打开文件: %s", logMsg + 20);
        appendLog(mgr, logMsg);
        return 0;
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    sprintf(logMsg, "开始固件升级, 固件大小: %u 字节", fileSize);
    appendLog(mgr, logMsg);

    TPCANMsg msg;
    msg.ID = PLATFORM_RX;
    msg.MSGTYPE = PCAN_MODE_STANDARD;
    msg.LEN = 8;
    memset(msg.DATA, 0, 8);

    can_frame_t* frame = (can_frame_t*)msg.DATA;
    frame->code = BOARD_START_UPDATE;
    frame->val = fileSize;

    TPCANStatus status = CAN_Write(mgr->channel, &msg);
    if (status != PCAN_ERROR_OK) {
        CloseHandle(hFile);
        appendLog(mgr, "发送固件大小失败");
        return 0;
    }

    uint32_t code, offset;
    if (!CAN_WaitForResponse(mgr, &code, &offset, 15000)) {
        CloseHandle(hFile);
        appendLog(mgr, "Flash擦除超时");
        return 0;
    }

    if (code != FW_CODE_OFFSET || offset != 0) {
        CloseHandle(hFile);
        sprintf(logMsg, "Flash擦除失败: code(%u), offset(%u)", code, offset);
        appendLog(mgr, logMsg);
        return 0;
    }

    DWORD bytesSent = 0;
    DWORD bytesRead;

    msg.ID = FW_DATA_RX;
    while (ReadFile(hFile, msg.DATA, 8, &bytesRead, NULL) && bytesRead > 0) {
        msg.LEN = bytesRead;
        if (CAN_Write(mgr->channel, &msg) != PCAN_ERROR_OK) {
            CloseHandle(hFile);
            appendLog(mgr, "发送文件数据失败");
            return 0;
        }

        bytesSent += bytesRead;

        if (bytesSent % 64 == 0 || bytesSent == fileSize) {
            // 更新进度
            if (mgr->progressCallback) {
                int percent = (int)(bytesSent * 100 / fileSize);
                mgr->progressCallback(percent);
            }

            if (!CAN_WaitForResponse(mgr, &code, &offset, 5000)) {
                CloseHandle(hFile);
                appendLog(mgr, "固件更新超时!");
                return 0;
            }

            if (code == FW_CODE_UPDATE_SUCCESS && offset == bytesSent) break;
            if (code != FW_CODE_OFFSET) {
                CloseHandle(hFile);
                sprintf(logMsg, "固件升级失败: code(%u), offset(%u)", code, offset);
                appendLog(mgr, logMsg);
                return 0;
            }
        }
    }

    CloseHandle(hFile);

    // 设置进度为 100%
    if (mgr->progressCallback) {
        mgr->progressCallback(100);
    }

    msg.ID = PLATFORM_RX;
    msg.LEN = 8;
    memset(msg.DATA, 0, 8);
    frame = (can_frame_t*)msg.DATA;
    frame->code = BOARD_CONFIRM;
    frame->val = testMode ? 0 : 1;

    if (CAN_Write(mgr->channel, &msg) != PCAN_ERROR_OK) {
        appendLog(mgr, "固件发送确认失败!");
        return 0;
    }

    if (!CAN_WaitForResponse(mgr, &code, &offset, 30000)) {
        appendLog(mgr, "固件确认超时!");
        return 0;
    }

    if (code == FW_CODE_CONFIRM && offset == 0x55AA55AA) {
        // 将宽字符文件名转换为 UTF-8 用于日志输出
        WideCharToMultiByte(CP_UTF8, 0, fileName, -1, logMsg + 20, 200, NULL, NULL);
        sprintf(logMsg, "文件 %s 上传完成. 点击重启，板卡将在45-60秒内完成重启", logMsg + 20);
        appendLog(mgr, logMsg);
        return 1;
    } else if (code == FW_CODE_TRANFER_ERROR) {
        appendLog(mgr, "固件更新失败");
    }

    return 0;
}

// 固件升级（主调度函数）
int CanManager_FirmwareUpgrade(CanManager* mgr, const wchar_t* fileName, int testMode) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->channel == PCAN_NONEBUS) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN已断开连接, 请重新连接");
        return 0;
    }

    LeaveCriticalSection(&mgr->criticalSection);

    // 根据通道类型选择升级方式
    if (mgr->channel == VIRTUAL_CAN_CHANNEL) {
        return VirtualCAN_FirmwareUpgrade(mgr, fileName);
    } else {
        return PCAN_FirmwareUpgrade(mgr, fileName, testMode);
    }
}

// 检测设备
int CanManager_DetectDevice(CanManager* mgr, TPCANHandle* channels, int maxCount) {
    int count = 0;
    char msg[64];
    for (int i = 0; i < 16 && count < maxCount; i++) {
        TPCANHandle channel = PCAN_NONEBUS;
        sprintf(msg, "devicetype=pcan_usb,controllernumber=%d", i);
        TPCANStatus result = CAN_LookUpChannel(msg, &channel);
        if (result == PCAN_ERROR_OK && channel != PCAN_NONEBUS) {
            channels[count++] = channel;
        }
    }
    sprintf(msg, "查询到 %d 个可用CAN设备", count);
    appendLog(mgr, msg);
    return count;
}
