/**
 * CAN Frame Dispatcher
 *
 * Single reader thread + subscriber pattern.
 * All CAN frame reads go through this dispatcher to avoid
 * race conditions between CanManager and CanCommand.
 */
#include "can_dispatcher.h"
#include <stdlib.h>
#include <string.h>

struct CanDispatcher {
    CanHal     *hal;

    /* Read thread */
    HANDLE      hThread;
    volatile LONG running;

    /* Subscriber list (protected by lock) */
    CRITICAL_SECTION lock;
    struct {
        CanDispCallback cb;
        void           *ctx;
    } subscribers[CAN_DISP_MAX_SUBSCRIBERS];
    int sub_count;

    /* Synchronous wait state (lock-free via Interlocked) */
    CanHalFrame  wait_frame;
    HANDLE       wait_sem;
    volatile LONG wait_armed;
    uint32_t     wait_filter_id;
};

/* ------------------------------------------------------------------ */
/*  Read thread – sole consumer of CanHal_Read                        */
/* ------------------------------------------------------------------ */
static DWORD WINAPI ReadThread(LPVOID param)
{
    CanDispatcher *disp = (CanDispatcher *)param;
    CanHalFrame frame;

    while (InterlockedCompareExchange(&disp->running, 1, 1) == 1) {
        if (!CanHal_Read(disp->hal, &frame, 10))
            continue;

        /* 1. Deliver to synchronous waiter if armed and matching */
        if (InterlockedCompareExchange(&disp->wait_armed, 1, 1) == 1) {
            if (frame.id == disp->wait_filter_id) {
                disp->wait_frame = frame;
                InterlockedExchange(&disp->wait_armed, 0);
                ReleaseSemaphore(disp->wait_sem, 1, NULL);
            }
        }

        /* 2. Dispatch to all async subscribers */
        EnterCriticalSection(&disp->lock);
        for (int i = 0; i < disp->sub_count; i++) {
            disp->subscribers[i].cb(&frame, disp->subscribers[i].ctx);
        }
        LeaveCriticalSection(&disp->lock);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

CanDispatcher *CanDisp_Create(CanHal *hal)
{
    CanDispatcher *disp = (CanDispatcher *)calloc(1, sizeof(CanDispatcher));
    if (!disp) return NULL;

    disp->hal       = hal;
    disp->running   = 0;
    disp->wait_armed = 0;
    disp->sub_count = 0;

    InitializeCriticalSection(&disp->lock);
    disp->wait_sem = CreateSemaphoreW(NULL, 0, 1, NULL);
    if (!disp->wait_sem) {
        DeleteCriticalSection(&disp->lock);
        free(disp);
        return NULL;
    }

    return disp;
}

void CanDisp_Destroy(CanDispatcher *disp)
{
    if (!disp) return;
    CanDisp_Stop(disp);
    if (disp->wait_sem) CloseHandle(disp->wait_sem);
    DeleteCriticalSection(&disp->lock);
    free(disp);
}

void CanDisp_Start(CanDispatcher *disp)
{
    if (!disp || InterlockedCompareExchange(&disp->running, 1, 0) != 0)
        return;

    InterlockedExchange(&disp->running, 1);
    disp->hThread = CreateThread(NULL, 0, ReadThread, disp, 0, NULL);
    if (!disp->hThread)
        InterlockedExchange(&disp->running, 0);
}

void CanDisp_Stop(CanDispatcher *disp)
{
    if (!disp) return;
    InterlockedExchange(&disp->running, 0);
    if (disp->hThread) {
        WaitForSingleObject(disp->hThread, 3000);
        CloseHandle(disp->hThread);
        disp->hThread = NULL;
    }
}

void CanDisp_Subscribe(CanDispatcher *disp, CanDispCallback cb, void *ctx)
{
    if (!disp || !cb) return;
    EnterCriticalSection(&disp->lock);
    if (disp->sub_count < CAN_DISP_MAX_SUBSCRIBERS) {
        disp->subscribers[disp->sub_count].cb  = cb;
        disp->subscribers[disp->sub_count].ctx  = ctx;
        disp->sub_count++;
    }
    LeaveCriticalSection(&disp->lock);
}

void CanDisp_Unsubscribe(CanDispatcher *disp, CanDispCallback cb, void *ctx)
{
    if (!disp || !cb) return;
    EnterCriticalSection(&disp->lock);
    for (int i = 0; i < disp->sub_count; i++) {
        if (disp->subscribers[i].cb == cb &&
            disp->subscribers[i].ctx == ctx) {
            for (int j = i; j < disp->sub_count - 1; j++)
                disp->subscribers[j] = disp->subscribers[j + 1];
            disp->sub_count--;
            break;
        }
    }
    LeaveCriticalSection(&disp->lock);
}

int CanDisp_WaitFrame(CanDispatcher *disp, uint32_t expected_id,
                       CanHalFrame *out, int timeout_ms)
{
    if (!disp) return 0;

    /* Arm waiter BEFORE sending command to avoid missing fast responses */
    disp->wait_filter_id = expected_id;
    InterlockedExchange(&disp->wait_armed, 1);

    DWORD result = WaitForSingleObject(disp->wait_sem, timeout_ms);

    if (result == WAIT_OBJECT_0) {
        if (out) *out = disp->wait_frame;
        return 1;
    }

    /* Timeout – disarm */
    InterlockedExchange(&disp->wait_armed, 0);
    return 0;
}
