/**
 * Tab 4: TCP/UDP Network Terminal Page
 *
 * Raw passthrough mode network terminal with dark-themed RichEdit.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"
#include "net_terminal.h"
#include "terminal_common.h"

/* Custom message for TCP disconnect notification */
#ifndef WM_NET_DISCONNECTED
#define WM_NET_DISCONNECTED (WM_APP + 10)
#endif

/* ------------------------------------------------------------------ */
/*  Per-window instance data (stored via GWLP_USERDATA)                */
/* ------------------------------------------------------------------ */
typedef struct {
    NetTerminal  *netTerm;
    TerminalCtx  *termCtx;
    HWND          hTabHwnd;      /* This tab page window */

    /* Child controls */
    HWND hRadioTcp;
    HWND hRadioUdp;
    HWND hEditHost;
    HWND hEditPort;
    HWND hBtnConnect;
    HWND hEditTerminal;
    HWND hBtnClear;
    HWND hLabelStatus;

    /* RichEdit subclass original WndProc */
    WNDPROC pfnOrigRichEditProc;

    /* Fonts */
    HFONT hFont;
    HFONT hFontMono;
} TAB_NET_DATA;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */
static TAB_NET_DATA *GetTabPageData(HWND hwnd)
{
    return (TAB_NET_DATA *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

/* Create a static label */
static HWND CreateLabel(HWND hParent, HINSTANCE hInst, int id,
                         int x, int y, int w, int h,
                         const wchar_t *text, HFONT hFont)
{
    HWND hw = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

/* Update UI to reflect connection state */
static void UpdateConnectionUI(TAB_NET_DATA *pData, int connected)
{
    if (connected) {
        SetWindowTextW(pData->hBtnConnect, L"\x65AD\x5F00");  /* "断开" */
        EnableWindow(pData->hRadioTcp, FALSE);
        EnableWindow(pData->hRadioUdp, FALSE);
        EnableWindow(pData->hEditHost, FALSE);
        EnableWindow(pData->hEditPort, FALSE);
        SetWindowTextW(pData->hLabelStatus, L"\x5DF2\x8FDE\x63A5 - Raw \x900F\x4F20\x6A21\x5F0F");
    } else {
        SetWindowTextW(pData->hBtnConnect, L"\x8FDE\x63A5");  /* "连接" */
        EnableWindow(pData->hRadioTcp, TRUE);
        EnableWindow(pData->hRadioUdp, TRUE);
        EnableWindow(pData->hEditHost, TRUE);
        EnableWindow(pData->hEditPort, TRUE);
        SetWindowTextW(pData->hLabelStatus, L"Raw \x900F\x4F20\x6A21\x5F0F");
    }
}

/* ------------------------------------------------------------------ */
/*  RichEdit subclass procedure                                       */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK RichEditSubclassProc(HWND hwnd, UINT uMsg,
                                              WPARAM wParam, LPARAM lParam,
                                              UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    TAB_NET_DATA *pData = (TAB_NET_DATA *)dwRefData;
    if (!pData || !pData->termCtx) {
        return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    }

    switch (uMsg) {
    case WM_KEYDOWN:
        if (Terminal_HandleKeyDown(pData->termCtx, wParam, lParam)) {
            return 0;  /* Key handled */
        }
        break;

    case WM_CHAR:
        Terminal_HandleChar(pData->termCtx, wParam);
        /* In raw mode, characters are sent immediately via send_func.
           Don't let RichEdit process the char (we manage display via AppendText). */
        return 0;

    case WM_PASTE: {
        /* Handle paste: send clipboard text through the terminal */
        if (OpenClipboard(hwnd)) {
            HANDLE hClip = GetClipboardData(CF_UNICODETEXT);
            if (hClip) {
                wchar_t *wtext = (wchar_t *)GlobalLock(hClip);
                if (wtext) {
                    /* Convert to UTF-8 and send */
                    int ulen = WideCharToMultiByte(CP_UTF8, 0, wtext, -1,
                                                    NULL, 0, NULL, NULL);
                    if (ulen > 0) {
                        char *ubuf = (char *)malloc(ulen);
                        if (ubuf) {
                            WideCharToMultiByte(CP_UTF8, 0, wtext, -1,
                                                ubuf, ulen, NULL, NULL);
                            /* ulen includes null terminator from -1, send ulen-1 */
                            if (pData->termCtx->send_func) {
                                pData->termCtx->send_func(ubuf, ulen - 1,
                                                           pData->termCtx->send_context);
                            }
                            free(ubuf);
                        }
                    }
                    GlobalUnlock(hClip);
                }
            }
            CloseClipboard();
        }
        return 0;
    }

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, RichEditSubclassProc, uIdSubclass);
        break;
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  Send function wrapper: called by TerminalCtx to send data          */
/* ------------------------------------------------------------------ */
static void NetSendFunc(const char *data, int len, void *context)
{
    TAB_NET_DATA *pData = (TAB_NET_DATA *)context;
    if (!pData || !pData->netTerm) return;
    NetTerminal_Send(pData->netTerm, data, len);
}

/* ------------------------------------------------------------------ */
/*  Receive callback: called from NetTerminal recv thread              */
/* ------------------------------------------------------------------ */
static void NetTerm_OnRecv(const char *data, int len, void *context)
{
    HWND hwnd = (HWND)context;
    if (!hwnd) return;

    if (data == NULL || len == 0) {
        /* TCP disconnect signal */
        PostMessage(hwnd, WM_NET_DISCONNECTED, 0, 0);
        return;
    }

    /* Allocate a copy of the data to send via PostMessage */
    char *copy = (char *)malloc(len + 1);
    if (!copy) return;
    memcpy(copy, data, len);
    copy[len] = '\0';

    PostMessage(hwnd, WM_NET_DATA_RECEIVED, (WPARAM)len, (LPARAM)copy);
}

/* ------------------------------------------------------------------ */
/*  WndProc for the tab page                                          */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK TabNetTerminal_WndProc(HWND hwnd, UINT uMsg,
                                                WPARAM wParam, LPARAM lParam)
{
    TAB_NET_DATA *pData = GetTabPageData(hwnd);

    switch (uMsg) {

    /* ---- Creation ---- */
    case WM_NCCREATE: {
        pData = (TAB_NET_DATA *)calloc(1, sizeof(TAB_NET_DATA));
        if (!pData) return FALSE;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pData);
        return TRUE;
    }

    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        HINSTANCE hInst = cs->hInstance;

        /* Recover the NetTerminal pointer passed via lpCreateParams */
        pData->netTerm = (NetTerminal *)cs->lpCreateParams;
        pData->hTabHwnd = hwnd;

        /* Create fonts */
        pData->hFont = CreateFontW(
            14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");
        pData->hFontMono = CreateFontW(
            16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
            L"Consolas");

        /* ---- Layout coordinates ---- */
        int margin = 12;
        int lineH = 26;

        /* ========== Connection Bar (top) ========== */
        int barY = margin;

        /* Protocol label */
        CreateLabel(hwnd, hInst, -1, margin, barY + 3, 44, 20,
                    L"\x534F\x8BAE:", pData->hFont);

        /* TCP radio button (default) */
        pData->hRadioTcp = CreateWindowExW(0, L"BUTTON", L"TCP",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            margin + 48, barY, 60, 22,
            hwnd, (HMENU)IDC_RADIO_TCP, hInst, NULL);
        SendMessageW(pData->hRadioTcp, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        SendMessageW(pData->hRadioTcp, BM_SETCHECK, BST_CHECKED, 0);

        /* UDP radio button */
        pData->hRadioUdp = CreateWindowExW(0, L"BUTTON", L"UDP",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            margin + 112, barY, 60, 22,
            hwnd, (HMENU)IDC_RADIO_UDP, hInst, NULL);
        SendMessageW(pData->hRadioUdp, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Host label */
        CreateLabel(hwnd, hInst, -1, margin + 176, barY + 3, 36, 20,
                    L"\x4E3B\x673A:", pData->hFont);

        /* Host edit */
        pData->hEditHost = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            L"192.168.1.100",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            margin + 216, barY, 160, 22,
            hwnd, (HMENU)IDC_EDIT_NET_HOST, hInst, NULL);
        SendMessageW(pData->hEditHost, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Port label */
        CreateLabel(hwnd, hInst, -1, margin + 384, barY + 3, 36, 20,
                    L"\x7AEF\x53E3:", pData->hFont);

        /* Port edit */
        pData->hEditPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            L"23",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
            margin + 424, barY, 60, 22,
            hwnd, (HMENU)IDC_EDIT_NET_PORT, hInst, NULL);
        SendMessageW(pData->hEditPort, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Connect button */
        pData->hBtnConnect = CreateWindowExW(0, L"BUTTON",
            L"\x8FDE\x63A5",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            margin + 496, barY, 80, 24,
            hwnd, (HMENU)IDC_BUTTON_NET_CONNECT, hInst, NULL);
        SendMessageW(pData->hBtnConnect, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* ========== Terminal Display (main area) ========== */
        int termX = margin;
        int termY = barY + lineH + 6;

        /* Get the available width/height for the terminal */
        RECT rcParent;
        GetClientRect(GetParent(hwnd), &rcParent);
        TabCtrl_AdjustRect(GetParent(hwnd), FALSE, &rcParent);
        int availW = rcParent.right - rcParent.left - 2 * margin;
        int availH = rcParent.bottom - rcParent.top - termY - 36 - margin;

        /* Load RichEdit library */
        static HMODULE hRichEdit = NULL;
        if (!hRichEdit) {
            hRichEdit = LoadLibraryW(L"Msftedit.dll");
        }

        pData->hEditTerminal = CreateWindowExW(WS_EX_CLIENTEDGE,
            MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            termX, termY, availW, availH,
            hwnd, (HMENU)IDC_EDIT_NET_TERMINAL, hInst, NULL);

        /* Dark theme for RichEdit */
        SendMessageW(pData->hEditTerminal, EM_SETBKGNDCOLOR, 0, RGB(0x0C, 0x0C, 0x0C));
        CHARFORMATW cf = {0};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
        cf.crTextColor = RGB(0xCC, 0xCC, 0xCC);
        cf.yHeight = 240;  /* 12pt = 240 twips */
        wcscpy(cf.szFaceName, L"Consolas");
        SendMessageW(pData->hEditTerminal, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
        SendMessageW(pData->hEditTerminal, EM_SETEVENTMASK, 0, ENM_KEYEVENTS);

        /* Subclass the RichEdit for keyboard handling */
        SetWindowSubclass(pData->hEditTerminal, RichEditSubclassProc,
                          1, (DWORD_PTR)pData);

        /* Create TerminalCtx in raw mode (is_shell_mode=0) */
        pData->termCtx = Terminal_Create(pData->hEditTerminal, 0);
        Terminal_SetSendFunc(pData->termCtx, NetSendFunc, pData);

        /* ========== Bottom Toolbar ========== */
        int tbY = termY + availH + 6;

        /* Clear button */
        pData->hBtnClear = CreateWindowExW(0, L"BUTTON",
            L"\x6E05\x9664",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            margin, tbY, 70, 24,
            hwnd, (HMENU)IDC_BUTTON_NET_CLEAR, hInst, NULL);
        SendMessageW(pData->hBtnClear, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Status label */
        pData->hLabelStatus = CreateWindowExW(0, L"STATIC",
            L"Raw \x900F\x4F20\x6A21\x5F0F",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            margin + 80, tbY + 3, 200, 20,
            hwnd, NULL, hInst, NULL);
        SendMessageW(pData->hLabelStatus, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Set up receive callback */
        NetTerminal_SetRecvCallback(pData->netTerm, NetTerm_OnRecv, (void *)hwnd);

        /* Initial UI state */
        UpdateConnectionUI(pData, 0);

        return 0;
    }

    /* ---- WM_SIZE: resize child controls ---- */
    case WM_SIZE: {
        if (!pData) break;

        int cxClient = LOWORD(lParam);
        int cyClient = HIWORD(lParam);
        int margin = 12;
        int lineH = 26;
        int barH = lineH + 6;
        int tbH = 36;

        int termX = margin;
        int termY = barH + margin;
        int termW = cxClient - 2 * margin;
        int termH = cyClient - termY - tbH;

        if (termW < 100) termW = 100;
        if (termH < 50) termH = 50;

        if (pData->hEditTerminal) {
            MoveWindow(pData->hEditTerminal, termX, termY, termW, termH, TRUE);
        }

        int tbY = termY + termH + 6;
        if (pData->hBtnClear) {
            MoveWindow(pData->hBtnClear, margin, tbY, 70, 24, TRUE);
        }
        if (pData->hLabelStatus) {
            MoveWindow(pData->hLabelStatus, margin + 80, tbY + 3, 300, 20, TRUE);
        }
        return 0;
    }

    /* ---- Command handling ---- */
    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case IDC_BUTTON_NET_CONNECT: {
            if (!pData->netTerm) return 0;

            if (NetTerminal_IsConnected(pData->netTerm)) {
                /* Disconnect */
                NetTerminal_Disconnect(pData->netTerm);
                Terminal_AppendText(pData->termCtx, "\r\n[已断开连接]\r\n");
                UpdateConnectionUI(pData, 0);
            } else {
                /* Read settings from controls */
                char host[256] = {0};
                wchar_t whost[256] = {0};
                GetWindowTextW(pData->hEditHost, whost, 256);
                WideCharToMultiByte(CP_UTF8, 0, whost, -1, host, sizeof(host), NULL, NULL);

                wchar_t wport[16] = {0};
                GetWindowTextW(pData->hEditPort, wport, 16);
                int port = _wtoi(wport);

                int use_tcp = (SendMessageW(pData->hRadioTcp, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;

                /* Connect */
                int result = NetTerminal_Connect(pData->netTerm, host, port, use_tcp);
                if (result) {
                    const char *proto = use_tcp ? "TCP" : "UDP";
                    char msg[512];
                    snprintf(msg, sizeof(msg),
                             "\r\n[连接成功] %s %s:%d\r\n",
                             proto, host, port);
                    Terminal_AppendText(pData->termCtx, msg);
                    UpdateConnectionUI(pData, 1);
                } else {
                    char msg[512];
                    snprintf(msg, sizeof(msg),
                             "\r\n[连接失败] %s:%d\r\n",
                             host, port);
                    Terminal_AppendText(pData->termCtx, msg);
                }
            }
            return 0;
        }

        case IDC_BUTTON_NET_CLEAR: {
            if (pData->termCtx) {
                Terminal_Clear(pData->termCtx);
            }
            return 0;
        }

        default:
            break;
        }
        break;

    /* ---- Network data received (from recv thread via PostMessage) ---- */
    case WM_NET_DATA_RECEIVED: {
        int len = (int)wParam;
        char *data = (char *)lParam;
        if (data && pData->termCtx) {
            data[len] = '\0';
            Terminal_AppendText(pData->termCtx, data);
        }
        free(data);
        return 0;
    }

    /* ---- TCP disconnect notification ---- */
    case WM_NET_DISCONNECTED: {
        if (pData->termCtx) {
            Terminal_AppendText(pData->termCtx,
                "\r\n[连接已断开]\r\n");
        }
        UpdateConnectionUI(pData, 0);
        return 0;
    }

    /* ---- Cleanup ---- */
    case WM_DESTROY: {
        if (pData) {
            /* Disconnect if still connected */
            if (pData->netTerm && NetTerminal_IsConnected(pData->netTerm)) {
                NetTerminal_Disconnect(pData->netTerm);
            }

            /* Remove RichEdit subclass */
            if (pData->hEditTerminal) {
                RemoveWindowSubclass(pData->hEditTerminal, RichEditSubclassProc, 1);
            }

            /* Destroy terminal context */
            if (pData->termCtx) {
                Terminal_Destroy(pData->termCtx);
                pData->termCtx = NULL;
            }

            /* Clean up receive callback */
            if (pData->netTerm) {
                NetTerminal_SetRecvCallback(pData->netTerm, NULL, NULL);
            }

            /* Delete fonts */
            if (pData->hFont)     DeleteObject(pData->hFont);
            if (pData->hFontMono) DeleteObject(pData->hFontMono);

            free(pData);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  Public API: Create / Destroy                                      */
/* ------------------------------------------------------------------ */

static const wchar_t *TAB_NET_CLASS = L"TabNetTerminalClass";
static int g_netClassRegistered = 0;

HWND TabNetTerminal_Create(HWND hParent, HINSTANCE hInst, NetTerminal *netTerm)
{
    if (!g_netClassRegistered) {
        WNDCLASSEXW wc = { 0 };
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = TabNetTerminal_WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = TAB_NET_CLASS;
        RegisterClassExW(&wc);
        g_netClassRegistered = 1;
    }

    RECT rcParent;
    GetClientRect(hParent, &rcParent);
    TabCtrl_AdjustRect(hParent, FALSE, &rcParent);

    HWND hwnd = CreateWindowExW(
        0,
        TAB_NET_CLASS,
        L"",
        WS_CHILD | WS_CLIPCHILDREN,
        rcParent.left, rcParent.top,
        rcParent.right - rcParent.left,
        rcParent.bottom - rcParent.top,
        hParent, NULL, hInst, netTerm);

    return hwnd;
}

void TabNetTerminal_Destroy(HWND hwnd)
{
    if (hwnd)
        DestroyWindow(hwnd);
}
