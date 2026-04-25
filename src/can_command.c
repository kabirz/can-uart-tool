#include "can_command.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Default quick commands                                             */
/* ------------------------------------------------------------------ */
static const CanQuickCommand g_defaultCommands[] = {
    { 0x101, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, 8, 0, 0, "启动升级" },
    { 0x101, {0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, 8, 0, 0, "确认" },
    { 0x101, {0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, 8, 0, 0, "查版本" },
    { 0x101, {0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, 8, 0, 0, "重启" },
};
static const int g_defaultCommandCount =
    sizeof(g_defaultCommands) / sizeof(g_defaultCommands[0]);

/* ------------------------------------------------------------------ */
/*  Internal structure                                                 */
/* ------------------------------------------------------------------ */
struct CanCommand {
    TPCANHandle      channel;
    CanFrameCallback frameCallback;
    void*            frameCallbackCtx;
    HANDLE           hMonitorThread;
    volatile int     monitorRunning;
    CanQuickCommand  quickCommands[MAX_QUICK_COMMANDS];
    int              quickCommandCount;
};

/* ------------------------------------------------------------------ */
/*  Monitor thread                                                     */
/* ------------------------------------------------------------------ */
static DWORD WINAPI MonitorThread(LPVOID param)
{
    CanCommand* cmd = (CanCommand*)param;
    TPCANMsg      msg;
    TPCANTimestamp ts;

    while (cmd->monitorRunning) {
        TPCANStatus status = CAN_Read(cmd->channel, &msg, &ts);
        if (status == PCAN_ERROR_OK) {
            if (cmd->frameCallback) {
                cmd->frameCallback(msg.ID, msg.DATA, msg.LEN, 0,
                                   cmd->frameCallbackCtx);
            }
        } else if (status == PCAN_ERROR_QRCVEMPTY) {
            Sleep(1);
        } else {
            Sleep(10);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Create / Destroy                                                   */
/* ------------------------------------------------------------------ */
CanCommand* CanCommand_Create(TPCANHandle channel)
{
    CanCommand* cmd = (CanCommand*)calloc(1, sizeof(CanCommand));
    if (!cmd) return NULL;

    cmd->channel          = channel;
    cmd->frameCallback    = NULL;
    cmd->frameCallbackCtx = NULL;
    cmd->hMonitorThread   = NULL;
    cmd->monitorRunning   = 0;

    /* Copy default quick commands */
    CanCommand_SetQuickCommands(cmd, g_defaultCommands, g_defaultCommandCount);

    return cmd;
}

void CanCommand_Destroy(CanCommand* cmd)
{
    if (!cmd) return;
    CanCommand_StopMonitor(cmd);
    free(cmd);
}

/* ------------------------------------------------------------------ */
/*  Channel management                                                 */
/* ------------------------------------------------------------------ */
void CanCommand_SetChannel(CanCommand* cmd, TPCANHandle channel)
{
    if (cmd) cmd->channel = channel;
}

/* ------------------------------------------------------------------ */
/*  Send frame                                                         */
/* ------------------------------------------------------------------ */
int CanCommand_SendFrame(CanCommand* cmd, uint32_t can_id,
                         const uint8_t* data, int dlc,
                         int is_extended, int is_remote)
{
    if (!cmd || cmd->channel == PCAN_NONEBUS) return 0;

    TPCANMsg msg;
    msg.ID      = can_id;
    msg.MSGTYPE = (TPCANMessageType)(is_extended ? PCAN_MODE_EXTENDED
                                                 : PCAN_MODE_STANDARD);
    if (is_remote)
        msg.MSGTYPE |= PCAN_MESSAGE_RTR;
    msg.LEN = (BYTE)((dlc > 8) ? 8 : dlc);
    memset(msg.DATA, 0, 8);
    if (data && dlc > 0)
        memcpy(msg.DATA, data, msg.LEN);

    TPCANStatus status = CAN_Write(cmd->channel, &msg);

    /* Notify callback about TX frame */
    if (cmd->frameCallback && status == PCAN_ERROR_OK) {
        cmd->frameCallback(can_id, data, msg.LEN, 1, cmd->frameCallbackCtx);
    }

    return (status == PCAN_ERROR_OK) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Quick commands                                                     */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  Frame callback                                                     */
/* ------------------------------------------------------------------ */
void CanCommand_SetFrameCallback(CanCommand* cmd, CanFrameCallback cb,
                                 void* ctx)
{
    if (!cmd) return;
    cmd->frameCallback    = cb;
    cmd->frameCallbackCtx = ctx;
}

/* ------------------------------------------------------------------ */
/*  Monitor start / stop                                               */
/* ------------------------------------------------------------------ */
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
