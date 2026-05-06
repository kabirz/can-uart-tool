#include "can_command.h"
#include <stdlib.h>
#include <string.h>

static const CanQuickCommand g_defaultCommands[] = {
    { 0x101, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, 8, 0, 0, "启动升级" },
    { 0x101, {0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, 8, 0, 0, "确认" },
    { 0x101, {0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, 8, 0, 0, "查版本" },
    { 0x101, {0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, 8, 0, 0, "重启" },
};
static const int g_defaultCommandCount =
    sizeof(g_defaultCommands) / sizeof(g_defaultCommands[0]);

struct CanCommand {
    CanHal           *hal;
    int               channel;
    CanFrameCallback  frameCallback;
    void*             frameCallbackCtx;
    HANDLE            hMonitorThread;
    volatile int      monitorRunning;
    CanQuickCommand   quickCommands[MAX_QUICK_COMMANDS];
    int               quickCommandCount;
};

static DWORD WINAPI MonitorThread(LPVOID param)
{
    CanCommand* cmd = (CanCommand*)param;
    CanHalFrame frame;

    while (cmd->monitorRunning) {
        int result = CanHal_Read(cmd->hal, &frame, 10);
        if (result) {
            if (cmd->frameCallback) {
                cmd->frameCallback(frame.id, frame.data, frame.dlc, 0,
                                   cmd->frameCallbackCtx);
            }
        } else {
            Sleep(1);
        }
    }
    return 0;
}

CanCommand* CanCommand_Create(CanHal *hal)
{
    CanCommand* cmd = (CanCommand*)calloc(1, sizeof(CanCommand));
    if (!cmd) return NULL;

    cmd->hal              = hal;
    cmd->channel          = CAN_HAL_INVALID_HANDLE;
    cmd->frameCallback    = NULL;
    cmd->frameCallbackCtx = NULL;
    cmd->hMonitorThread   = NULL;
    cmd->monitorRunning   = 0;

    CanCommand_SetQuickCommands(cmd, g_defaultCommands, g_defaultCommandCount);

    return cmd;
}

void CanCommand_Destroy(CanCommand* cmd)
{
    if (!cmd) return;
    CanCommand_StopMonitor(cmd);
    free(cmd);
}

void CanCommand_SetChannel(CanCommand* cmd, int channel)
{
    if (cmd) cmd->channel = channel;
}

int CanCommand_SendFrame(CanCommand* cmd, uint32_t can_id,
                         const uint8_t* data, int dlc,
                         int is_extended, int is_remote)
{
    if (!cmd || cmd->channel == CAN_HAL_INVALID_HANDLE) return 0;
    if (!cmd->hal) return 0;

    CanHalFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.id = can_id;
    frame.flags = is_extended ? CAN_HAL_FLAG_EXTENDED : CAN_HAL_FLAG_STANDARD;
    if (is_remote)
        frame.flags |= CAN_HAL_FLAG_REMOTE;
    frame.dlc = (dlc > 8) ? 8 : (uint8_t)dlc;
    if (data && dlc > 0)
        memcpy(frame.data, data, frame.dlc);

    int status = CanHal_Write(cmd->hal, &frame);

    if (cmd->frameCallback && status) {
        cmd->frameCallback(can_id, data, frame.dlc, 1, cmd->frameCallbackCtx);
    }

    return status;
}

int CanCommand_GetQuickCommandCount(CanCommand* cmd)
{
    return cmd ? cmd->quickCommandCount : 0;
}

const CanQuickCommand* CanCommand_GetQuickCommand(CanCommand* cmd, int index)
{
    if (!cmd || index < 0 || index >= cmd->quickCommandCount)
        return NULL;
    return &cmd->quickCommands[index];
}

void CanCommand_SetQuickCommands(CanCommand* cmd,
                                 const CanQuickCommand* cmds, int count)
{
    if (!cmd) return;
    if (count > MAX_QUICK_COMMANDS)
        count = MAX_QUICK_COMMANDS;
    if (count > 0 && cmds)
        memcpy(cmd->quickCommands, cmds, count * sizeof(CanQuickCommand));
    cmd->quickCommandCount = count;
}

void CanCommand_SetFrameCallback(CanCommand* cmd, CanFrameCallback cb,
                                 void* ctx)
{
    if (!cmd) return;
    cmd->frameCallback    = cb;
    cmd->frameCallbackCtx = ctx;
}

void CanCommand_StartMonitor(CanCommand* cmd)
{
    if (!cmd || cmd->monitorRunning) return;

    cmd->monitorRunning = 1;
    cmd->hMonitorThread = CreateThread(NULL, 0, MonitorThread, cmd,
                                       0, NULL);
    if (!cmd->hMonitorThread)
        cmd->monitorRunning = 0;
}

void CanCommand_StopMonitor(CanCommand* cmd)
{
    if (!cmd || !cmd->monitorRunning) return;

    cmd->monitorRunning = 0;
    if (cmd->hMonitorThread) {
        WaitForSingleObject(cmd->hMonitorThread, 3000);
        CloseHandle(cmd->hMonitorThread);
        cmd->hMonitorThread = NULL;
    }
}
