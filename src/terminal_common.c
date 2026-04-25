#include "terminal_common.h"
#include <stdlib.h>
#include <string.h>
#include <richedit.h>

/* ------------------------------------------------------------------ */
/*  Helper: replace the current editable line in the Edit/RichEdit    */
/*  control.  This is used for history navigation and inline editing. */
/* ------------------------------------------------------------------ */
static void replace_current_line(TerminalCtx* ctx)
{
    /* Select from after the last newline to the end, then replace */
    HWND hwnd = ctx->hwnd;
    int total = GetWindowTextLengthW(hwnd);
    /* We'll just replace from the end - line_len characters */
    int sel_start = total - ctx->line_len;
    if (sel_start < 0) sel_start = 0;
    SendMessageW(hwnd, EM_SETSEL, (WPARAM)sel_start, (LPARAM)total);
    SendMessageW(hwnd, EM_REPLACESEL, FALSE, (LPARAM)ctx->line_buffer);
}

TerminalCtx* Terminal_Create(HWND hwnd, int is_shell_mode)
{
    TerminalCtx* ctx = (TerminalCtx*)malloc(sizeof(TerminalCtx));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(TerminalCtx));
    ctx->hwnd = hwnd;
    ctx->cursor_pos = 0;
    ctx->line_len = 0;
    ctx->history_index = -1;
    ctx->history_count = 0;
    ctx->is_shell_mode = is_shell_mode;
    ctx->insert_mode = 0;
    ctx->send_func = NULL;
    ctx->send_context = NULL;

    InitializeCriticalSection(&ctx->cs);

    return ctx;
}

void Terminal_Destroy(TerminalCtx* ctx)
{
    if (!ctx) return;

    DeleteCriticalSection(&ctx->cs);

    for (int i = 0; i < ctx->history_count; i++) {
        free(ctx->history[i]);
        ctx->history[i] = NULL;
    }

    free(ctx);
}

void Terminal_AppendText(TerminalCtx* ctx, const char* utf8_text)
{
    if (!ctx || !utf8_text) return;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, NULL, 0);
    if (wlen <= 0) return;

    wchar_t* wbuf = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wbuf) return;

    MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, wbuf, wlen);

    EnterCriticalSection(&ctx->cs);

    int text_len = GetWindowTextLengthW(ctx->hwnd);
    SendMessageW(ctx->hwnd, EM_SETSEL, (WPARAM)text_len, (LPARAM)text_len);
    /* wlen includes the null terminator from MultiByteToWideChar, so send wlen-1 chars */
    SendMessageW(ctx->hwnd, EM_REPLACESEL, FALSE, (LPARAM)wbuf);
    SendMessageW(ctx->hwnd, EM_SCROLLCARET, 0, 0);

    LeaveCriticalSection(&ctx->cs);

    free(wbuf);
}

void Terminal_Clear(TerminalCtx* ctx)
{
    if (!ctx) return;
    SetWindowTextW(ctx->hwnd, L"");
}

int Terminal_HandleKeyDown(TerminalCtx* ctx, WPARAM wParam, LPARAM lParam)
{
    if (!ctx) return 0;

    if (ctx->is_shell_mode) {
        /* ---------- Shell mode (Zephyr shell shortcuts) ---------- */
        switch (wParam) {
        case VK_TAB:
            if (ctx->send_func) {
                ctx->send_func("\t", 1, ctx->send_context);
            }
            return 1;

        case VK_UP:
            if (ctx->history_count > 0) {
                if (ctx->history_index <= 0)
                    ctx->history_index = 0;
                else
                    ctx->history_index--;

                wchar_t* entry = ctx->history[ctx->history_index];
                wcscpy(ctx->line_buffer, entry);
                ctx->line_len = (int)wcslen(entry);
                ctx->cursor_pos = ctx->line_len;
                replace_current_line(ctx);
            }
            return 1;

        case VK_DOWN:
            if (ctx->history_index >= ctx->history_count - 1) {
                ctx->history_index = -1;
                ctx->line_buffer[0] = L'\0';
                ctx->line_len = 0;
                ctx->cursor_pos = 0;
            } else {
                ctx->history_index++;
                wchar_t* entry = ctx->history[ctx->history_index];
                wcscpy(ctx->line_buffer, entry);
                ctx->line_len = (int)wcslen(entry);
                ctx->cursor_pos = ctx->line_len;
            }
            replace_current_line(ctx);
            return 1;

        case VK_LEFT:
            if (ctx->cursor_pos > 0)
                ctx->cursor_pos--;
            return 1;

        case VK_RIGHT:
            if (ctx->cursor_pos < ctx->line_len)
                ctx->cursor_pos++;
            return 1;

        case VK_HOME:
            ctx->cursor_pos = 0;
            return 1;

        case VK_END:
            ctx->cursor_pos = ctx->line_len;
            return 1;

        case VK_INSERT:
            ctx->insert_mode = !ctx->insert_mode;
            return 1;

        case VK_DELETE:
            if (ctx->cursor_pos < ctx->line_len) {
                memmove(&ctx->line_buffer[ctx->cursor_pos],
                        &ctx->line_buffer[ctx->cursor_pos + 1],
                        (ctx->line_len - ctx->cursor_pos) * sizeof(wchar_t));
                ctx->line_len--;
                replace_current_line(ctx);
            }
            return 1;

        case VK_RETURN:
        {
            /* Convert line_buffer to UTF-8 and send */
            ctx->line_buffer[ctx->line_len] = L'\0';

            if (ctx->send_func) {
                int ulen = WideCharToMultiByte(CP_UTF8, 0, ctx->line_buffer,
                                               ctx->line_len, NULL, 0, NULL, NULL);
                if (ulen > 0) {
                    char* ubuf = (char*)malloc(ulen + 2);  /* +2 for \r\n */
                    if (ubuf) {
                        WideCharToMultiByte(CP_UTF8, 0, ctx->line_buffer,
                                            ctx->line_len, ubuf, ulen, NULL, NULL);
                        ubuf[ulen] = '\r';
                        ubuf[ulen + 1] = '\n';
                        ctx->send_func(ubuf, ulen + 2, ctx->send_context);
                        free(ubuf);
                    }
                }
            }

            /* Display the entered line in the terminal */
            Terminal_AppendText(ctx, "\r\n");

            /* Add to history if non-empty */
            if (ctx->line_len > 0) {
                Terminal_AddHistory(ctx, ctx->line_buffer);
            }

            /* Reset line state */
            ctx->line_buffer[0] = L'\0';
            ctx->cursor_pos = 0;
            ctx->line_len = 0;
            ctx->history_index = -1;
            return 1;
        }

        case VK_BACK:
            if (ctx->cursor_pos > 0) {
                memmove(&ctx->line_buffer[ctx->cursor_pos - 1],
                        &ctx->line_buffer[ctx->cursor_pos],
                        (ctx->line_len - ctx->cursor_pos) * sizeof(wchar_t));
                ctx->cursor_pos--;
                ctx->line_len--;
                ctx->line_buffer[ctx->line_len] = L'\0';
                replace_current_line(ctx);
            }
            return 1;

        default:
            return 0;
        }
    } else {
        /* ---------- Raw 透传 mode ---------- */
        switch (wParam) {
        case VK_RETURN:
        {
            ctx->line_buffer[ctx->line_len] = L'\0';

            if (ctx->send_func) {
                int ulen = WideCharToMultiByte(CP_UTF8, 0, ctx->line_buffer,
                                               ctx->line_len, NULL, 0, NULL, NULL);
                if (ulen > 0) {
                    char* ubuf = (char*)malloc(ulen + 2);
                    if (ubuf) {
                        WideCharToMultiByte(CP_UTF8, 0, ctx->line_buffer,
                                            ctx->line_len, ubuf, ulen, NULL, NULL);
                        ubuf[ulen] = '\r';
                        ubuf[ulen + 1] = '\n';
                        ctx->send_func(ubuf, ulen + 2, ctx->send_context);
                        free(ubuf);
                    }
                }
            }

            Terminal_AppendText(ctx, "\r\n");

            ctx->line_buffer[0] = L'\0';
            ctx->cursor_pos = 0;
            ctx->line_len = 0;
            return 1;
        }

        default:
            return 0;
        }
    }
}

void Terminal_HandleChar(TerminalCtx* ctx, WPARAM wParam)
{
    if (!ctx) return;

    wchar_t ch = (wchar_t)wParam;

    if (ctx->is_shell_mode) {
        /* Insert character at cursor_pos */
        if (ctx->line_len < LINE_BUF_SIZE - 1) {
            if (ctx->insert_mode) {
                /* Overwrite mode */
                if (ctx->cursor_pos < ctx->line_len) {
                    ctx->line_buffer[ctx->cursor_pos] = ch;
                } else {
                    ctx->line_buffer[ctx->cursor_pos] = ch;
                    ctx->line_len++;
                }
                ctx->cursor_pos++;
            } else {
                /* Insert mode: shift characters right */
                memmove(&ctx->line_buffer[ctx->cursor_pos + 1],
                        &ctx->line_buffer[ctx->cursor_pos],
                        (ctx->line_len - ctx->cursor_pos) * sizeof(wchar_t));
                ctx->line_buffer[ctx->cursor_pos] = ch;
                ctx->cursor_pos++;
                ctx->line_len++;
            }
            ctx->line_buffer[ctx->line_len] = L'\0';
            replace_current_line(ctx);
        }
    } else {
        /* Raw mode: send character immediately as a single byte */
        if (ctx->send_func) {
            char buf[4];
            int len = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, buf, sizeof(buf), NULL, NULL);
            if (len > 0) {
                ctx->send_func(buf, len, ctx->send_context);
            }
        }
    }
}

void Terminal_AddHistory(TerminalCtx* ctx, const wchar_t* cmd)
{
    if (!ctx || !cmd || cmd[0] == L'\0') return;

    if (ctx->history_count >= MAX_HISTORY) {
        /* Free oldest entry and shift left */
        free(ctx->history[0]);
        memmove(&ctx->history[0], &ctx->history[1],
                (MAX_HISTORY - 1) * sizeof(wchar_t*));
        ctx->history_count = MAX_HISTORY - 1;
    }

    ctx->history[ctx->history_count] = wcsdup(cmd);
    ctx->history_count++;
    ctx->history_index = -1;  /* Reset navigation */
}

void Terminal_SetSendFunc(TerminalCtx* ctx, TerminalSendFunc func, void* context)
{
    if (!ctx) return;
    ctx->send_func = func;
    ctx->send_context = context;
}
