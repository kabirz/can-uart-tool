#include "terminal_common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <richedit.h>

/* ------------------------------------------------------------------ */
/*  ANSI color table (standard 8 colors, dim + bright)                 */
/* ------------------------------------------------------------------ */
static const COLORREF ansi_colors[] = {
    RGB(0x00, 0x00, 0x00), /* 0: Black   */
    RGB(0xCC, 0x33, 0x33), /* 1: Red     */
    RGB(0x33, 0xCC, 0x33), /* 2: Green   */
    RGB(0xCC, 0xCC, 0x33), /* 3: Yellow  */
    RGB(0x33, 0x66, 0xCC), /* 4: Blue    */
    RGB(0xCC, 0x33, 0xCC), /* 5: Magenta */
    RGB(0x33, 0xCC, 0xCC), /* 6: Cyan    */
    RGB(0xCC, 0xCC, 0xCC), /* 7: White   */
    RGB(0x66, 0x66, 0x66), /* 8: Bright Black   */
    RGB(0xFF, 0x66, 0x66), /* 9: Bright Red     */
    RGB(0x66, 0xFF, 0x66), /* 10: Bright Green  */
    RGB(0xFF, 0xFF, 0x66), /* 11: Bright Yellow */
    RGB(0x66, 0x99, 0xFF), /* 12: Bright Blue   */
    RGB(0xFF, 0x66, 0xFF), /* 13: Bright Magenta*/
    RGB(0x66, 0xFF, 0xFF), /* 14: Bright Cyan   */
    RGB(0xFF, 0xFF, 0xFF), /* 15: Bright White  */
};

TerminalCtx* Terminal_Create(HWND hwnd, int is_shell_mode)
{
    TerminalCtx* ctx = (TerminalCtx*)malloc(sizeof(TerminalCtx));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(TerminalCtx));
    ctx->hwnd = hwnd;
    ctx->is_shell_mode = is_shell_mode;
    ctx->default_fg = RGB(0xF8, 0xF8, 0xF2);
    ctx->default_bg = RGB(0x28, 0x2A, 0x36);
    ctx->cur_fg = ctx->default_fg;
    ctx->cur_bg = ctx->default_bg;

    InitializeCriticalSection(&ctx->cs);

    return ctx;
}

void Terminal_Destroy(TerminalCtx* ctx)
{
    if (!ctx) return;

    DeleteCriticalSection(&ctx->cs);
    free(ctx);
}

/* Delete the last character from the RichEdit */
static void delete_last_char(TerminalCtx* ctx)
{
    HWND hwnd = ctx->hwnd;
    CHARRANGE cr;

    cr.cpMin = 0x7FFFFFFF;
    cr.cpMax = 0x7FFFFFFF;
    SendMessageW(hwnd, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessageW(hwnd, EM_EXGETSEL, 0, (LPARAM)&cr);

    if (cr.cpMin > 0) {
        cr.cpMin -= 1;
        SendMessageW(hwnd, EM_EXSETSEL, 0, (LPARAM)&cr);
        SendMessageW(hwnd, WM_CLEAR, 0, 0);
    }
}

/* Apply current ANSI color to a range of text in the RichEdit */
/* Parse one SGR parameter and update color state */
static void apply_sgr(TerminalCtx* ctx, int code)
{
    switch (code) {
    case 0:
        ctx->cur_fg = ctx->default_fg;
        ctx->cur_bg = ctx->default_bg;
        ctx->ansi_bold = 0;
        break;
    case 1:
        ctx->ansi_bold = 1;
        break;
    case 22:
        ctx->ansi_bold = 0;
        break;
    case 30: case 31: case 32: case 33:
    case 34: case 35: case 36: case 37:
        ctx->cur_fg = ansi_colors[code - 30];
        break;
    case 38:
        /* 256-color / truecolor: just skip, keep current color */
        break;
    case 39:
        ctx->cur_fg = ctx->default_fg;
        break;
    case 40: case 41: case 42: case 43:
    case 44: case 45: case 46: case 47:
        ctx->cur_bg = ansi_colors[code - 40];
        break;
    case 49:
        ctx->cur_bg = ctx->default_bg;
        break;
    case 90: case 91: case 92: case 93:
    case 94: case 95: case 96: case 97:
        ctx->cur_fg = ansi_colors[code - 90 + 8];
        break;
    case 100: case 101: case 102: case 103:
    case 104: case 105: case 106: case 107:
        ctx->cur_bg = ansi_colors[code - 100 + 8];
        break;
    }
}

void Terminal_AppendText(TerminalCtx* ctx, const char* utf8_text)
{
    if (!ctx || !utf8_text) return;

    int slen = (int)strlen(utf8_text);
    if (slen == 0) return;

    EnterCriticalSection(&ctx->cs);

    /* Prepend any buffered incomplete ANSI sequence */
    char *data;
    int dlen;
    if (ctx->ansi_buf_len > 0) {
        dlen = ctx->ansi_buf_len + slen;
        data = (char *)malloc(dlen + 1);
        if (!data) {
            LeaveCriticalSection(&ctx->cs);
            return;
        }
        memcpy(data, ctx->ansi_buf, ctx->ansi_buf_len);
        memcpy(data + ctx->ansi_buf_len, utf8_text, slen);
        data[dlen] = '\0';
        ctx->ansi_buf_len = 0;
    } else {
        data = (char *)utf8_text;
        dlen = slen;
    }

    HWND hwnd = ctx->hwnd;
    int i = 0;
    while (i < dlen) {
        if (data[i] == '\x1b') {
            /* Look for CSI: ESC [ */
            if (i + 1 < dlen && data[i + 1] == '[') {
                int j = i + 2;
                int params[16];
                int nparam = 0;

                while (j < dlen && data[j] >= '0' && data[j] <= '9') {
                    int val = 0;
                    while (j < dlen && data[j] >= '0' && data[j] <= '9') {
                        val = val * 10 + (data[j] - '0');
                        j++;
                    }
                    if (nparam < 16) params[nparam++] = val;
                    if (j < dlen && data[j] == ';') j++;
                }

                if (j < dlen && data[j] >= 0x40 && data[j] <= 0x7E) {
                    /* Complete CSI sequence found */
                    if (data[j] == 'm') {
                        if (nparam == 0)
                            apply_sgr(ctx, 0);
                        else
                            for (int k = 0; k < nparam; k++)
                                apply_sgr(ctx, params[k]);
                    } else if (data[j] == 'C') {
                        /* CUF – Cursor Forward: insert spaces */
                        int count = (nparam > 0 && params[0] > 0) ? params[0] : 1;
                        if (count > 128) count = 128;
                        if (count > 0) {
                            wchar_t spaces[129];
                            for (int s = 0; s < count; s++) spaces[s] = L' ';
                            spaces[count] = L'\0';
                            int pos = GetWindowTextLengthW(hwnd);
                            SendMessageW(hwnd, EM_SETSEL, (WPARAM)pos, (LPARAM)pos);
                            CHARFORMATW cfc = { 0 };
                            cfc.cbSize = sizeof(cfc);
                            cfc.dwMask = CFM_COLOR | CFM_BOLD | CFM_SIZE | CFM_FACE;
                            cfc.crTextColor = ctx->cur_fg;
                            cfc.yHeight = 260;
                            wcscpy(cfc.szFaceName, L"Consolas");
                            if (ctx->ansi_bold) cfc.dwEffects = CFE_BOLD;
                            SendMessageW(hwnd, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfc);
                            SendMessageW(hwnd, EM_REPLACESEL, FALSE, (LPARAM)spaces);
                        }
                    } else if (data[j] == 'D') {
                        /* CUB – Cursor Back: delete characters */
                        int count = (nparam > 0 && params[0] > 0) ? params[0] : 1;
                        for (int d = 0; d < count; d++)
                            delete_last_char(ctx);
                    }
                    /* 'J' (ED – Erase Display) and others: silently consume */
                    j++;
                    i = j;
                    continue;
                } else if (j >= dlen) {
                    /* Incomplete: buffer from ESC to end */
                    int remain = dlen - i;
                    if (remain < (int)sizeof(ctx->ansi_buf)) {
                        memcpy(ctx->ansi_buf, data + i, remain);
                        ctx->ansi_buf_len = remain;
                    }
                    i = dlen;
                    continue;
                }
                /* Unknown state, skip ESC and continue */
                i++;
                continue;
            } else if (i + 1 >= dlen) {
                /* Lone ESC at end, buffer it */
                ctx->ansi_buf[0] = '\x1b';
                ctx->ansi_buf_len = 1;
                i = dlen;
                continue;
            }
            /* ESC followed by something other than [, skip ESC */
            i++;
            continue;
        }

        /* Handle backspace from device */
        if (data[i] == '\b') {
            delete_last_char(ctx);
            i++;
            continue;
        }

        /* Collect a run of plain text */
        int run_start = i;
        while (i < dlen && data[i] != '\x1b' && data[i] != '\b')
            i++;

        int run_len = i - run_start;
        if (run_len > 0) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, data + run_start, run_len, NULL, 0);
            if (wlen > 0) {
                wchar_t* wbuf = (wchar_t*)malloc((wlen + 1) * sizeof(wchar_t));
                if (wbuf) {
                    MultiByteToWideChar(CP_UTF8, 0, data + run_start, run_len, wbuf, wlen);
                    wbuf[wlen] = L'\0';

                    /* Expand tabs to 4 spaces for readable alignment */
                    {
                        int tab_count = 0;
                        for (int ti = 0; ti < wlen; ti++) {
                            if (wbuf[ti] == L'\t') tab_count++;
                        }
                        if (tab_count > 0) {
                            int new_len = wlen + tab_count * 3;
                            wchar_t *expanded = (wchar_t*)malloc((new_len + 1) * sizeof(wchar_t));
                            if (expanded) {
                                int di = 0;
                                for (int ti = 0; ti < wlen; ti++) {
                                    if (wbuf[ti] == L'\t') {
                                        expanded[di++] = L' ';
                                        expanded[di++] = L' ';
                                        expanded[di++] = L' ';
                                        expanded[di++] = L' ';
                                    } else {
                                        expanded[di++] = wbuf[ti];
                                    }
                                }
                                expanded[di] = L'\0';
                                free(wbuf);
                                wbuf = expanded;
                                wlen = di;
                            }
                        }
                    }

                    int pos = GetWindowTextLengthW(hwnd);
                    SendMessageW(hwnd, EM_SETSEL, (WPARAM)pos, (LPARAM)pos);

                    /* Set insertion format (color + font) before inserting text */
                    CHARFORMATW cf = { 0 };
                    cf.cbSize = sizeof(cf);
                    cf.dwMask = CFM_COLOR | CFM_BOLD | CFM_SIZE | CFM_FACE;
                    cf.crTextColor = ctx->cur_fg;
                    cf.yHeight = 260;
                    wcscpy(cf.szFaceName, L"Consolas");
                    if (ctx->ansi_bold) cf.dwEffects = CFE_BOLD;
                    SendMessageW(hwnd, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

                    SendMessageW(hwnd, EM_REPLACESEL, FALSE, (LPARAM)wbuf);
                    free(wbuf);
                }
            }
        }
    }

    SendMessageW(hwnd, EM_SCROLLCARET, 0, 0);

    if (data != utf8_text)
        free(data);

    LeaveCriticalSection(&ctx->cs);
}

void Terminal_Clear(TerminalCtx* ctx)
{
    if (!ctx) return;
    SetWindowTextW(ctx->hwnd, L"");
}

int Terminal_HandleKeyDown(TerminalCtx* ctx, WPARAM wParam, LPARAM lParam)
{
    if (!ctx || !ctx->send_func) return 0;

    /* Pure passthrough: send all special keys as raw bytes */
    switch (wParam) {
    case VK_RETURN:
        ctx->send_func("\r", 1, ctx->send_context);
        return 1;

    case VK_BACK:
        ctx->send_func("\x08", 1, ctx->send_context);
        return 1;

    case VK_TAB:
        ctx->send_func("\t", 1, ctx->send_context);
        return 1;

    case VK_UP:
        ctx->send_func("\x1b[A", 3, ctx->send_context);
        return 1;

    case VK_DOWN:
        ctx->send_func("\x1b[B", 3, ctx->send_context);
        return 1;

    case VK_RIGHT:
        ctx->send_func("\x1b[C", 3, ctx->send_context);
        return 1;

    case VK_LEFT:
        ctx->send_func("\x1b[D", 3, ctx->send_context);
        return 1;

    case VK_HOME:
        ctx->send_func("\x1b[H", 3, ctx->send_context);
        return 1;

    case VK_END:
        ctx->send_func("\x1b[F", 3, ctx->send_context);
        return 1;

    case VK_DELETE:
        ctx->send_func("\x1b[3~", 4, ctx->send_context);
        return 1;

    default:
        return 0;
    }
}

void Terminal_HandleChar(TerminalCtx* ctx, WPARAM wParam)
{
    if (!ctx || !ctx->send_func) return;

    wchar_t ch = (wchar_t)wParam;

    /* Skip control characters (< 0x20) — already handled by HandleKeyDown */
    if (ch < 0x20) return;

    char buf[4];
    int len = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, buf, sizeof(buf), NULL, NULL);
    if (len > 0) {
        ctx->send_func(buf, len, ctx->send_context);
    }
}

void Terminal_SetSendFunc(TerminalCtx* ctx, TerminalSendFunc func, void* context)
{
    if (!ctx) return;
    ctx->send_func = func;
    ctx->send_context = context;
}
