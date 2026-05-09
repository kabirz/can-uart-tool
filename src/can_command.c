/**
 * CAN Command – frame sending + bus monitoring via CanDispatcher.
 *
 * Monitoring no longer owns a reader thread.
 * Instead it subscribes to the shared CanDispatcher, which is the
 * sole owner of the CAN read thread.
 */
#include "can_command.h"
#include "can_dispatcher.h"
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
    CanDispatcher    *disp;
    int               channel;
    CanFrameCallback  frameCallback;
    void*             frameCallbackCtx;
    volatile int      monitoring;    /* subscribed to dispatcher */
    CanQuickCommand   quickCommands[MAX_QUICK_COMMANDS];
    int               quickCommandCount;
};

/* Dispatcher callback – converts CanHalFrame to user-level callback */
static void DispFrameCallback(const CanHalFrame *frame, void *ctx)
{
    CanCommand* cmd = (CanCommand*)ctx;
    if (cmd->frameCallback && cmd->monitoring) {
        cmd->frameCallback(frame->id, frame->data, frame->dlc, 0,
                           cmd->frameCallbackCtx);
    }
}

CanCommand* CanCommand_Create(CanHal *hal, CanDispatcher *disp)
{
    CanCommand* cmd = (CanCommand*)calloc(1, sizeof(CanCommand));
    if (!cmd) return NULL;

    cmd->hal              = hal;
    cmd->disp             = disp;
    cmd->channel          = CAN_HAL_INVALID_HANDLE;
    cmd->frameCallback    = NULL;
    cmd->frameCallbackCtx = NULL;
    cmd->monitoring       = 0;

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

void CanCommand_ReplaceHal(CanCommand *cmd, CanHal *hal, CanDispatcher *disp)
{
    if (!cmd) return;
    int wasMonitoring = cmd->monitoring;
    if (wasMonitoring)
        CanCommand_StopMonitor(cmd);
    cmd->hal  = hal;
    cmd->disp = disp;
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
    if (!cmd || cmd->monitoring || !cmd->disp) return;
    cmd->monitoring = 1;
    CanDisp_Subscribe(cmd->disp, DispFrameCallback, cmd);
}

void CanCommand_StopMonitor(CanCommand* cmd)
{
    if (!cmd || !cmd->monitoring) return;
    cmd->monitoring = 0;
    if (cmd->disp)
        CanDisp_Unsubscribe(cmd->disp, DispFrameCallback, cmd);
}
