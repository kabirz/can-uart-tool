/**
 * CAN/UART Tool - Main Window with Tab Control
 */
#include <windows.h>
#include <commctrl.h>
#include "resource.h"
#include "can_manager.h"
#include "uart_manager.h"
#include "can_command.h"

/* Forward declarations for tab page creation */
extern HWND TabCanUpgrade_Create(HWND hParent, HINSTANCE hInst, CanManager *can_mgr, UartManager *uart_mgr);
extern void TabCanUpgrade_Destroy(HWND hwnd);
extern HWND TabCanCommand_Create(HWND hParent, HINSTANCE hInst, CanCommand *cmd);
extern void TabCanCommand_Destroy(HWND hwnd);
extern void TabCanCommand_UpdateChannel(HWND hwnd, TPCANHandle channel);

#define MAX_TABS 4

static const wchar_t *g_TabNames[MAX_TABS] = {
    L"CAN/UART 升级",
    L"CAN 命令",
    L"UART 终端",
    L"网络终端"
};

static const wchar_t *g_TabPlaceholders[MAX_TABS] = {
    L"Tab 1: CAN/UART 固件升级页面",
    L"Tab 2: CAN 命令发送页面",
    L"Tab 3: UART Shell 终端页面",
    L"Tab 4: TCP/UDP 网络终端页面"
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

/* Create tab pages: tab 0 uses the real upgrade page, others are placeholders */
static void CreateTabPages(HWND hTabCtrl, HINSTANCE hInst)
{
    RECT rc;
    int i;

    /* Get tab control display area */
    GetClientRect(hTabCtrl, &rc);
    TabCtrl_AdjustRect(hTabCtrl, FALSE, &rc);

    /* Tab 0: CAN/UART Firmware Upgrade (real page) */
    g_App.hTabPages[0] = TabCanUpgrade_Create(hTabCtrl, hInst,
                                               g_App.canMgr, g_App.uartMgr);

    /* Tab 1: CAN Command page */
    g_App.hTabPages[1] = TabCanCommand_Create(hTabCtrl, hInst, g_App.canCmd);

    /* Tabs 2-3: placeholder static controls */
    for (i = 2; i < MAX_TABS; i++) {
        g_App.hTabPages[i] = CreateWindowExW(
            0, L"STATIC", g_TabPlaceholders[i],
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            rc.left, rc.top,
            rc.right - rc.left,
            rc.bottom - rc.top,
            hTabCtrl, (HMENU)(INT_PTR)(IDC_STATIC + i), hInst, NULL);
        SendMessageW(g_App.hTabPages[i], WM_SETFONT, (WPARAM)g_App.hFont, TRUE);
    }

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

        /* Create tab pages (placeholders) */
        CreateTabPages(g_App.hTabCtrl, hInst);

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

    case WM_DESTROY:
        /* Destroy tab pages explicitly */
        TabCanUpgrade_Destroy(g_App.hTabPages[0]);
        g_App.hTabPages[0] = NULL;
        TabCanCommand_Destroy(g_App.hTabPages[1]);
        g_App.hTabPages[1] = NULL;

        if (g_App.hFont) {
            DeleteObject(g_App.hFont);
            g_App.hFont = NULL;
        }
        if (g_App.hTabFont) {
            DeleteObject(g_App.hTabFont);
            g_App.hTabFont = NULL;
        }

        /* Destroy managers */
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
