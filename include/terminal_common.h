#ifndef TERMINAL_COMMON_H
#define TERMINAL_COMMON_H

#include <windows.h>
#include <stdint.h>

typedef void (*TerminalSendFunc)(const char* data, int len, void* context);

typedef struct {
    HWND hwnd;                  /* Terminal display control (RichEdit) */
    int is_shell_mode;          /* 0=raw passthrough, 1=shell mode */
    TerminalSendFunc send_func;
    void* send_context;
    COLORREF default_fg;        /* Default foreground color */
    COLORREF default_bg;        /* Default background color */
    COLORREF cur_fg;            /* Current foreground color */
    COLORREF cur_bg;            /* Current background color */
    int ansi_bold;              /* ANSI bold flag */
    char ansi_buf[64];          /* Incomplete ANSI sequence buffer */
    int ansi_buf_len;
    CRITICAL_SECTION cs;
} TerminalCtx;

TerminalCtx* Terminal_Create(HWND hwnd, int is_shell_mode);
void Terminal_Destroy(TerminalCtx* ctx);

void Terminal_AppendText(TerminalCtx* ctx, const char* utf8_text);
void Terminal_Clear(TerminalCtx* ctx);

/* Returns 1 if key was handled, 0 if not */
int Terminal_HandleKeyDown(TerminalCtx* ctx, WPARAM wParam, LPARAM lParam);
void Terminal_HandleChar(TerminalCtx* ctx, WPARAM wParam);

void Terminal_SetSendFunc(TerminalCtx* ctx, TerminalSendFunc func, void* context);

#endif /* TERMINAL_COMMON_H */
