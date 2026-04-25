/**
 * CAN/UART Tool - Main Window with Tab Control
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include "resource.h"
#include "can_manager.h"
#include "uart_manager.h"
#include "can_command.h"
#include "uart_terminal.h"
#include "net_terminal.h"

/* Forward declarations for tab page creation */
extern HWND TabCanUpgrade_Create(HWND hParent, HINSTANCE hInst, CanManager *can_mgr, UartManager *uart_mgr, HWND hNotifyWnd);
extern void TabCanUpgrade_Destroy(HWND hwnd);
extern HWND TabCanCommand_Create(HWND hParent, HINSTANCE hInst, CanCommand *cmd);
extern void TabCanCommand_Destroy(HWND hwnd);
extern void TabCanCommand_UpdateChannel(HWND hwnd, TPCANHandle channel);
extern HWND TabUartTerminal_Create(HWND hParent, HINSTANCE hInst, UartTerminal *uartTerm);
extern void TabUartTerminal_Destroy(HWND hwnd);
extern HWND TabNetTerminal_Create(HWND hParent, HINSTANCE hInst, NetTerminal *netTerm);
extern void TabNetTerminal_Destroy(HWND hwnd);

#define MAX_TABS 4

static const wchar_t *g_TabNames[MAX_TABS] = {
    L"CAN/UART 升级",
    L"CAN 命令",
    L"UART 终端",
    L"网络终端"
};

typedef struct {
    HWND  hTabCtrl;
    HWND  hStatusBar;
    HWND  hTabPages[MAX_TABS];
    HFONT hFont;
    HFONT hTabFont;
    CanManager  *canMgr;
    UartManager *uartMgr;
    CanCommand  *canCmd;
    UartTerminal *uartTerm;
    NetTerminal  *netTerm;
} APP_DATA;

static APP_DATA g_App;

/* Calculate window rect from desired client rect */
static void CalcWindowRectFromClient(DWORD style, DWORD exStyle, int cx, int cy, RECT *prc)
{
    prc->left   = 0;
    prc->top    = 0;
    prc->right  = cx;
    prc->bottom = cy;
    /* Account for menu height */
    AdjustWindowRectEx(prc, style, TRUE, exStyle);
    /* Approximate menu bar height: add 26 pixels */
    prc->bottom += 26;
}

/* Create tab pages */
static void CreateTabPages(HWND hTabCtrl, HWND hMainWnd, HINSTANCE hInst)
{
    RECT rc;
    int i;

    /* Get tab control display area */
    GetClientRect(hTabCtrl, &rc);
    TabCtrl_AdjustRect(hTabCtrl, FALSE, &rc);

    /* Tab 0: CAN/UART Firmware Upgrade (real page) */
    g_App.hTabPages[0] = TabCanUpgrade_Create(hTabCtrl, hInst,
                                               g_App.canMgr, g_App.uartMgr,
                                               hMainWnd);

    /* Tab 1: CAN Command page */
    g_App.hTabPages[1] = TabCanCommand_Create(hTabCtrl, hInst, g_App.canCmd);

    /* Tab 2: UART Shell Terminal */
    g_App.hTabPages[2] = TabUartTerminal_Create(hTabCtrl, hInst, g_App.uartTerm);

    /* Tab 3: TCP/UDP Network Terminal */
    g_App.hTabPages[3] = TabNetTerminal_Create(hTabCtrl, hInst, g_App.netTerm);

    /* Show only the first tab page */
    ShowWindow(g_App.hTabPages[0], SW_SHOW);
    for (i = 1; i < MAX_TABS; i++) {
        ShowWindow(g_App.hTabPages[i], SW_HIDE);
    }
}

/* Resize tab control and status bar */
static void OnSize(HWND hWnd)
{
    RECT rcMain;
    int  cx, cy;

    GetClientRect(hWnd, &rcMain);
    cx = rcMain.right;
    cy = rcMain.bottom;

    /* Resize status bar */
    SendMessageW(g_App.hStatusBar, WM_SIZE, 0, 0);

    /* Get status bar height */
    RECT rcStatus;
    GetWindowRect(g_App.hStatusBar, &rcStatus);
    int statusH = rcStatus.bottom - rcStatus.top;

    /* Resize tab control to fill area above status bar */
    MoveWindow(g_App.hTabCtrl, 0, 0, cx, cy - statusH, TRUE);

    /* Reposition tab page content */
    RECT rcTab;
    GetClientRect(g_App.hTabCtrl, &rcTab);
    TabCtrl_AdjustRect(g_App.hTabCtrl, FALSE, &rcTab);

    int i;
    for (i = 0; i < MAX_TABS; i++) {
        if (g_App.hTabPages[i]) {
            MoveWindow(g_App.hTabPages[i],
                       rcTab.left, rcTab.top,
                       rcTab.right - rcTab.left,
                       rcTab.bottom - rcTab.top,
                       TRUE);
        }
    }
}

/* Window procedure */
static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {

    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        HINSTANCE hInst = cs->hInstance;
        int i;

        /* Create Tab Control */
        g_App.hTabCtrl = CreateWindowExW(
            0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
            0, 0, 0, 0,
            hWnd, (HMENU)IDC_TAB_CONTROL, hInst, NULL);

        /* Set tab control font */
        g_App.hTabFont = CreateFontW(
            14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");
        SendMessageW(g_App.hTabCtrl, WM_SETFONT, (WPARAM)g_App.hTabFont, TRUE);

        /* Insert tabs */
        TCITEMW tie;
        tie.mask = TCIF_TEXT;
        for (i = 0; i < MAX_TABS; i++) {
            tie.pszText = (LPWSTR)g_TabNames[i];
            TabCtrl_InsertItem(g_App.hTabCtrl, i, &tie);
        }

        /* Create status bar */
        g_App.hStatusBar = CreateWindowExW(
            0, STATUSCLASSNAMEW, NULL,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0,
            hWnd, (HMENU)IDC_STATIC, hInst, NULL);
        SendMessageW(g_App.hStatusBar, SB_SETTEXTW, 0, (LPARAM)L"就绪");

        /* Create UI font for tab pages */
        g_App.hFont = CreateFontW(
            14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");

        /* Create tab pages */
        CreateTabPages(g_App.hTabCtrl, hWnd, hInst);

        return 0;
    }

    case WM_SIZE:
        OnSize(hWnd);
        return 0;

    case WM_NOTIFY: {
        NMHDR *pnm = (NMHDR *)lParam;
        if (pnm->idFrom == IDC_TAB_CONTROL && pnm->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(g_App.hTabCtrl);
            int i;
            for (i = 0; i < MAX_TABS; i++) {
                ShowWindow(g_App.hTabPages[i], (i == sel) ? SW_SHOW : SW_HIDE);
            }
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FILE_EXIT:
            DestroyWindow(hWnd);
            return 0;
        case IDM_HELP_ABOUT:
            MessageBoxW(hWnd,
                        L"CAN/UART 工具 v1.0\n\n"
                        L"基于 PCAN 的 CAN/UART 调试工具",
                        L"关于",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;

    /* CAN 连接共享：Tab0 通知 Tab1 更新 CAN 通道 */
    case WM_CAN_CONNECTED:
        if (g_App.hTabPages[1]) {
            TabCanCommand_UpdateChannel(g_App.hTabPages[1], (TPCANHandle)wParam);
            SendMessageW(g_App.hStatusBar, SB_SETTEXTW, 0,
                         (LPARAM)L"CAN 已连接 - 通道已同步到 CAN 命令页");
        }
        return 0;

    case WM_CAN_DISCONNECTED:
        if (g_App.hTabPages[1]) {
            TabCanCommand_UpdateChannel(g_App.hTabPages[1], PCAN_NONEBUS);
            SendMessageW(g_App.hStatusBar, SB_SETTEXTW, 0,
                         (LPARAM)L"CAN 已断开");
        }
        return 0;

    case WM_DESTROY:
        /* Destroy tab pages explicitly */
        TabCanUpgrade_Destroy(g_App.hTabPages[0]);
        g_App.hTabPages[0] = NULL;
        TabCanCommand_Destroy(g_App.hTabPages[1]);
        g_App.hTabPages[1] = NULL;
        TabUartTerminal_Destroy(g_App.hTabPages[2]);
        g_App.hTabPages[2] = NULL;
        TabNetTerminal_Destroy(g_App.hTabPages[3]);
        g_App.hTabPages[3] = NULL;

        if (g_App.hFont) {
            DeleteObject(g_App.hFont);
            g_App.hFont = NULL;
        }
        if (g_App.hTabFont) {
            DeleteObject(g_App.hTabFont);
            g_App.hTabFont = NULL;
        }

        /* Destroy managers */
        if (g_App.netTerm) {
            NetTerminal_Destroy(g_App.netTerm);
            g_App.netTerm = NULL;
        }
        if (g_App.uartTerm) {
            UartTerminal_Destroy(g_App.uartTerm);
            g_App.uartTerm = NULL;
        }
        if (g_App.canCmd) {
            CanCommand_Destroy(g_App.canCmd);
            g_App.canCmd = NULL;
        }
        if (g_App.canMgr) {
            CanManager_Destroy(g_App.canMgr);
            g_App.canMgr = NULL;
        }
        if (g_App.uartMgr) {
            UartManager_Destroy(g_App.uartMgr);
            g_App.uartMgr = NULL;
        }

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    HWND        hWnd;
    MSG         msg;

    /* Initialize common controls */
    InitCommonControls();

    /* Create CAN and UART managers */
    g_App.canMgr = CanManager_Create();
    if (!g_App.canMgr) {
        MessageBoxW(NULL, L"无法创建CAN管理器", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    g_App.uartMgr = UartManager_Create();
    if (!g_App.uartMgr) {
        MessageBoxW(NULL, L"无法创建UART管理器", L"错误", MB_OK | MB_ICONERROR);
        CanManager_Destroy(g_App.canMgr);
        return 1;
    }

    /* Create CAN command module */
    g_App.canCmd = CanCommand_Create(PCAN_NONEBUS);
    if (!g_App.canCmd) {
        MessageBoxW(NULL, L"无法创建CAN命令模块", L"错误", MB_OK | MB_ICONERROR);
        UartManager_Destroy(g_App.uartMgr);
        CanManager_Destroy(g_App.canMgr);
        return 1;
    }

    /* Create UartTerminal module */
    g_App.uartTerm = UartTerminal_Create();
    if (!g_App.uartTerm) {
        MessageBoxW(NULL, L"无法创建UART终端模块", L"错误", MB_OK | MB_ICONERROR);
        CanCommand_Destroy(g_App.canCmd);
        UartManager_Destroy(g_App.uartMgr);
        CanManager_Destroy(g_App.canMgr);
        return 1;
    }

    /* Create NetTerminal module */
    g_App.netTerm = NetTerminal_Create();
    if (!g_App.netTerm) {
        MessageBoxW(NULL, L"无法创建网络终端模块", L"错误", MB_OK | MB_ICONERROR);
        UartTerminal_Destroy(g_App.uartTerm);
        CanCommand_Destroy(g_App.canCmd);
        UartManager_Destroy(g_App.uartMgr);
        CanManager_Destroy(g_App.canMgr);
        return 1;
    }

    /* Register window class */
    WNDCLASSEXW wc = { 0 };
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"CanUartToolClass";
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);

    /* Calculate window size for desired client area of 800x560 */
    DWORD dwStyle   = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    DWORD dwExStyle = 0;
    RECT  rcWin;
    CalcWindowRectFromClient(dwStyle, dwExStyle, WINDOW_WIDTH, WINDOW_HEIGHT, &rcWin);

    /* Create main window */
    hWnd = CreateWindowExW(
        dwExStyle,
        L"CanUartToolClass",
        L"CAN/UART 工具",
        dwStyle,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rcWin.right - rcWin.left,
        rcWin.bottom - rcWin.top,
        NULL,
        LoadMenuW(hInstance, MAKEINTRESOURCEW(IDR_MAINMENU)),
        hInstance,
        NULL);

    if (!hWnd) {
        MessageBoxW(NULL, L"创建主窗口失败", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    /* Message loop */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
