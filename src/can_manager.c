#include "can_manager.h"
#include "can_dispatcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CanManager {
    CRITICAL_SECTION criticalSection;
    CanHal          *hal;
    CanDispatcher   *disp;
    int              channel;
    msgCallback      msgCallbackFn;
    void            *msgCallbackCtx;
    progressCallback progressCallbackFn;
    void            *progressCallbackCtx;
    int              initialized;
    int              virtualMode;
};

CanManager* CanManager_Create(CanHal *hal, CanDispatcher *disp) {
    CanManager* mgr = (CanManager*)malloc(sizeof(CanManager));
    if (!mgr) return NULL;

    InitializeCriticalSection(&mgr->criticalSection);
    mgr->hal                = hal;
    mgr->disp               = disp;
    mgr->channel            = CAN_HAL_INVALID_HANDLE;
    mgr->msgCallbackFn      = NULL;
    mgr->msgCallbackCtx     = NULL;
    mgr->progressCallbackFn = NULL;
    mgr->progressCallbackCtx = NULL;
    mgr->initialized        = 1;
    mgr->virtualMode        = 0;

    return mgr;
}

void CanManager_Destroy(CanManager* mgr) {
    if (!mgr) return;
    if (mgr->initialized) {
        DeleteCriticalSection(&mgr->criticalSection);
    }
    free(mgr);
}

void CanManager_SetCallback(CanManager* mgr, msgCallback msg_call, void *ctx) {
    if (mgr) {
        mgr->msgCallbackFn  = msg_call;
        mgr->msgCallbackCtx = ctx;
    }
}

void CanManager_SetProgressCallback(CanManager* mgr, progressCallback progress_call, void *ctx) {
    if (mgr) {
        mgr->progressCallbackFn  = progress_call;
        mgr->progressCallbackCtx = ctx;
    }
}

static void appendLog(CanManager* mgr, const char* msg) {
    if (mgr && mgr->msgCallbackFn) {
        mgr->msgCallbackFn(msg, mgr->msgCallbackCtx);
    }
}

static void reportProgress(CanManager* mgr, int percent) {
    if (mgr && mgr->progressCallbackFn) {
        mgr->progressCallbackFn(percent, mgr->progressCallbackCtx);
    }
}

int CanManager_IsVirtualChannel(int channel) {
    return channel == CAN_HAL_VIRTUAL_CHANNEL;
}

CanHal *CanManager_GetHal(CanManager* mgr) {
    return mgr ? mgr->hal : NULL;
}

CanDispatcher *CanManager_GetDisp(CanManager* mgr) {
    return mgr ? mgr->disp : NULL;
}

void CanManager_ReplaceHal(CanManager *mgr, CanHal *newHal, CanDispatcher *newDisp)
{
    if (!mgr) return;
    EnterCriticalSection(&mgr->criticalSection);
    mgr->hal  = newHal;
    mgr->disp = newDisp;
    LeaveCriticalSection(&mgr->criticalSection);
}

int CanManager_Connect(CanManager* mgr, int channel, int baud_index) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->channel != CAN_HAL_INVALID_HANDLE) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN 连接已存在, 请勿重复连接");
        return 1;
    }

    if (channel == CAN_HAL_VIRTUAL_CHANNEL) {
        mgr->channel = channel;
        mgr->virtualMode = 1;
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "虚拟 CAN 连接成功 (测试模式)");
        return 1;
    }

    if (!mgr->hal) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN HAL 未初始化");
        return 0;
    }

    int result = CanHal_Connect(mgr->hal, channel, baud_index);
    if (result) {
        mgr->channel = channel;
        mgr->virtualMode = 0;
        LeaveCriticalSection(&mgr->criticalSection);

        char logMsg[64];
        sprintf(logMsg, "CAN(id=%xh) 连接成功 (%s)", channel, CanHal_GetName(mgr->hal));
        appendLog(mgr, logMsg);

        /* Start dispatcher read thread – accepts ALL frames */
        if (mgr->disp)
            CanDisp_Start(mgr->disp);

        return 1;
    } else {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN 初始化失败");
        return 0;
    }
}

void CanManager_Disconnect(CanManager* mgr) {
    if (!mgr || !mgr->initialized) return;

    EnterCriticalSection(&mgr->criticalSection);

    /* Stop dispatcher first */
    if (mgr->disp)
        CanDisp_Stop(mgr->disp);

    char logMsg[64];
    if (mgr->virtualMode) {
        sprintf(logMsg, "虚拟 CAN 连接已断开");
    } else {
        sprintf(logMsg, "CAN(id=%xh) 连接已断开", mgr->channel);
        if (mgr->hal)
            CanHal_Disconnect(mgr->hal);
    }
    mgr->channel = CAN_HAL_INVALID_HANDLE;
    mgr->virtualMode = 0;
    LeaveCriticalSection(&mgr->criticalSection);
    appendLog(mgr, logMsg);
}

/* Wait for a specific CAN response frame via dispatcher */
static int CAN_WaitForResponse(CanManager* mgr, uint32_t* code, uint32_t* param, int timeoutMs) {
    CanHalFrame frame;

    if (!mgr->disp) return 0;

    if (!CanDisp_WaitFrame(mgr->disp, PLATFORM_TX, &frame, timeoutMs))
        return 0;

    can_frame_t* f = (can_frame_t*)frame.data;
    *code = f->code;
    *param = f->val;
    return 1;
}

uint32_t CanManager_GetFirmwareVersion(CanManager* mgr) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->channel == CAN_HAL_INVALID_HANDLE) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN已断开连接, 请重新连接");
        return 0;
    }

    if (mgr->virtualMode) {
        appendLog(mgr, "固件版本: v1.0.0 (虚拟 CAN)");
        LeaveCriticalSection(&mgr->criticalSection);
        return 0x01000000;
    }

    CanHalFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.id = PLATFORM_RX;
    frame.dlc = 8;
    frame.flags = CAN_HAL_FLAG_STANDARD;
    can_frame_t* f = (can_frame_t*)frame.data;
    f->code = BOARD_VERSION;

    if (!CanHal_Write(mgr->hal, &frame)) {
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

int CanManager_BoardReboot(CanManager* mgr) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->channel == CAN_HAL_INVALID_HANDLE) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN已断开连接, 请重新连接");
        return 0;
    }

    if (mgr->virtualMode) {
        appendLog(mgr, "虚拟板卡重启成功");
        LeaveCriticalSection(&mgr->criticalSection);
        return 1;
    }

    CanHalFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.id = PLATFORM_RX;
    frame.dlc = 8;
    frame.flags = CAN_HAL_FLAG_STANDARD;
    can_frame_t* f = (can_frame_t*)frame.data;
    f->code = BOARD_REBOOT;

    int result = CanHal_Write(mgr->hal, &frame);
    LeaveCriticalSection(&mgr->criticalSection);

    if (!result) {
        appendLog(mgr, "CAN 发送失败");
        return 0;
    }

    return 1;
}

static int VirtualCAN_FirmwareUpgrade(CanManager* mgr, const wchar_t* fileName) {
    char logMsg[256];
    appendLog(mgr, "虚拟 CAN 模式：模拟固件升级...");

    HANDLE hSrcFile = CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSrcFile == INVALID_HANDLE_VALUE) {
        appendLog(mgr, "无法打开源固件文件");
        return 0;
    }

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
    sprintf(logMsg, "开始固件升级, 固件大小: %lu 字节", fileSize);
    appendLog(mgr, logMsg);
    appendLog(mgr, "输出文件: virtual_firmware.bin");

    Sleep(500);
    appendLog(mgr, "Flash 擦除完成");

    BYTE buffer[4096];
    DWORD bytesRead, bytesWritten;
    DWORD totalBytes = 0;

    while (ReadFile(hSrcFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        WriteFile(hDstFile, buffer, bytesRead, &bytesWritten, NULL);
        totalBytes += bytesRead;

        if (totalBytes % 64 == 0 || totalBytes == fileSize) {
            reportProgress(mgr, (int)(totalBytes * 100 / fileSize));
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

static int HAL_FirmwareUpgrade(CanManager* mgr, const wchar_t* fileName, int testMode) {
    char logMsg[256];

    HANDLE hFile = CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        WideCharToMultiByte(CP_UTF8, 0, fileName, -1, logMsg + 20, 200, NULL, NULL);
        sprintf(logMsg, "无法打开文件: %s", logMsg + 20);
        appendLog(mgr, logMsg);
        return 0;
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    sprintf(logMsg, "开始固件升级, 固件大小: %lu 字节", fileSize);
    appendLog(mgr, logMsg);

    CanHalFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.id = PLATFORM_RX;
    frame.dlc = 8;
    frame.flags = CAN_HAL_FLAG_STANDARD;

    can_frame_t* f = (can_frame_t*)frame.data;
    f->code = BOARD_START_UPDATE;
    f->val = fileSize;

    if (!CanHal_Write(mgr->hal, &frame)) {
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

    frame.id = FW_DATA_RX;
    while (ReadFile(hFile, frame.data, 8, &bytesRead, NULL) && bytesRead > 0) {
        frame.dlc = (uint8_t)bytesRead;
        if (!CanHal_Write(mgr->hal, &frame)) {
            CloseHandle(hFile);
            appendLog(mgr, "发送文件数据失败");
            return 0;
        }

        bytesSent += bytesRead;

        if (bytesSent % 64 == 0 || bytesSent == fileSize) {
            reportProgress(mgr, (int)(bytesSent * 100 / fileSize));

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

    reportProgress(mgr, 100);

    frame.id = PLATFORM_RX;
    frame.dlc = 8;
    frame.flags = CAN_HAL_FLAG_STANDARD;
    memset(frame.data, 0, 8);
    f = (can_frame_t*)frame.data;
    f->code = BOARD_CONFIRM;
    f->val = testMode ? 0 : 1;

    if (!CanHal_Write(mgr->hal, &frame)) {
        appendLog(mgr, "固件发送确认失败!");
        return 0;
    }

    if (!CAN_WaitForResponse(mgr, &code, &offset, 30000)) {
        appendLog(mgr, "固件确认超时!");
        return 0;
    }

    if (code == FW_CODE_CONFIRM && offset == 0x55AA55AA) {
        WideCharToMultiByte(CP_UTF8, 0, fileName, -1, logMsg + 20, 200, NULL, NULL);
        sprintf(logMsg, "文件 %s 上传完成. 点击重启，板卡将在45-60秒内完成重启", logMsg + 20);
        appendLog(mgr, logMsg);
        return 1;
    } else if (code == FW_CODE_TRANFER_ERROR) {
        appendLog(mgr, "固件更新失败");
    }

    return 0;
}

int CanManager_FirmwareUpgrade(CanManager* mgr, const wchar_t* fileName, int testMode) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->channel == CAN_HAL_INVALID_HANDLE) {
        LeaveCriticalSection(&mgr->criticalSection);
        appendLog(mgr, "CAN已断开连接, 请重新连接");
        return 0;
    }

    LeaveCriticalSection(&mgr->criticalSection);

    if (mgr->virtualMode) {
        return VirtualCAN_FirmwareUpgrade(mgr, fileName);
    } else {
        return HAL_FirmwareUpgrade(mgr, fileName, testMode);
    }
}

int CanManager_DetectDevice(CanManager* mgr, int* channels, int maxCount) {
    if (!mgr || !mgr->hal) return 0;
    CanHal_SetLogCallback(mgr->hal, NULL);  /* suppress duplicate log via HAL */
    return CanHal_DetectDevices(mgr->hal, channels, maxCount);
}
