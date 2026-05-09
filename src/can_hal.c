#include "can_hal.h"
#include <stdlib.h>

const int CAN_HAL_BAUD_VALUES[CAN_HAL_BAUD_COUNT] = {
    10000, 20000, 50000, 100000,
    125000, 250000, 500000, 1000000
};

extern CanHal *CanHal_CreatePCAN(void);
extern CanHal *CanHal_CreateIXXAT(void);

CanHal *CanHal_Create(int adapter_type)
{
    switch (adapter_type) {
    case CAN_HAL_ADAPTER_PCAN:
        return CanHal_CreatePCAN();
    case CAN_HAL_ADAPTER_IXXAT:
        return CanHal_CreateIXXAT();
    default:
        return NULL;
    }
}

void CanHal_Destroy(CanHal *hal)
{
    if (hal && hal->ops && hal->ops->destroy)
        hal->ops->destroy(hal);
}

void CanHal_SetLogCallback(CanHal *hal, CanHalLogCallback cb)
{
    if (hal) hal->log_cb = cb;
}

int CanHal_DetectDevices(CanHal *hal, int *channels, int max_count)
{
    if (!hal || !hal->ops || !hal->ops->detect) return 0;
    return hal->ops->detect(hal, channels, max_count);
}

int CanHal_Connect(CanHal *hal, int channel, int baud_index)
{
    if (!hal || !hal->ops || !hal->ops->connect) return 0;
    return hal->ops->connect(hal, channel, baud_index);
}

void CanHal_Disconnect(CanHal *hal)
{
    if (hal && hal->ops && hal->ops->disconnect)
        hal->ops->disconnect(hal);
}

int CanHal_Write(CanHal *hal, const CanHalFrame *frame)
{
    if (!hal || !hal->ops || !hal->ops->write) return 0;
    return hal->ops->write(hal, frame);
}

int CanHal_Read(CanHal *hal, CanHalFrame *frame, int timeout_ms)
{
    if (!hal || !hal->ops || !hal->ops->read) return 0;
    return hal->ops->read(hal, frame, timeout_ms);
}

int CanHal_SetFilter(CanHal *hal, uint32_t from_id, uint32_t to_id)
{
    if (!hal || !hal->ops || !hal->ops->set_filter) return 0;
    return hal->ops->set_filter(hal, from_id, to_id);
}

int CanHal_IsConnected(CanHal *hal)
{
    return hal ? hal->connected : 0;
}

int CanHal_GetChannel(CanHal *hal)
{
    return hal ? hal->channel : CAN_HAL_INVALID_HANDLE;
}

const char *CanHal_GetName(CanHal *hal)
{
    if (hal && hal->ops) return hal->ops->name;
    return "Unknown";
}
