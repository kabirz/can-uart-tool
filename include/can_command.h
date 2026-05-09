#ifndef CAN_COMMAND_H
#define CAN_COMMAND_H

#include <windows.h>
#include <stdint.h>
#include "can_hal.h"

#define MAX_QUICK_COMMANDS  32
#define QUICK_CMD_NAME_LEN  32

typedef struct {
    uint32_t can_id;
    uint8_t data[8];
    uint8_t dlc;
    int is_extended;
    int is_remote;
    char name[QUICK_CMD_NAME_LEN];
} CanQuickCommand;

typedef struct CanDispatcher CanDispatcher;
typedef struct CanCommand CanCommand;

typedef void (*CanFrameCallback)(uint32_t id, const uint8_t* data,
                                  int dlc, int is_tx, void* context);

CanCommand* CanCommand_Create(CanHal *hal, CanDispatcher *disp);
void CanCommand_Destroy(CanCommand* cmd);

void CanCommand_SetChannel(CanCommand* cmd, int channel);

/* Replace HAL + dispatcher (for adapter switching) */
void CanCommand_ReplaceHal(CanCommand *cmd, CanHal *hal, CanDispatcher *disp);

int CanCommand_SendFrame(CanCommand* cmd, uint32_t can_id,
                         const uint8_t* data, int dlc,
                         int is_extended, int is_remote);

int CanCommand_GetQuickCommandCount(CanCommand* cmd);
const CanQuickCommand* CanCommand_GetQuickCommand(CanCommand* cmd, int index);
void CanCommand_SetQuickCommands(CanCommand* cmd, const CanQuickCommand* cmds, int count);

void CanCommand_SetFrameCallback(CanCommand* cmd, CanFrameCallback cb, void* ctx);
void CanCommand_StartMonitor(CanCommand* cmd);
void CanCommand_StopMonitor(CanCommand* cmd);

#endif
