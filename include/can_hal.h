#ifndef CAN_HAL_H
#define CAN_HAL_H

#include <stdint.h>

#define CAN_HAL_MAX_DEVICES     16
#define CAN_HAL_MAX_DATA_LEN    8
#define CAN_HAL_INVALID_HANDLE  (-1)

#define CAN_HAL_FLAG_STANDARD   0x00
#define CAN_HAL_FLAG_EXTENDED   0x01
#define CAN_HAL_FLAG_REMOTE     0x02

#define CAN_HAL_BAUD_10K    0
#define CAN_HAL_BAUD_20K    1
#define CAN_HAL_BAUD_50K    2
#define CAN_HAL_BAUD_100K   3
#define CAN_HAL_BAUD_125K   4
#define CAN_HAL_BAUD_250K   5
#define CAN_HAL_BAUD_500K   6
#define CAN_HAL_BAUD_1M     7
#define CAN_HAL_BAUD_COUNT  8

#define CAN_HAL_ADAPTER_PCAN    0
#define CAN_HAL_ADAPTER_IXXAT   1
#define CAN_HAL_ADAPTER_COUNT   2

#define CAN_HAL_VIRTUAL_CHANNEL 0xFFFF

typedef struct {
    uint32_t id;
    uint8_t  data[CAN_HAL_MAX_DATA_LEN];
    uint8_t  dlc;
    uint8_t  flags;
} CanHalFrame;

typedef struct CanHal CanHal;

typedef void (*CanHalLogCallback)(const char *msg);

struct CanHal {
    const struct CanHalOps *ops;
    void *priv;
    int channel;
    int connected;
    CanHalLogCallback log_cb;
};

typedef struct CanHalOps {
    const char *name;
    int  (*detect)(CanHal *hal, int *channels, int max_count);
    int  (*connect)(CanHal *hal, int channel, int baud_index);
    void (*disconnect)(CanHal *hal);
    int  (*write)(CanHal *hal, const CanHalFrame *frame);
    int  (*read)(CanHal *hal, CanHalFrame *frame, int timeout_ms);
    int  (*set_filter)(CanHal *hal, uint32_t from_id, uint32_t to_id);
    void (*destroy)(CanHal *hal);
} CanHalOps;

CanHal *CanHal_Create(int adapter_type);
void    CanHal_Destroy(CanHal *hal);
void    CanHal_SetLogCallback(CanHal *hal, CanHalLogCallback cb);

int     CanHal_DetectDevices(CanHal *hal, int *channels, int max_count);
int     CanHal_Connect(CanHal *hal, int channel, int baud_index);
void    CanHal_Disconnect(CanHal *hal);
int     CanHal_Write(CanHal *hal, const CanHalFrame *frame);
int     CanHal_Read(CanHal *hal, CanHalFrame *frame, int timeout_ms);
int     CanHal_SetFilter(CanHal *hal, uint32_t from_id, uint32_t to_id);
int     CanHal_IsConnected(CanHal *hal);
int     CanHal_GetChannel(CanHal *hal);
const char *CanHal_GetName(CanHal *hal);

extern const int CAN_HAL_BAUD_VALUES[CAN_HAL_BAUD_COUNT];

#endif
