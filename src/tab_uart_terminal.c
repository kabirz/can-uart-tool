/**
 * Tab 3: UART Shell Terminal Page
 *
 * Provides a Zephyr shell terminal with dark-themed RichEdit,
 * keyboard subclassing for Tab completion, arrow-key history, etc.
 */
#include <windows.h>
#include <richedit.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"
#include "uart_terminal.h"
#include "terminal_common.h"

/* ------------------------------------------------------------------ */
/*  Per-window instance data (stored via GWLP_USERDATA)                */
/* ------------------------------------------------------------------ */
typedef struct {
    UartTerminal *uartTerm;
    TerminalCtx  *termCtx;

    /* Child control handles */
    HWND hComboPort;
    HWND hComboBaud;
    HWND hBtnRefresh;
    HWND hBtnConnect;
    HWND hEditTerminal;
    HWND hBtnClear;

    /* Subclass */
    WNDPROC origRichEditProc;

    /* Fonts */
    HFONT hFont;
    HFONT hFontBold;
    HFONT hFontMono;

    int isConnected;
} TAB_UART_DATA;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */
static TAB_UART_DATA *GetTabPageData(HWND hwnd)
{
    return (TAB_UART_DATA *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
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

/* Populate the port ComboBox with enumerated COM ports */
static void RefreshPortList(TAB_UART_DATA *pData)
{
    char ports[64][32];
    int count = UartTerminal_EnumPorts(ports, 64);

    SendMessageW(pData->hComboPort, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < count; i++) {
        wchar_t wport[32];
        MultiByteToWideChar(CP_ACP, 0, ports[i], -1, wport, 32);
        SendMessageW(pData->hComboPort, CB_ADDSTRING, 0, (LPARAM)wport);
    }
    if (count > 0)
        SendMessageW(pData->hComboPort, CB_SETCURSEL, 0, 0);
}

/* Populate baud rate ComboBox */
static void InitBaudRates(HWND hCombo, HFONT hFont)
{
    static const DWORD bauds[] = {
        9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600
    };
    static const int defaultIndex = 4; /* 115200 */

    for (int i = 0; i < (int)(sizeof(bauds) / sizeof(bauds[0])); i++) {
        wchar_t buf[16];
        wsprintfW(buf, L"%u", bauds[i]);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessageW(hCombo, CB_SETCURSEL, defaultIndex, 0);
    SendMessageW(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
}

/* Get selected baud rate from ComboBox */
static DWORD GetSelectedBaud(HWND hCombo)
{
    wchar_t buf[16];
    int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return 115200;
    SendMessageW(hCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
    return (DWORD)wcstoul(buf, NULL, 10);
}

/* Send wrapper for TerminalCtx */
static void UartSendFunc(const char *data, int len, void *context)
{
    TAB_UART_DATA *pData = (TAB_UART_DATA *)context;
    if (pData && pData->uartTerm && pData->isConnected) {
        UartTerminal_Send(pData->uartTerm, data, len);
    }
}

/* UART receive callback (called from background thread) */
typedef struct {
    int   len;
    char  data[4096];
} UartRecvMsg;

static void UartTerm_OnRecv(const char *data, int len, void *context)
{
    HWND hwnd = (HWND)context;
    if (!hwnd || len <= 0) return;

    UartRecvMsg *msg = (UartRecvMsg *)malloc(sizeof(UartRecvMsg));
    if (!msg) return;
    msg->len = len > (int)sizeof(msg->data) ? (int)sizeof(msg->data) : len;
    memcpy(msg->data, data, msg->len);

    PostMessageW(hwnd, WM_UART_DATA_RECEIVED, 0, (LPARAM)msg);
}

/* ------------------------------------------------------------------ */
/*  RichEdit Subclass Proc                                            */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK RichEdit_SubclassProc(HWND hwnd, UINT uMsg,
                                               WPARAM wParam, LPARAM lParam,
                                               UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    TAB_UART_DATA *pData = (TAB_UART_DATA *)dwRefData;
    if (!pData || !pData->termCtx) {
        return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    }

    switch (uMsg) {
    case WM_KEYDOWN: {
        int handled = Terminal_HandleKeyDown(pData->termCtx, wParam, lParam);
        if (handled)
            return 0;
        break;
    }
    case WM_CHAR: {
        if (pData->termCtx->is_shell_mode) {
            Terminal_HandleChar(pData->termCtx, wParam);
            return 0; /* suppress default processing in shell mode */
        }
        break;
    }
    case WM_DESTROY:
        /* Remove subclass on destroy */
        RemoveWindowSubclass(hwnd, RichEdit_SubclassProc, uIdSubclass);
        break;
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  WndProc for the tab page                                          */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK TabUartTerminal_WndProc(HWND hwnd, UINT uMsg,
                                                  WPARAM wParam, LPARAM lParam)
{
    TAB_UART_DATA *pData = GetTabPageData(hwnd);

    switch (uMsg) {

    /* ---- Creation ---- */
    case WM_NCCREATE: {
        pData = (TAB_UART_DATA *)calloc(1, sizeof(TAB_UART_DATA));
        if (!pData) return FALSE;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pData);
        return TRUE;
    }

    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        HINSTANCE hInst = cs->hInstance;

        /* Recover UartTerminal pointer passed via lpCreateParams */
        pData->uartTerm = (UartTerminal *)cs->lpCreateParams;
        pData->isConnected = 0;

        /* Create fonts */
        pData->hFont = CreateFontW(
            24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");
        pData->hFontBold = CreateFontW(
            24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");
        pData->hFontMono = CreateFontW(
            20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
            L"Consolas");

        /* ---- Layout for 1200x1080 client area ---- */
        /* Tab page display area ~1172 x 1010 */
        int margin = 10;
        int cx, cy;

        /* ========== Connection Bar (top, full width) ========== */
        /* Height: 42px, y = 10, controls vertically centered */
        cy = margin;

        /* Port label + ComboBox (160px) */
        CreateLabel(hwnd, hInst, -1, margin, cy + 11, 40, 20, L"端口:", pData->hFont);
        pData->hComboPort = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | WS_VSCROLL,
            margin + 44, cy + 6, 160, 200,
            hwnd, (HMENU)IDC_COMBO_UART_PORT, hInst, NULL);
        SendMessageW(pData->hComboPort, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Baud rate label + ComboBox (140px) */
        CreateLabel(hwnd, hInst, -1, margin + 212, cy + 11, 50, 20,
            L"波特率:", pData->hFont);
        pData->hComboBaud = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            margin + 266, cy + 6, 140, 200,
            hwnd, (HMENU)IDC_COMBO_UART_TERM_BAUD, hInst, NULL);
        InitBaudRates(pData->hComboBaud, pData->hFont);

        /* Refresh button (90px) */
        pData->hBtnRefresh = CreateWindowExW(0, L"BUTTON",
            L"刷新",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            margin + 414, cy + 6, 90, 30,
            hwnd, NULL, hInst, NULL);
        SendMessageW(pData->hBtnRefresh, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Connect/Disconnect button (90px) */
        pData->hBtnConnect = CreateWindowExW(0, L"BUTTON",
            L"连接",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            margin + 514, cy + 6, 90, 30,
            hwnd, (HMENU)IDC_BUTTON_UART_CONNECT, hInst, NULL);
        SendMessageW(pData->hBtnConnect, WM_SETFONT, (WPARAM)pData->hFontBold, TRUE);

        /* Populate ports */
        RefreshPortList(pData);

        /* ========== Terminal Display (main area) ========== */
        /* (10, 56, 1152, 900) */
        int termX = margin;
        int termY = 56;
        int termW = 1152;
        int termH = 900;

        /* Load RichEdit module */
        static HMODULE hRichEdit = NULL;
        if (!hRichEdit)
            hRichEdit = LoadLibraryW(L"Msftedit.dll");

        pData->hEditTerminal = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            MSFTEDIT_CLASS,
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            termX, termY, termW, termH,
            hwnd, (HMENU)IDC_EDIT_UART_TERMINAL, hInst, NULL);

        /* Dark theme for RichEdit */
        if (pData->hEditTerminal) {
            SendMessageW(pData->hEditTerminal, EM_SETBKGNDCOLOR, 0, RGB(0x0C, 0x0C, 0x0C));

            CHARFORMATW cf = { 0 };
            cf.cbSize = sizeof(cf);
            cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
            cf.crTextColor = RGB(0xCC, 0xCC, 0xCC);
            cf.yHeight = 400; /* 20pt in twips */
            wcscpy(cf.szFaceName, L"Consolas");
            SendMessageW(pData->hEditTerminal, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

            /* Create TerminalCtx in shell mode */
            pData->termCtx = Terminal_Create(pData->hEditTerminal, 1);
            if (pData->termCtx) {
                Terminal_SetSendFunc(pData->termCtx, UartSendFunc, pData);
            }

            /* Subclass the RichEdit to capture keyboard input */
            SetWindowSubclass(pData->hEditTerminal, RichEdit_SubclassProc,
                              1, (DWORD_PTR)pData);
        }

        /* ========== Bottom Toolbar ========== */
        int tbY = 960;

        /* Clear button */
        pData->hBtnClear = CreateWindowExW(0, L"BUTTON",
            L"清除",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            margin, tbY, 70, 28,
            hwnd, (HMENU)IDC_BUTTON_UART_CLEAR, hInst, NULL);
        SendMessageW(pData->hBtnClear, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Set UART receive callback */
        UartTerminal_SetRecvCallback(pData->uartTerm, UartTerm_OnRecv, (void *)hwnd);

        return 0;
    }

    /* ---- Command handling ---- */
    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        /* Refresh button (no specific ID, match by hBtnRefresh) */
        default:
            if ((HWND)lParam == pData->hBtnRefresh) {
                RefreshPortList(pData);
                return 0;
            }
            break;

        case IDC_BUTTON_UART_CONNECT: {
            if (pData->isConnected) {
                /* Disconnect */
                UartTerminal_StopRecv(pData->uartTerm);
                UartTerminal_Disconnect(pData->uartTerm);
                pData->isConnected = 0;
                SetWindowTextW(pData->hBtnConnect, L"连接");
                EnableWindow(pData->hComboPort, TRUE);
                EnableWindow(pData->hComboBaud, TRUE);
                EnableWindow(pData->hBtnRefresh, TRUE);
                return 0;
            }

            /* Connect */
            wchar_t wport[32];
            int sel = (int)SendMessageW(pData->hComboPort, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) {
                MessageBoxW(hwnd, L"请选择串口",
                    L"错误", MB_OK | MB_ICONWARNING);
                return 0;
            }
            SendMessageW(pData->hComboPort, CB_GETLBTEXT, sel, (LPARAM)wport);

            char port[32];
            WideCharToMultiByte(CP_ACP, 0, wport, -1, port, 32, NULL, NULL);

            DWORD baud = GetSelectedBaud(pData->hComboBaud);

            int ret = UartTerminal_Connect(pData->uartTerm, port, baud);
            if (!ret) {
                MessageBoxW(hwnd,
                    L"连接失败，请检查端口和波特率",
                    L"错误", MB_OK | MB_ICONERROR);
                return 0;
            }

            pData->isConnected = 1;
            UartTerminal_StartRecv(pData->uartTerm);

            SetWindowTextW(pData->hBtnConnect, L"断开");
            EnableWindow(pData->hComboPort, FALSE);
            EnableWindow(pData->hComboBaud, FALSE);
            EnableWindow(pData->hBtnRefresh, FALSE);

            /* Set focus to terminal */
            SetFocus(pData->hEditTerminal);
            return 0;
        }

        case IDC_BUTTON_UART_CLEAR:
            if (pData->termCtx)
                Terminal_Clear(pData->termCtx);
            return 0;
        }
        break;

    /* ---- UART data received (from background thread via PostMessage) ---- */
    case WM_UART_DATA_RECEIVED: {
        UartRecvMsg *msg = (UartRecvMsg *)lParam;
        if (msg && pData->termCtx) {
            /* Null-terminate for Terminal_AppendText (expects UTF-8 string) */
            char *data = (char *)malloc(msg->len + 1);
            if (data) {
                memcpy(data, msg->data, msg->len);
                data[msg->len] = '\0';
                Terminal_AppendText(pData->termCtx, data);
                free(data);
            }
        }
        free(msg);
        return 0;
    }

    /* ---- Set focus to terminal when tab page receives focus ---- */
    case WM_SETFOCUS:
        if (pData && pData->hEditTerminal)
            SetFocus(pData->hEditTerminal);
        return 0;

    /* ---- Cleanup ---- */
    case WM_DESTROY:
        if (pData) {
            /* Disconnect if still connected */
            if (pData->isConnected) {
                UartTerminal_StopRecv(pData->uartTerm);
                UartTerminal_Disconnect(pData->uartTerm);
                pData->isConnected = 0;
            }
            UartTerminal_SetRecvCallback(pData->uartTerm, NULL, NULL);

            if (pData->termCtx) {
                Terminal_Destroy(pData->termCtx);
                pData->termCtx = NULL;
            }
            if (pData->hFont)     DeleteObject(pData->hFont);
            if (pData->hFontBold) DeleteObject(pData->hFontBold);
            if (pData->hFontMono) DeleteObject(pData->hFontMono);
            free(pData);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  Public API: Create / Destroy                                      */
/* ------------------------------------------------------------------ */

static const wchar_t *TAB_UART_CLASS = L"TabUartTerminalClass";
static int g_uartClassRegistered = 0;

HWND TabUartTerminal_Create(HWND hParent, HINSTANCE hInst, UartTerminal *uartTerm)
{
    if (!g_uartClassRegistered) {
        WNDCLASSEXW wc = { 0 };
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = TabUartTerminal_WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = TAB_UART_CLASS;
        RegisterClassExW(&wc);
        g_uartClassRegistered = 1;
    }

    RECT rcParent;
    GetClientRect(hParent, &rcParent);
    TabCtrl_AdjustRect(hParent, FALSE, &rcParent);

    HWND hwnd = CreateWindowExW(
        0,
        TAB_UART_CLASS,
        L"",
        WS_CHILD | WS_CLIPCHILDREN,
        rcParent.left, rcParent.top,
        rcParent.right - rcParent.left,
        rcParent.bottom - rcParent.top,
        hParent, NULL, hInst, uartTerm);

    return hwnd;
}

void TabUartTerminal_Destroy(HWND hwnd)
{
    if (hwnd)
        DestroyWindow(hwnd);
}
