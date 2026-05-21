/**
 * ModHandler PC Tool - Main Window with Tab Control
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include <commdlg.h>
#include "resource.h"
#include "can_hal.h"
#include "can_dispatcher.h"
#include "can_manager.h"
#include "uart_manager.h"
#include "can_command.h"
#include "lora_sdk.h"

/* Forward declarations for tab page creation */
extern HWND TabCanUpgrade_Create(HWND hParent, HINSTANCE hInst, CanManager *can_mgr, UartManager *uart_mgr, HWND hNotifyWnd);
extern void TabCanUpgrade_Destroy(HWND hwnd);
extern HWND TabCanCommand_Create(HWND hParent, HINSTANCE hInst, CanCommand *cmd);
extern void TabCanCommand_Destroy(HWND hwnd);
extern void TabCanCommand_UpdateChannel(HWND hwnd, int channel);
extern HWND TabLoraData_Create(HWND hParent, HINSTANCE hInst, lora_sdk_t *sdk);
extern void TabLoraData_Destroy(HWND hwnd);
extern HWND TabLoraCfg_Create(HWND hParent, HINSTANCE hInst, lora_sdk_t *sdk);
extern void TabLoraCfg_Destroy(HWND hwnd);

#define MAX_TABS 4

static const wchar_t *g_TabNames[MAX_TABS] = {
    L"LoRa 数据",
    L"LoRa 配置",
    L"固件升级",
    L"CAN 命令"
};

/* LoRa SDK thread-marshaling payload structs */
typedef struct {
    uint32_t nid;
    uint16_t len;
    uint8_t  data[1]; /* variable-length */
} LoraFrameMsg;

typedef struct {
    char mac[32];
    char name[64];
    char sw[32];
    char ip[64];
} LoraDeviceMsg;

typedef struct {
    char ip[64];
    char mask[64];
    char gateway[64];
} LoraNetParamsMsg;

typedef struct {
    char prefix[64];
    int  len;
    uint8_t data[1]; /* variable-length */
} LoraHexDumpMsg;

/* Tab HWND pair passed as SDK callback user_data */
typedef struct {
    HWND hDataTab;
    HWND hCfgTab;
} LoraTabPair;

typedef struct {
    HWND  hTabCtrl;
    HWND  hStatusBar;
    HWND  hTabPages[MAX_TABS];
    HFONT hFont;
    HFONT hTabFont;
    CanHal        *canHal;
    CanDispatcher *canDisp;
    CanManager    *canMgr;
    UartManager   *uartMgr;
    CanCommand    *canCmd;
    lora_sdk_t    *loraSdk;
    LoraTabPair    loraTabs;
} APP_DATA;

static APP_DATA g_App;

/* ------------------------------------------------------------------ */
/*  LoRa SDK callbacks (fire from background threads)                 */
/* ------------------------------------------------------------------ */

static void cb_on_conn_state(void *ud, enum lora_sdk_conn_state state)
{
    LoraTabPair *tabs = (LoraTabPair *)ud;
    if (tabs->hDataTab)
        PostMessageW(tabs->hDataTab, WM_LORA_CONN_STATE, (WPARAM)state, 0);
}

static void cb_on_frame(void *ud, uint32_t nid,
                         const uint8_t *payload, uint16_t payload_len)
{
    LoraTabPair *tabs = (LoraTabPair *)ud;
    if (!tabs->hDataTab || !payload || payload_len == 0) return;
    LoraFrameMsg *msg = (LoraFrameMsg *)malloc(
        offsetof(LoraFrameMsg, data) + payload_len);
    if (!msg) return;
    msg->nid = nid;
    msg->len = payload_len;
    memcpy(msg->data, payload, payload_len);
    PostMessageW(tabs->hDataTab, WM_LORA_FRAME, 0, (LPARAM)msg);
}

static void cb_on_device_found(void *ud, const char *mac,
                                const char *device_name,
                                const char *sw_version,
                                const char *from_ip)
{
    LoraTabPair *tabs = (LoraTabPair *)ud;
    if (!tabs->hCfgTab) return;
    LoraDeviceMsg *msg = (LoraDeviceMsg *)calloc(1, sizeof(LoraDeviceMsg));
    if (!msg) return;
    if (mac)         strncpy(msg->mac, mac, 31);
    if (device_name) strncpy(msg->name, device_name, 63);
    if (sw_version)  strncpy(msg->sw, sw_version, 31);
    if (from_ip)     strncpy(msg->ip, from_ip, 63);
    PostMessageW(tabs->hCfgTab, WM_LORA_DEVICE_FOUND, 0, (LPARAM)msg);
}

static void cb_on_at_response(void *ud, const char *at_response)
{
    LoraTabPair *tabs = (LoraTabPair *)ud;
    if (!tabs->hCfgTab || !at_response) return;
    char *copy = _strdup(at_response);
    if (copy) PostMessageW(tabs->hCfgTab, WM_LORA_AT_RESPONSE, 0, (LPARAM)copy);
}

static void cb_on_net_params(void *ud, const char *ip,
                              const char *mask, const char *gateway)
{
    LoraTabPair *tabs = (LoraTabPair *)ud;
    if (!tabs->hCfgTab) return;
    LoraNetParamsMsg *msg = (LoraNetParamsMsg *)calloc(1, sizeof(LoraNetParamsMsg));
    if (!msg) return;
    if (ip)      strncpy(msg->ip, ip, 63);
    if (mask)    strncpy(msg->mask, mask, 63);
    if (gateway) strncpy(msg->gateway, gateway, 63);
    PostMessageW(tabs->hCfgTab, WM_LORA_NET_PARAMS, 0, (LPARAM)msg);
    if (tabs->hDataTab) {
        LoraNetParamsMsg *msg2 = (LoraNetParamsMsg *)calloc(1, sizeof(LoraNetParamsMsg));
        if (msg2) {
            memcpy(msg2->ip, msg->ip, 64);
            memcpy(msg2->mask, msg->mask, 64);
            memcpy(msg2->gateway, msg->gateway, 64);
            PostMessageW(tabs->hDataTab, WM_LORA_NET_PARAMS, 0, (LPARAM)msg2);
        }
    }
}

static void cb_on_log(void *ud, const char *message,
                      enum lora_sdk_log_source source)
{
    LoraTabPair *tabs = (LoraTabPair *)ud;
    if (!message) return;

    /* Route to appropriate tab based on source */
    HWND hTarget = (source == LORA_SDK_LOG_TCP) ? tabs->hDataTab : tabs->hCfgTab;
    if (!hTarget) return;

    char *copy = _strdup(message);
    if (copy) PostMessageW(hTarget, WM_LORA_LOG, 0, (LPARAM)copy);
}

static void cb_on_hex_dump(void *ud, const char *prefix,
                            const uint8_t *data, int len)
{
    LoraTabPair *tabs = (LoraTabPair *)ud;
    if (!tabs->hDataTab || !data || len <= 0) return;
    LoraHexDumpMsg *msg = (LoraHexDumpMsg *)malloc(
        offsetof(LoraHexDumpMsg, data) + len);
    if (!msg) return;
    memset(msg->prefix, 0, sizeof(msg->prefix));
    if (prefix) strncpy(msg->prefix, prefix, 63);
    msg->len = len;
    memcpy(msg->data, data, len);
    PostMessageW(tabs->hDataTab, WM_LORA_HEX_DUMP, 0, (LPARAM)msg);
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

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

    /* Tab 0: LoRa Data page */
    g_App.hTabPages[0] = TabLoraData_Create(hTabCtrl, hInst, g_App.loraSdk);

    /* Tab 1: LoRa Config page */
    g_App.hTabPages[1] = TabLoraCfg_Create(hTabCtrl, hInst, g_App.loraSdk);

    /* Tab 2: Firmware Upgrade */
    g_App.hTabPages[2] = TabCanUpgrade_Create(hTabCtrl, hInst,
                                               g_App.canMgr, g_App.uartMgr,
                                               hMainWnd);

    /* Tab 3: CAN Command page */
    g_App.hTabPages[3] = TabCanCommand_Create(hTabCtrl, hInst, g_App.canCmd);

    /* Store tab HWNDs for SDK callback routing */
    g_App.loraTabs.hDataTab = g_App.hTabPages[0];
    g_App.loraTabs.hCfgTab  = g_App.hTabPages[1];

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
            28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");
        SendMessageW(g_App.hTabCtrl, WM_SETFONT, (WPARAM)g_App.hTabFont, TRUE);

        /* Increase tab label spacing (horizontal, vertical padding) */
        SendMessageW(g_App.hTabCtrl, TCM_SETPADDING, 0, MAKELPARAM(24, 4));

        /* Insert tabs */
        TCITEMW tie;
        tie.mask = TCIF_TEXT;
        for (i = 0; i < MAX_TABS; i++) {
            /* TCITEMW.pszText is LPWSTR (non-const), safe cast for read-only text */
            tie.pszText = (wchar_t *)g_TabNames[i];
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
            24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

    case WM_GETMINMAXINFO: {
        /* Set minimum window size so controls remain usable */
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = 900;
        mmi->ptMinTrackSize.y = 650;
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *pnm = (NMHDR *)lParam;
        if (pnm->idFrom == IDC_TAB_CONTROL) {
            if (pnm->code == TCN_SELCHANGE) {
                int sel = TabCtrl_GetCurSel(g_App.hTabCtrl);
                int i;
                for (i = 0; i < MAX_TABS; i++) {
                    ShowWindow(g_App.hTabPages[i], (i == sel) ? SW_SHOW : SW_HIDE);
                }
            }
            else if (pnm->code == NM_CUSTOMDRAW) {
                NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lParam;
                switch (cd->dwDrawStage) {
                case CDDS_PREPAINT:
                    SetWindowLongPtrW(hWnd, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                    return TRUE;
                case CDDS_ITEMPREPAINT: {
                    int sel = TabCtrl_GetCurSel(g_App.hTabCtrl);
                    int idx = (int)cd->dwItemSpec;
                    HDC hdc = cd->hdc;
                    RECT rc = cd->rc;

                    /* Extend tab rect to fill gap at bottom */
                    rc.bottom += 4;

                    HBRUSH hBrush;
                    if (idx == sel) {
                        /* Selected: blue background, white text */
                        hBrush = CreateSolidBrush(RGB(0x00, 0x78, 0xD4));
                        FillRect(hdc, &rc, hBrush);
                        DeleteObject(hBrush);
                        SetTextColor(hdc, RGB(0xFF, 0xFF, 0xFF));
                    } else {
                        /* Unselected: light gray background, dark text */
                        FillRect(hdc, &rc, (HBRUSH)GetSysColorBrush(COLOR_BTNFACE));
                        SetTextColor(hdc, RGB(0x33, 0x33, 0x33));
                    }
                    SetBkMode(hdc, TRANSPARENT);

                    /* Draw tab text centered */
                    SelectObject(hdc, g_App.hTabFont);
                    RECT textRc = rc;
                    DrawTextW(hdc, g_TabNames[idx], -1, &textRc,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SetWindowLongPtrW(hWnd, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                    return TRUE;
                }
                }
            }
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FILE_OPEN: {
            /* Open firmware file and set path in Tab1 (CAN Upgrade) */
            wchar_t fileName[MAX_PATH] = L"";
            OPENFILENAMEW ofn;
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hWnd;
            ofn.lpstrFilter = L"固件文件 (*.bin)\0*.bin\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile   = fileName;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            ofn.lpstrTitle  = L"选择固件文件";
            if (GetOpenFileNameW(&ofn)) {
                if (g_App.hTabPages[2]) {
                    HWND hEdit = GetDlgItem(g_App.hTabPages[2], IDC_EDIT_FIRMWARE);
                    if (hEdit) SetWindowTextW(hEdit, fileName);
                }
            }
            return 0;
        }
        case IDM_FILE_EXIT:
            DestroyWindow(hWnd);
            return 0;
        case IDM_HELP_ABOUT:
            MessageBoxW(hWnd,
                        L"ModHandler PC Tool v0.3.0\n"
                        L"基于 Win32 API 的激光测距系统 PC 端配套工具\n\n"
                        L"功能：\n"
                        L"  - LoRa 网关数据通信与测试\n"
                        L"  - LoRa 网关配置管理\n"
                        L"  - CAN 帧收发与监控\n"
                        L"  - 固件升级\n\n"
                        L"依赖：PCAN-Basic SDK / IXXAT VCI SDK（运行时动态加载）\n"
                         L"编译：CMake + Visual Studio 2026",
                        L"关于 ModHandler PC Tool",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;

    /* CAN 连接共享：固件升级 Tab 通知 CAN 命令 Tab 更新通道 */
    case WM_CAN_CONNECTED:
        if (g_App.hTabPages[3]) {
            TabCanCommand_UpdateChannel(g_App.hTabPages[3], (int)wParam);
            SendMessageW(g_App.hStatusBar, SB_SETTEXTW, 0,
                         (LPARAM)L"CAN 已连接 - 通道已同步到 CAN 命令页");
        }
        return 0;

    case WM_CAN_DISCONNECTED:
        if (g_App.hTabPages[3]) {
            TabCanCommand_UpdateChannel(g_App.hTabPages[3], CAN_HAL_INVALID_HANDLE);
            SendMessageW(g_App.hStatusBar, SB_SETTEXTW, 0,
                         (LPARAM)L"CAN 已断开");
        }
        return 0;

    /* Adapter switch: replace HAL + dispatcher, update all consumers */
    case WM_ADAPTER_CHANGED: {
        int adapter_idx = (int)wParam;
        if (adapter_idx < 0 || adapter_idx >= CAN_HAL_ADAPTER_COUNT) return 0;

        CanHal *newHal = CanHal_Create(adapter_idx);
        if (!newHal) return 0;

        CanDispatcher *newDisp = CanDisp_Create(newHal);
        if (!newDisp) {
            CanHal_Destroy(newHal);
            return 0;
        }

        /* Swap: update all consumers, then destroy old */
        CanHal        *oldHal  = g_App.canHal;
        CanDispatcher *oldDisp = g_App.canDisp;

        g_App.canHal  = newHal;
        g_App.canDisp = newDisp;

        CanManager_ReplaceHal(g_App.canMgr, newHal, newDisp);
        CanCommand_ReplaceHal(g_App.canCmd, newHal, newDisp);

        CanDisp_Destroy(oldDisp);
        CanHal_Destroy(oldHal);

        /* Tell firmware tab to refresh device list */
        if (g_App.hTabPages[2])
            PostMessage(g_App.hTabPages[2], WM_ADAPTER_CHANGED, 0, 0);

        wchar_t status[128];
        wsprintfW(status, L"适配器已切换到 %s", CanHal_GetName(newHal));
        SendMessageW(g_App.hStatusBar, SB_SETTEXTW, 0, (LPARAM)status);
        return 0;
    }

    case WM_DESTROY:
        /* Destroy tab pages explicitly */
        TabLoraData_Destroy(g_App.hTabPages[0]);
        g_App.hTabPages[0] = NULL;
        TabLoraCfg_Destroy(g_App.hTabPages[1]);
        g_App.hTabPages[1] = NULL;
        TabCanUpgrade_Destroy(g_App.hTabPages[2]);
        g_App.hTabPages[2] = NULL;
        TabCanCommand_Destroy(g_App.hTabPages[3]);
        g_App.hTabPages[3] = NULL;

        /* Clear callback routing pointers */
        g_App.loraTabs.hDataTab = NULL;
        g_App.loraTabs.hCfgTab  = NULL;

        if (g_App.hFont) {
            DeleteObject(g_App.hFont);
            g_App.hFont = NULL;
        }
        if (g_App.hTabFont) {
            DeleteObject(g_App.hTabFont);
            g_App.hTabFont = NULL;
        }

        /* Destroy LoRa SDK (stops background threads) */
        if (g_App.loraSdk) {
            lora_sdk_cleanup(g_App.loraSdk);
            g_App.loraSdk = NULL;
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
        if (g_App.canDisp) {
            CanDisp_Destroy(g_App.canDisp);
            g_App.canDisp = NULL;
        }
        if (g_App.canHal) {
            CanHal_Destroy(g_App.canHal);
            g_App.canHal = NULL;
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
    (void)hPrevInstance;
    (void)lpCmdLine;
    HWND        hWnd;
    MSG         msg;

    /* Initialize common controls */
    InitCommonControls();

    /* Create CAN HAL (default: PCAN) */
    g_App.canHal = CanHal_Create(CAN_HAL_ADAPTER_PCAN);
    if (!g_App.canHal) {
        MessageBoxW(NULL, L"无法创建CAN HAL", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Create CAN frame dispatcher */
    g_App.canDisp = CanDisp_Create(g_App.canHal);
    if (!g_App.canDisp) {
        CanHal_Destroy(g_App.canHal);
        MessageBoxW(NULL, L"无法创建CAN分发器", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Create CAN and UART managers */
    g_App.canMgr = CanManager_Create(g_App.canHal, g_App.canDisp);
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
    g_App.canCmd = CanCommand_Create(g_App.canHal, g_App.canDisp);
    if (!g_App.canCmd) {
        MessageBoxW(NULL, L"无法创建CAN命令模块", L"错误", MB_OK | MB_ICONERROR);
        UartManager_Destroy(g_App.uartMgr);
        CanManager_Destroy(g_App.canMgr);
        return 1;
    }

    /* Create LoRa SDK */
    lora_sdk_callbacks_t loraCbs = {0};
    loraCbs.on_conn_state   = cb_on_conn_state;
    loraCbs.on_frame        = cb_on_frame;
    loraCbs.on_device_found = cb_on_device_found;
    loraCbs.on_at_response  = cb_on_at_response;
    loraCbs.on_net_params   = cb_on_net_params;
    loraCbs.on_log          = cb_on_log;
    loraCbs.on_hex_dump     = cb_on_hex_dump;
    loraCbs.on_error        = cb_on_log;

    g_App.loraTabs.hDataTab = NULL;
    g_App.loraTabs.hCfgTab  = NULL;
    g_App.loraSdk = lora_sdk_init(&loraCbs, &g_App.loraTabs);
    if (!g_App.loraSdk) {
        MessageBoxW(NULL, L"无法创建LoRa SDK", L"错误", MB_OK | MB_ICONERROR);
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
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ModHandlerPcToolClass";
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);

    /* Calculate window size — auto-fit to screen, resizable */
    DWORD dwStyle   = WS_OVERLAPPEDWINDOW;
    DWORD dwExStyle = 0;

    /* Work area excludes taskbar */
    RECT rcWork;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    int workW = rcWork.right - rcWork.left;
    int workH = rcWork.bottom - rcWork.top;

    /* Calculate ideal window rect from desired client area */
    RECT rcWin;
    CalcWindowRectFromClient(dwStyle, dwExStyle, WINDOW_WIDTH, WINDOW_HEIGHT, &rcWin);
    int winW = rcWin.right - rcWin.left;
    int winH = rcWin.bottom - rcWin.top;

    /* Clamp window to work area with margin */
    if (winW > workW - 20) winW = workW - 20;
    if (winH > workH - 10) winH = workH - 10;

    /* Center within work area */
    int posX = rcWork.left + (workW - winW) / 2;
    int posY = rcWork.top  + (workH - winH) / 2;

    hWnd = CreateWindowExW(
        dwExStyle,
        L"ModHandlerPcToolClass",
        L"ModHandler PC Tool",
        dwStyle,
        posX, posY,
        winW, winH,
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

    /* Load accelerator table for keyboard shortcuts */
    HACCEL hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_MAINACCEL));

    /* Message loop */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hWnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return (int)msg.wParam;
}
