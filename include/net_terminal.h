#ifndef NET_TERMINAL_H
#define NET_TERMINAL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include "terminal_common.h"

typedef struct NetTerminal NetTerminal;

typedef void (*NetRecvCallback)(const char* data, int len, void* context);

NetTerminal* NetTerminal_Create(void);
void NetTerminal_Destroy(NetTerminal* nt);

/* use_tcp: 1=TCP, 0=UDP */
int NetTerminal_Connect(NetTerminal* nt, const char* host, int port, int use_tcp);
void NetTerminal_Disconnect(NetTerminal* nt);
int NetTerminal_IsConnected(NetTerminal* nt);

int NetTerminal_Send(NetTerminal* nt, const char* data, int len);
void NetTerminal_SetRecvCallback(NetTerminal* nt, NetRecvCallback cb, void* ctx);

#endif /* NET_TERMINAL_H */
