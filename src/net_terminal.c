#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdlib.h>
#include "net_terminal.h"

struct NetTerminal {
    SOCKET sock;
    int use_tcp;
    int connected;
    HANDLE hRecvThread;
    volatile int recvRunning;
    NetRecvCallback recvCallback;
    void* recvCallbackCtx;
    struct sockaddr_in remoteAddr; /* For UDP */
    CRITICAL_SECTION cs;
};

static DWORD WINAPI RecvThread(LPVOID param)
{
    NetTerminal* nt = (NetTerminal*)param;
    char buffer[4096];

    while (nt->recvRunning && nt->connected) {
        int bytesRecv;

        if (nt->use_tcp) {
            bytesRecv = recv(nt->sock, buffer, sizeof(buffer) - 1, 0);
            if (bytesRecv <= 0) {
                /* Connection closed or error */
                nt->connected = 0;
                if (nt->recvCallback) {
                    nt->recvCallback(NULL, 0, nt->recvCallbackCtx); /* Signal disconnect */
                }
                break;
            }
        } else {
            struct sockaddr_in from;
            int fromLen = sizeof(from);
            bytesRecv = recvfrom(nt->sock, buffer, sizeof(buffer) - 1, 0,
                                 (struct sockaddr*)&from, &fromLen);
            if (bytesRecv <= 0) {
                Sleep(10);
                continue;
            }
        }

        if (bytesRecv > 0 && nt->recvCallback) {
            nt->recvCallback(buffer, bytesRecv, nt->recvCallbackCtx);
        }
    }
    return 0;
}

NetTerminal* NetTerminal_Create(void)
{
    NetTerminal* nt = (NetTerminal*)calloc(1, sizeof(NetTerminal));
    if (!nt) return NULL;

    nt->sock = INVALID_SOCKET;
    nt->connected = 0;
    nt->recvRunning = 0;
    nt->hRecvThread = NULL;
    nt->recvCallback = NULL;
    nt->recvCallbackCtx = NULL;
    memset(&nt->remoteAddr, 0, sizeof(nt->remoteAddr));

    InitializeCriticalSection(&nt->cs);

    return nt;
}

void NetTerminal_Destroy(NetTerminal* nt)
{
    if (!nt) return;

    if (nt->connected) {
        NetTerminal_Disconnect(nt);
    }

    DeleteCriticalSection(&nt->cs);
    free(nt);
}

int NetTerminal_Connect(NetTerminal* nt, const char* host, int port, int use_tcp)
{
    WSADATA wsaData;

    if (!nt) return 0;

    if (nt->connected) {
        NetTerminal_Disconnect(nt);
    }

    /* WSAStartup (safe to call multiple times; ref-counted by Windows) */
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return 0;
    }

    /* Create socket */
    nt->sock = socket(AF_INET, use_tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (nt->sock == INVALID_SOCKET) {
        return 0;
    }

    /* Resolve host */
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (addr.sin_addr.s_addr == INADDR_NONE) {
        /* Try to resolve hostname */
        struct hostent* he = gethostbyname(host);
        if (!he) {
            closesocket(nt->sock);
            nt->sock = INVALID_SOCKET;
            return 0;
        }
        addr.sin_addr.s_addr = *(u_long*)he->h_addr_list[0];
    }

    if (use_tcp) {
        if (connect(nt->sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            closesocket(nt->sock);
            nt->sock = INVALID_SOCKET;
            return 0;
        }
    } else {
        /* For UDP, store remote address for sendto */
        nt->remoteAddr = addr;
    }

    nt->use_tcp = use_tcp;
    nt->connected = 1;

    /* Start recv thread */
    nt->recvRunning = 1;
    nt->hRecvThread = CreateThread(NULL, 0, RecvThread, nt, 0, NULL);

    return 1;
}

void NetTerminal_Disconnect(NetTerminal* nt)
{
    if (!nt) return;

    EnterCriticalSection(&nt->cs);

    nt->recvRunning = 0;
    nt->connected = 0;

    if (nt->sock != INVALID_SOCKET) {
        /* Shutdown the socket to unblock any pending recv/recvfrom */
        shutdown(nt->sock, SD_BOTH);
        closesocket(nt->sock);
        nt->sock = INVALID_SOCKET;
    }

    LeaveCriticalSection(&nt->cs);

    if (nt->hRecvThread) {
        WaitForSingleObject(nt->hRecvThread, 3000);
        CloseHandle(nt->hRecvThread);
        nt->hRecvThread = NULL;
    }

    WSACleanup();
}

int NetTerminal_IsConnected(NetTerminal* nt)
{
    if (!nt) return 0;
    return nt->connected;
}

int NetTerminal_Send(NetTerminal* nt, const char* data, int len)
{
    if (!nt || !nt->connected) return 0;

    int sent;
    if (nt->use_tcp) {
        sent = send(nt->sock, data, len, 0);
    } else {
        sent = sendto(nt->sock, data, len, 0,
                      (struct sockaddr*)&nt->remoteAddr, sizeof(nt->remoteAddr));
    }
    return (sent > 0) ? sent : 0;
}

void NetTerminal_SetRecvCallback(NetTerminal* nt, NetRecvCallback cb, void* ctx)
{
    if (!nt) return;
    EnterCriticalSection(&nt->cs);
    nt->recvCallback = cb;
    nt->recvCallbackCtx = ctx;
    LeaveCriticalSection(&nt->cs);
}
