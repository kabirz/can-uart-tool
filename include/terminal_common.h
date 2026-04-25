#ifndef TERMINAL_COMMON_H
#define TERMINAL_COMMON_H

#include <windows.h>
#include <stdint.h>

#define MAX_HISTORY     256
#define LINE_BUF_SIZE   1024

typedef void (*TerminalSendFunc)(const char* data, int len, void* context);

typedef struct {
    HWND hwnd;                  /* 终端显示控件句柄（RichEdit 或 Edit） */
    wchar_t line_buffer[LINE_BUF_SIZE];
    int cursor_pos;
    int line_len;
    wchar_t* history[MAX_HISTORY];
    int history_count;
    int history_index;
    int is_shell_mode;          /* 0=raw透传, 1=shell模式(Zephyr快捷键) */
    TerminalSendFunc send_func;
    void* send_context;
    int insert_mode;            /* 0=insert, 1=overwrite (shell模式) */
    CRITICAL_SECTION cs;
} TerminalCtx;

TerminalCtx* Terminal_Create(HWND hwnd, int is_shell_mode);
void Terminal_Destroy(TerminalCtx* ctx);

void Terminal_AppendText(TerminalCtx* ctx, const char* utf8_text);
void Terminal_Clear(TerminalCtx* ctx);

/* Returns 1 if key was handled, 0 if not */
int Terminal_HandleKeyDown(TerminalCtx* ctx, WPARAM wParam, LPARAM lParam);
void Terminal_HandleChar(TerminalCtx* ctx, WPARAM wParam);

void Terminal_AddHistory(TerminalCtx* ctx, const wchar_t* cmd);
void Terminal_SetSendFunc(TerminalCtx* ctx, TerminalSendFunc func, void* context);

#endif /* TERMINAL_COMMON_H */
