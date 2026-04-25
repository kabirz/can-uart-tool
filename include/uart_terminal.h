#ifndef UART_TERMINAL_H
#define UART_TERMINAL_H

#include <windows.h>
#include "terminal_common.h"

typedef struct UartTerminal UartTerminal;

typedef void (*UartRecvCallback)(const char* data, int len, void* context);

UartTerminal* UartTerminal_Create(void);
void UartTerminal_Destroy(UartTerminal* ut);

int UartTerminal_Connect(UartTerminal* ut, const char* port, DWORD baudrate);
void UartTerminal_Disconnect(UartTerminal* ut);
int UartTerminal_IsConnected(UartTerminal* ut);

int UartTerminal_Send(UartTerminal* ut, const char* data, int len);
void UartTerminal_SetRecvCallback(UartTerminal* ut, UartRecvCallback cb, void* ctx);

void UartTerminal_StartRecv(UartTerminal* ut);
void UartTerminal_StopRecv(UartTerminal* ut);

/* Enumerate serial ports (returns count, fills ports array with "COM3" etc.) */
int UartTerminal_EnumPorts(char ports[][32], int max_count);

#endif /* UART_TERMINAL_H */
