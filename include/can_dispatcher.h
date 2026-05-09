#ifndef CAN_DISPATCHER_H
#define CAN_DISPATCHER_H

#include "can_hal.h"
#include <windows.h>

#define CAN_DISP_MAX_SUBSCRIBERS 4

typedef struct CanDispatcher CanDispatcher;

typedef void (*CanDispCallback)(const CanHalFrame *frame, void *ctx);

CanDispatcher *CanDisp_Create(CanHal *hal);
void          CanDisp_Destroy(CanDispatcher *disp);

void CanDisp_Start(CanDispatcher *disp);
void CanDisp_Stop(CanDispatcher *disp);

void CanDisp_Subscribe(CanDispatcher *disp, CanDispCallback cb, void *ctx);
void CanDisp_Unsubscribe(CanDispatcher *disp, CanDispCallback cb, void *ctx);

int  CanDisp_WaitFrame(CanDispatcher *disp, uint32_t expected_id,
                        CanHalFrame *out, int timeout_ms);

#endif /* CAN_DISPATCHER_H */
