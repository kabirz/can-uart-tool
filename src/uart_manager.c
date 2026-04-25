#include "uart_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#include <devpkey.h>

// 需要链接 setupapi.lib
#pragma comment(lib, "setupapi.lib")

// CRC16-CCITT 计算函数
static uint16_t CalcCRC16(const uint8_t* data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// 内部结构定义
struct UartManager {
    CRITICAL_SECTION criticalSection;
    HANDLE hSerial;
    char portName[MAX_PORT_NAME_LEN];
    DWORD baudRate;
    UartMsgCallback msgCallback;
    UartProgressCallback progressCallback;
    int initialized;
};

// 创建 UART 管理器
UartManager* UartManager_Create(void) {
    UartManager* mgr = (UartManager*)malloc(sizeof(UartManager));
    if (!mgr) return NULL;

    InitializeCriticalSection(&mgr->criticalSection);
    mgr->hSerial = INVALID_HANDLE_VALUE;
    mgr->portName[0] = '\0';
    mgr->baudRate = 115200;
    mgr->msgCallback = NULL;
    mgr->progressCallback = NULL;
    mgr->initialized = 1;

    return mgr;
}

// 销毁 UART 管理器
void UartManager_Destroy(UartManager* mgr) {
    if (!mgr) return;
    if (mgr->initialized) {
        DeleteCriticalSection(&mgr->criticalSection);
    }
    free(mgr);
}

// 设置日志回调函数
void UartManager_SetCallback(UartManager* mgr, UartMsgCallback msg_call) {
    if (mgr) {
        mgr->msgCallback = msg_call;
    }
}

// 设置进度回调函数
void UartManager_SetProgressCallback(UartManager* mgr, UartProgressCallback progress_call) {
    if (mgr) {
        mgr->progressCallback = progress_call;
    }
}

// 添加日志
static void AppendLog(UartManager* mgr, const char* msg) {
    if (mgr && mgr->msgCallback) {
        mgr->msgCallback(msg);
    }
}

// 构建 UART 帧
static int BuildUARTFrame(uint8_t type, const uint8_t* data, int dataLen, uint8_t* outFrame, int maxLen) {
    if (dataLen > 8) return 0;  // 数据长度限制

    int idx = 0;
    outFrame[idx++] = FRAME_HEAD;
    outFrame[idx++] = type;
    // 大端序长度
    outFrame[idx++] = (dataLen >> 8) & 0xFF;
    outFrame[idx++] = dataLen & 0xFF;
    // 数据
    if (dataLen > 0) {
        memcpy(&outFrame[idx], data, dataLen);
        idx += dataLen;
    }
    // CRC16（大端序）
    uint16_t crc = CalcCRC16(data, dataLen);
    outFrame[idx++] = (crc >> 8) & 0xFF;
    outFrame[idx++] = crc & 0xFF;
    outFrame[idx++] = FRAME_TAIL;

    return idx;
}

// 解析 UART 帧
static int ParseUARTFrame(const uint8_t* buffer, int bufLen, uint8_t* outType, uint8_t* outData, int* outDataLen) {
    if (bufLen < 7) return 0;  // 最小帧: HEAD(1) + TYPE(1) + LEN(2) + CRC(2) + TAIL(1)

    int idx = 0;

    // 查找帧头
    while (idx < bufLen && buffer[idx] != FRAME_HEAD) {
        idx++;
    }
    if (idx >= bufLen - 6) return 0;  // 没有足够的字节

    int frameStart = idx;
    idx++;  // 跳过 HEAD

    uint8_t frameType = buffer[idx++];
    uint16_t dataLen = ((uint16_t)buffer[idx] << 8) | buffer[idx + 1];
    idx += 2;

    if (dataLen > 8) {
        return -(frameStart + 1);  // 无效数据长度，返回丢弃位置
    }

    // 检查是否有足够的数据
    int totalLen = 1 + 1 + 2 + dataLen + 2 + 1;  // HEAD + TYPE + LEN + DATA + CRC + TAIL
    if (frameStart + totalLen > bufLen) {
        return 0;  // 帧不完整
    }

    // 提取数据
    if (dataLen > 0 && outData) {
        memcpy(outData, &buffer[idx], dataLen);
    }
    idx += dataLen;
    if (outDataLen) *outDataLen = dataLen;
    if (outType) *outType = frameType;

    // 校验 CRC
    uint16_t recvCRC = ((uint16_t)buffer[idx] << 8) | buffer[idx + 1];
    idx += 2;
    uint16_t calcCRC = CalcCRC16(outData, dataLen);

    if (recvCRC != calcCRC) {
        return -(frameStart + 1);  // CRC 错误
    }

    // 检查帧尾
    if (buffer[idx] != FRAME_TAIL) {
        return -(frameStart + 1);  // 帧尾错误
    }

    return totalLen;  // 返回帧长度
}

// 串口写入
static int UartWrite(UartManager* mgr, const uint8_t* data, int len) {
    DWORD bytesWritten;
    if (!WriteFile(mgr->hSerial, data, len, &bytesWritten, NULL) || bytesWritten != (DWORD)len) {
        return 0;
    }
    return 1;
}

// 串口读取（带超时）
static int UartRead(UartManager* mgr, uint8_t* buffer, int maxLen, int timeoutMs, int* outRead) {
    DWORD startTime = GetTickCount();
    int totalRead = 0;

    while (totalRead < maxLen) {
        DWORD elapsed = GetTickCount() - startTime;
        if (elapsed >= (DWORD)timeoutMs) {
            *outRead = totalRead;
            return 0;  // 超时
        }

        DWORD bytesRead;
        if (ReadFile(mgr->hSerial, &buffer[totalRead], maxLen - totalRead, &bytesRead, NULL)) {
            if (bytesRead > 0) {
                totalRead += bytesRead;
                // 如果已经读取到最小帧长度，尝试解析
                if (totalRead >= 7) {
                    *outRead = totalRead;
                    return 1;
                }
            }
        } else {
            // 读取错误
            *outRead = totalRead;
            return 0;
        }

        Sleep(1);
    }

    *outRead = totalRead;
    return 1;
}

// 等待 UART 响应（带帧解析）
static int UartWaitForResponse(UartManager* mgr, uint32_t* code, uint32_t* param, int timeoutMs) {
    uint8_t buffer[256];
    int bufPos = 0;
    DWORD startTime = GetTickCount();

    while ((int)(GetTickCount() - startTime) < timeoutMs) {
        DWORD bytesRead;
        if (ReadFile(mgr->hSerial, &buffer[bufPos], sizeof(buffer) - bufPos, &bytesRead, NULL)) {
            if (bytesRead > 0) {
                bufPos += bytesRead;

                // 尝试解析帧
                uint8_t frameType;
                uint8_t data[8];
                int dataLen;

                int result = ParseUARTFrame(buffer, bufPos, &frameType, data, &dataLen);
                if (result > 0) {
                    // 成功解析帧
                    if (dataLen == 8) {
                        // 解析参数（小端序）
                        *code = ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) |
                                ((uint32_t)data[1] << 8) | data[0];
                        *param = ((uint32_t)data[7] << 24) | ((uint32_t)data[6] << 16) |
                                 ((uint32_t)data[5] << 8) | data[4];
                        return 1;
                    }
                    // 移除已解析的帧
                    memmove(buffer, &buffer[result], bufPos - result);
                    bufPos -= result;
                } else if (result < 0) {
                    // 丢弃无效数据
                    int discard = -result;
                    memmove(buffer, &buffer[discard], bufPos - discard);
                    bufPos -= discard;
                }
            }
        }
    }

    return 0;  // 超时
}

// 连接串口设备
int UartManager_Connect(UartManager* mgr, const char* portName, DWORD baudRate) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->hSerial != INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "串口已连接, 请勿重复连接");
        return 1;
    }

    // 构建设备名称
    char deviceName[32];
    if (strncmp(portName, "COM", 3) == 0 && strlen(portName) < 8) {
        sprintf(deviceName, "\\\\.\\%s", portName);
    } else {
        strncpy(deviceName, portName, sizeof(deviceName) - 1);
        deviceName[sizeof(deviceName) - 1] = '\0';
    }

    // 打开串口
    mgr->hSerial = CreateFileA(deviceName, GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);

    if (mgr->hSerial == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&mgr->criticalSection);
        char logMsg[64];
        sprintf(logMsg, "无法打开串口 %s", portName);
        AppendLog(mgr, logMsg);
        return 0;
    }

    // 配置串口参数
    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(mgr->hSerial, &dcb)) {
        CloseHandle(mgr->hSerial);
        mgr->hSerial = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "获取串口状态失败");
        return 0;
    }

    dcb.BaudRate = baudRate;
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

    if (!SetCommState(mgr->hSerial, &dcb)) {
        CloseHandle(mgr->hSerial);
        mgr->hSerial = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "配置串口参数失败");
        return 0;
    }

    // 设置超时 - 非阻塞模式
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;  // 禁用间隔超时
    timeouts.ReadTotalTimeoutMultiplier = 0;  // 不使用乘数超时
    timeouts.ReadTotalTimeoutConstant = 0;    // 立即返回（非阻塞）
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 5000;

    if (!SetCommTimeouts(mgr->hSerial, &timeouts)) {
        CloseHandle(mgr->hSerial);
        mgr->hSerial = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "设置串口超时失败");
        return 0;
    }

    // 清空缓冲区
    PurgeComm(mgr->hSerial, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);

    strncpy(mgr->portName, portName, MAX_PORT_NAME_LEN - 1);
    mgr->portName[MAX_PORT_NAME_LEN - 1] = '\0';
    mgr->baudRate = baudRate;

    char logMsg[64];
    sprintf(logMsg, "串口 %s 连接成功 (%u bps)", portName, baudRate);
    AppendLog(mgr, logMsg);

    LeaveCriticalSection(&mgr->criticalSection);
    return 1;
}

// 断开串口连接
void UartManager_Disconnect(UartManager* mgr) {
    if (!mgr || !mgr->initialized) return;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(mgr->hSerial);
        mgr->hSerial = INVALID_HANDLE_VALUE;

        char logMsg[64];
        sprintf(logMsg, "串口 %s 连接已断开", mgr->portName);
        AppendLog(mgr, logMsg);
    }

    LeaveCriticalSection(&mgr->criticalSection);
}

// 获取固件版本
uint32_t UartManager_GetFirmwareVersion(UartManager* mgr) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->hSerial == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "串口未连接");
        return 0;
    }

    // 清空接收缓冲区
    PurgeComm(mgr->hSerial, PURGE_RXCLEAR);

    // 构建命令帧
    uint8_t cmdData[8];
    cmdData[0] = BOARD_VERSION & 0xFF;
    cmdData[1] = (BOARD_VERSION >> 8) & 0xFF;
    cmdData[2] = (BOARD_VERSION >> 16) & 0xFF;
    cmdData[3] = (BOARD_VERSION >> 24) & 0xFF;
    cmdData[4] = 0;
    cmdData[5] = 0;
    cmdData[6] = 0;
    cmdData[7] = 0;

    uint8_t frame[32];
    int frameLen = BuildUARTFrame(FRAME_TYPE_CMD, cmdData, 8, frame, sizeof(frame));

    FlushFileBuffers(mgr->hSerial);

    if (!UartWrite(mgr, frame, frameLen)) {
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "发送版本查询命令失败");
        return 0;
    }

    AppendLog(mgr, "等待版本响应...");

    uint32_t code, version;
    if (!UartWaitForResponse(mgr, &code, &version, 5000)) {
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "读取版本响应超时");
        return 0;
    }

    if (code == FW_CODE_VERSION) {
        char buf[64];
        sprintf(buf, "固件版本: v%u.%u.%u", (version >> 24) & 0xFF,
                (version >> 16) & 0xFF, (version >> 8) & 0xFF);
        AppendLog(mgr, buf);
        LeaveCriticalSection(&mgr->criticalSection);
        return version;
    }

    LeaveCriticalSection(&mgr->criticalSection);
    AppendLog(mgr, "读取版本响应数据错误");
    return 0;
}

// 重启板卡
int UartManager_BoardReboot(UartManager* mgr) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->hSerial == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "串口未连接");
        return 0;
    }

    // 构建命令帧
    uint8_t cmdData[8];
    cmdData[0] = BOARD_REBOOT & 0xFF;
    cmdData[1] = (BOARD_REBOOT >> 8) & 0xFF;
    cmdData[2] = (BOARD_REBOOT >> 16) & 0xFF;
    cmdData[3] = (BOARD_REBOOT >> 24) & 0xFF;
    cmdData[4] = 0;
    cmdData[5] = 0;
    cmdData[6] = 0;
    cmdData[7] = 0;

    uint8_t frame[32];
    int frameLen = BuildUARTFrame(FRAME_TYPE_CMD, cmdData, 8, frame, sizeof(frame));

    int result = UartWrite(mgr, frame, frameLen);
    LeaveCriticalSection(&mgr->criticalSection);

    if (!result) {
        AppendLog(mgr, "发送重启命令失败");
        return 0;
    }

    AppendLog(mgr, "重启命令已发送");
    return 1;
}

// 固件升级
int UartManager_FirmwareUpgrade(UartManager* mgr, const wchar_t* fileName, int testMode) {
    if (!mgr || !mgr->initialized) return 0;

    EnterCriticalSection(&mgr->criticalSection);

    if (mgr->hSerial == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "串口未连接");
        return 0;
    }

    char logMsg[256];

    HANDLE hFile = CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        WideCharToMultiByte(CP_UTF8, 0, fileName, -1, logMsg + 20, 200, NULL, NULL);
        sprintf(logMsg, "无法打开文件: %s", logMsg + 20);
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, logMsg);
        return 0;
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    sprintf(logMsg, "开始固件升级, 固件大小: %u 字节", fileSize);
    AppendLog(mgr, logMsg);

    // 发送开始升级命令
    uint8_t cmdData[8];
    cmdData[0] = BOARD_START_UPDATE & 0xFF;
    cmdData[1] = (BOARD_START_UPDATE >> 8) & 0xFF;
    cmdData[2] = (BOARD_START_UPDATE >> 16) & 0xFF;
    cmdData[3] = (BOARD_START_UPDATE >> 24) & 0xFF;
    cmdData[4] = fileSize & 0xFF;
    cmdData[5] = (fileSize >> 8) & 0xFF;
    cmdData[6] = (fileSize >> 16) & 0xFF;
    cmdData[7] = (fileSize >> 24) & 0xFF;

    uint8_t frame[32];
    int frameLen = BuildUARTFrame(FRAME_TYPE_CMD, cmdData, 8, frame, sizeof(frame));

    if (!UartWrite(mgr, frame, frameLen)) {
        CloseHandle(hFile);
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "发送固件大小失败");
        return 0;
    }

    uint32_t code, offset;
    if (!UartWaitForResponse(mgr, &code, &offset, 15000)) {
        CloseHandle(hFile);
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "Flash擦除超时");
        return 0;
    }

    if (code != FW_CODE_OFFSET || offset != 0) {
        CloseHandle(hFile);
        sprintf(logMsg, "Flash擦除失败: code(%u), offset(%u)", code, offset);
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, logMsg);
        return 0;
    }

    AppendLog(mgr, "Flash擦除完成");

    DWORD bytesSent = 0;
    DWORD bytesRead;
    uint8_t dataBuffer[8];
    int transferComplete = 0;

    while (ReadFile(hFile, dataBuffer, 8, &bytesRead, NULL) && bytesRead > 0) {
        // 填充到8字节
        while (bytesRead < 8) {
            dataBuffer[bytesRead++] = 0xFF;
        }

        // 构建数据帧
        frameLen = BuildUARTFrame(FRAME_TYPE_DATA, dataBuffer, 8, frame, sizeof(frame));

        if (!UartWrite(mgr, frame, frameLen)) {
            CloseHandle(hFile);
            LeaveCriticalSection(&mgr->criticalSection);
            AppendLog(mgr, "发送文件数据失败");
            return 0;
        }

        bytesSent += 8;

        // 每64字节或发送完毕后确认
        if (bytesSent % 64 == 0 || bytesSent >= fileSize) {
            // 更新进度
            if (mgr->progressCallback && bytesSent <= fileSize) {
                int percent = (int)(bytesSent * 100 / fileSize);
                mgr->progressCallback(percent);
            }

            if (!UartWaitForResponse(mgr, &code, &offset, 5000)) {
                CloseHandle(hFile);
                LeaveCriticalSection(&mgr->criticalSection);
                AppendLog(mgr, "固件更新超时");
                return 0;
            }

            if (code == FW_CODE_UPDATE_SUCCESS) {
                transferComplete = 1;
                break;
            }
            if (code != FW_CODE_OFFSET) {
                CloseHandle(hFile);
                sprintf(logMsg, "固件升级失败: code(%u), offset(%u)", code, offset);
                LeaveCriticalSection(&mgr->criticalSection);
                AppendLog(mgr, logMsg);
                return 0;
            }
        }
    }

    CloseHandle(hFile);

    // 设置进度为 100%
    if (mgr->progressCallback) {
        mgr->progressCallback(100);
    }

    // 检查传输是否完成
    if (!transferComplete && bytesSent > 0) {
        // 最后一次等待完成响应
        if (!UartWaitForResponse(mgr, &code, &offset, 5000)) {
            LeaveCriticalSection(&mgr->criticalSection);
            AppendLog(mgr, "等待固件传输完成超时");
            return 0;
        }
        if (code != FW_CODE_UPDATE_SUCCESS) {
            sprintf(logMsg, "固件传输未成功完成: code(%u), offset(%u)", code, offset);
            LeaveCriticalSection(&mgr->criticalSection);
            AppendLog(mgr, logMsg);
            return 0;
        }
    }

    // 发送确认命令
    cmdData[0] = BOARD_CONFIRM & 0xFF;
    cmdData[1] = (BOARD_CONFIRM >> 8) & 0xFF;
    cmdData[2] = (BOARD_CONFIRM >> 16) & 0xFF;
    cmdData[3] = (BOARD_CONFIRM >> 24) & 0xFF;
    uint32_t confirmParam = testMode ? 0 : 1;
    cmdData[4] = confirmParam & 0xFF;
    cmdData[5] = (confirmParam >> 8) & 0xFF;
    cmdData[6] = (confirmParam >> 16) & 0xFF;
    cmdData[7] = (confirmParam >> 24) & 0xFF;

    frameLen = BuildUARTFrame(FRAME_TYPE_CMD, cmdData, 8, frame, sizeof(frame));

    if (!UartWrite(mgr, frame, frameLen)) {
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "发送确认命令失败");
        return 0;
    }

    if (!UartWaitForResponse(mgr, &code, &offset, 30000)) {
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "固件确认超时");
        return 0;
    }

    if (code == FW_CODE_CONFIRM && offset == 0x55AA55AA) {
        WideCharToMultiByte(CP_UTF8, 0, fileName, -1, logMsg + 20, 200, NULL, NULL);
        sprintf(logMsg, "文件 %s 上传完成", logMsg + 20);
        AppendLog(mgr, logMsg);
        LeaveCriticalSection(&mgr->criticalSection);
        return 1;
    } else if (code == FW_CODE_TRANFER_ERROR) {
        LeaveCriticalSection(&mgr->criticalSection);
        AppendLog(mgr, "固件更新失败");
        return 0;
    }

    LeaveCriticalSection(&mgr->criticalSection);
    return 0;
}

// 枚举可用串口（使用 SetupAPI 快速枚举）
int UartManager_EnumPorts(UartManager* mgr, SerialPortInfo* ports, int maxCount) {
    int count = 0;
    HDEVINFO deviceInfoSet;
    SP_DEVINFO_DATA deviceInfoData;
    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    // 获取所有串口设备
    deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, 0, 0, DIGCF_PRESENT);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        AppendLog(mgr, "枚举串口失败");
        return 0;
    }

    // 遍历所有串口设备
    for (DWORD i = 0; i < 128 && count < maxCount; i++) {
        if (!SetupDiEnumDeviceInfo(deviceInfoSet, i, &deviceInfoData)) {
            break;  // 没有更多设备
        }

        // 获取设备描述（使用 Unicode 版本）
        WCHAR descW[256] = {0};
        DWORD dataSize = 0;
        DWORD dataType = 0;

        if (!SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &deviceInfoData,
                                                SPDRP_FRIENDLYNAME, &dataType,
                                                (PBYTE)descW, sizeof(descW), &dataSize)) {
            // 如果没有友好名称，尝试设备描述
            if (!SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &deviceInfoData,
                                                    SPDRP_DEVICEDESC, &dataType,
                                                    (PBYTE)descW, sizeof(descW), &dataSize)) {
                continue;
            }
        }

        // 过滤掉蓝牙串口（不区分大小写）
        WCHAR descLower[256] = {0};
        for (int j = 0; descW[j] && j < 255; j++) {
            descLower[j] = (WCHAR)tolower(descW[j]);
        }

        if (wcsstr(descLower, L"bluetooth") || wcsstr(descLower, L"蓝牙")) {
            continue;  // 跳过蓝牙串口
        }

        // 从描述中提取 COM 口号 (格式如 "USB Serial Port (COM3)")
        WCHAR* comStart = wcsstr(descW, L"(COM");
        if (comStart) {
            comStart += 4;  // 跳过 "(COM"
            int portNum = _wtoi(comStart);
            if (portNum > 0 && portNum <= 256) {
                // portName 存储为 "COM" + 号码，如 "COM3"
                sprintf(ports[count].portName, "COM%d", portNum);
                // friendlyName 也简化为 "COM3"
                sprintf(ports[count].friendlyName, "COM%d", portNum);
                count++;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    char msg[64];
    sprintf(msg, "查询到 %d 个可用串口", count);
    AppendLog(mgr, msg);

    return count;
}
