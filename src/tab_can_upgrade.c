/**
 * Tab 1: CAN/UART Firmware Upgrade Page
 *
 * Replicates the reference project's main.c firmware upgrade functionality
 * as a tab page child window.
 */
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"
#include "can_manager.h"
#include "uart_manager.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define TRANSPORT_MODE_CAN    0
#define TRANSPORT_MODE_UART   1

/* CAN baud rate table */
static const int BAUD_RATES[] = {
    CAN_HAL_BAUD_10K, CAN_HAL_BAUD_20K, CAN_HAL_BAUD_50K, CAN_HAL_BAUD_100K,
    CAN_HAL_BAUD_125K, CAN_HAL_BAUD_250K, CAN_HAL_BAUD_500K, CAN_HAL_BAUD_1M
};
static const wchar_t *baudNames[] = {
    L"10K", L"20K", L"50K", L"100K",
    L"125K", L"250K", L"500K", L"1000K"
};

/* UART baud rate table */
static const DWORD UART_BAUD_RATES[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600
};
static const wchar_t *uartBaudNames[] = {
    L"9600", L"19200", L"38400", L"57600",
    L"115200", L"230400", L"460800", L"921600"
};

/* Firmware update thread parameter block */
typedef struct {
    HWND  hwnd;
    wchar_t fileName[MAX_PATH];
    int   testMode;
} FirmwareUpdateParams;

/* Per-window instance data, stored via SetWindowLongPtr */
typedef struct {
    CanManager     *canMgr;
    UartManager    *uartMgr;
    HWND            hNotifyWnd;       /* 主窗口，用于发送 CAN 连接通知 */
    int             transportMode;   /* TRANSPORT_MODE_CAN or TRANSPORT_MODE_UART */
    int             isConnected;
    int             isUpdating;
    HWND            hUpdatingDialog;

    /* Device lists */
    int             channels[MAX_DEVICES];
    int             channelCount;
    SerialPortInfo  serialPorts[MAX_SERIAL_PORTS];
    int             serialPortCount;

    /* Child control handles (set during WM_CREATE) */
    HWND hBtnCan;
    HWND hBtnUart;
    HWND hComboAdapter;
    HWND hComboChannel;
    HWND hComboBaudrate;
    HWND hComboUartBaudrate;
    HWND hBtnRefresh;
    HWND hBtnConnect;
    HWND hEditFirmware;
    HWND hBtnBrowse;
    HWND hCheckTestMode;
    HWND hProgress;
    HWND hLabelPercent;
    HWND hBtnFlash;
    HWND hBtnGetVersion;
    HWND hBtnReboot;
    HWND hLabelVersion;
    HWND hEditLog;
    HWND hBtnClearLog;

    /* Fonts */
    HFONT hFont;
    HFONT hFontBold;
} TAB_UPGRADE_DATA;

/* Struct used to pass both managers through lpCreateParams */
typedef struct {
    CanManager  *canMgr;
    UartManager *uartMgr;
    HWND         hNotifyWnd;   /* 主窗口句柄，用于发送 CAN 连接/断开通知 */
} TabCanUpgrade_InitParams;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static TAB_UPGRADE_DATA *GetTabPageData(HWND hwnd)
{
    return (TAB_UPGRADE_DATA *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

/* Append a timestamped line to the log edit control */
static void AppendLog(TAB_UPGRADE_DATA *pData, const char *msg)
{
    HWND hLog = pData->hEditLog;
    if (!hLog) return;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, msg, -1, NULL, 0);
    wchar_t *wstr = (wchar_t *)malloc(sizeof(wchar_t) * wlen);
    MultiByteToWideChar(CP_UTF8, 0, msg, -1, wstr, wlen);

    SYSTEMTIME st;
    wchar_t timestamp[16];
    GetLocalTime(&st);
    wsprintfW(timestamp, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    int len = GetWindowTextLengthW(hLog);
    SendMessageW(hLog, EM_SETSEL, len, len);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)timestamp);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)wstr);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");

    free(wstr);
}

/* Static trampoline so CanManager/UartManager can call AppendLog */
static TAB_UPGRADE_DATA *g_ActiveDataForLog = NULL;

static void LogCallback(const char *msg)
{
    if (g_ActiveDataForLog)
        AppendLog(g_ActiveDataForLog, msg);
}

static void ProgressCallback(int percent)
{
    /* Forward progress to the active page via PostMessage */
    if (g_ActiveDataForLog && g_ActiveDataForLog->hUpdatingDialog)
        PostMessage(g_ActiveDataForLog->hUpdatingDialog, WM_UPDATE_PROGRESS, percent, 0);
}

/* Enable/disable the Flash button based on connection, file, and update state */
static void UpdateFlashButtonState(TAB_UPGRADE_DATA *pData)
{
    wchar_t fileName[MAX_PATH];
    GetWindowTextW(pData->hEditFirmware, fileName, MAX_PATH);
    int hasFile = (wcslen(fileName) > 0);
    EnableWindow(pData->hBtnFlash, pData->isConnected && hasFile && !pData->isUpdating);
}

/* ------------------------------------------------------------------ */
/*  Device enumeration                                                */
/* ------------------------------------------------------------------ */

static void GetDeviceList(TAB_UPGRADE_DATA *pData)
{
    wchar_t buf[128];
    HWND hChannel = pData->hComboChannel;

    SendMessageW(hChannel, CB_RESETCONTENT, 0, 0);

    if (pData->transportMode == TRANSPORT_MODE_CAN) {
        CanManager_SetCallback(pData->canMgr, LogCallback);
        pData->channelCount = CanManager_DetectDevice(pData->canMgr, pData->channels, MAX_DEVICES);

        const char *adapterName = "";
        CanHal *hal = CanManager_GetHal(pData->canMgr);
        if (hal) adapterName = CanHal_GetName(hal);

        for (int i = 0; i < pData->channelCount; i++) {
            wsprintfW(buf, L"%S: %xh", adapterName, pData->channels[i]);
            SendMessageW(hChannel, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        /* Append virtual CAN option */
        if (pData->channelCount < MAX_DEVICES) {
            SendMessageW(hChannel, CB_ADDSTRING, 0, (LPARAM)L"虚拟 CAN (测试模式)");
            pData->channels[pData->channelCount] = CAN_HAL_VIRTUAL_CHANNEL;
            pData->channelCount++;
        }
    } else {
        UartManager_SetCallback(pData->uartMgr, LogCallback);
        pData->serialPortCount = UartManager_EnumPorts(pData->uartMgr, pData->serialPorts, MAX_SERIAL_PORTS);

        for (int i = 0; i < pData->serialPortCount; i++) {
            MultiByteToWideChar(CP_UTF8, 0, pData->serialPorts[i].friendlyName, -1, buf, 128);
            SendMessageW(hChannel, CB_ADDSTRING, 0, (LPARAM)buf);
        }
    }

    if (SendMessageW(hChannel, CB_GETCOUNT, 0, 0) > 0)
        SendMessageW(hChannel, CB_SETCURSEL, 0, 0);
}

/* Show/hide controls depending on transport mode */
static void UpdateTransportModeUI(TAB_UPGRADE_DATA *pData)
{
    if (pData->transportMode == TRANSPORT_MODE_CAN) {
        ShowWindow(pData->hComboBaudrate, SW_SHOW);
        ShowWindow(pData->hComboUartBaudrate, SW_HIDE);
        ShowWindow(pData->hComboAdapter, SW_SHOW);
        SendMessageW(pData->hBtnCan, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(pData->hBtnUart, BM_SETCHECK, BST_UNCHECKED, 0);
    } else {
        ShowWindow(pData->hComboBaudrate, SW_HIDE);
        ShowWindow(pData->hComboUartBaudrate, SW_SHOW);
        ShowWindow(pData->hComboAdapter, SW_HIDE);
        SendMessageW(pData->hBtnCan, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(pData->hBtnUart, BM_SETCHECK, BST_CHECKED, 0);
    }
    EnableWindow(pData->hBtnRefresh, !pData->isConnected);

    g_ActiveDataForLog = pData;
    GetDeviceList(pData);
}

/* ------------------------------------------------------------------ */
/*  Firmware update thread                                            */
/* ------------------------------------------------------------------ */

static DWORD WINAPI FirmwareUpdateThread(LPVOID lpParam)
{
    FirmwareUpdateParams *params = (FirmwareUpdateParams *)lpParam;

    TAB_UPGRADE_DATA *pData = GetTabPageData(params->hwnd);
    pData->hUpdatingDialog = params->hwnd;

    int success = 0;
    if (pData->transportMode == TRANSPORT_MODE_CAN) {
        CanManager_SetProgressCallback(pData->canMgr, ProgressCallback);
        success = CanManager_FirmwareUpgrade(pData->canMgr, params->fileName, params->testMode);
    } else {
        UartManager_SetProgressCallback(pData->uartMgr, ProgressCallback);
        success = UartManager_FirmwareUpgrade(pData->uartMgr, params->fileName, params->testMode);
    }
    PostMessage(params->hwnd, WM_UPDATE_COMPLETE, success, 0);

    pData->hUpdatingDialog = NULL;
    free(params);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Layout helper – create a static label                             */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  WndProc for the tab page                                          */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK TabCanUpgrade_WndProc(HWND hwnd, UINT uMsg,
                                               WPARAM wParam, LPARAM lParam)
{
    TAB_UPGRADE_DATA *pData = GetTabPageData(hwnd);

    switch (uMsg) {

    /* ---- Creation ---- */
    case WM_NCCREATE: {
        /* Allocate per-window data early so WM_CREATE can use it */
        pData = (TAB_UPGRADE_DATA *)calloc(1, sizeof(TAB_UPGRADE_DATA));
        if (!pData) return FALSE;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pData);
        return TRUE;
    }

    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        HINSTANCE hInst = cs->hInstance;

        /* Recover the manager pointers passed via lpCreateParams */
        TabCanUpgrade_InitParams *init = (TabCanUpgrade_InitParams *)cs->lpCreateParams;
        pData->canMgr    = init->canMgr;
        pData->uartMgr   = init->uartMgr;
        pData->hNotifyWnd = init->hNotifyWnd;

        /* Create fonts */
        pData->hFont = CreateFontW(
            FONT_H, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");
        pData->hFontBold = CreateFontW(
            FONT_H, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");

        /* ---- Layout coordinates ---- */
        int margin = MARGIN;
        int lineH  = LINE_H;
        int lblW   = 104;  /* wider than LABEL_W(72) to fit Chinese labels like "传输模式:" */

        /* ========== Group 1: Connection Settings (left column) ========== */
        int grp1X = margin, grp1Y = margin;
        int grp1W = 630, grp1H = 236;
        CreateWindowExW(0, L"BUTTON", L"连接设置",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            grp1X, grp1Y, grp1W, grp1H, hwnd, NULL, hInst, NULL);

        int cx = grp1X + 14, cy = grp1Y + LINE_H;

        /* Row 1: Transport mode label + CAN/UART radio buttons */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, lblW, CTRL_H, L"传输模式:", pData->hFont);
        pData->hBtnCan = CreateWindowExW(0, L"BUTTON", L"CAN",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            cx + lblW + 8, cy, 70, CTRL_H, hwnd, (HMENU)0x9001, hInst, NULL);
        SendMessageW(pData->hBtnCan, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hBtnUart = CreateWindowExW(0, L"BUTTON", L"UART",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            cx + lblW + 8 + 78, cy, 80, CTRL_H, hwnd, (HMENU)0x9002, hInst, NULL);
        SendMessageW(pData->hBtnUart, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        cy += lineH;

        /* Row 1.5: Adapter type selector (only shown for CAN mode) */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, lblW, CTRL_H, L"适配器:", pData->hFont);
        pData->hComboAdapter = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + lblW + 8, cy, 240, 200, hwnd, (HMENU)IDC_COMBO_ADAPTER, hInst, NULL);
        SendMessageW(pData->hComboAdapter, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        SendMessageW(pData->hComboAdapter, CB_ADDSTRING, 0, (LPARAM)L"PEAK PCAN-USB");
        SendMessageW(pData->hComboAdapter, CB_ADDSTRING, 0, (LPARAM)L"IXXAT USB-to-CAN");
        SendMessageW(pData->hComboAdapter, CB_SETCURSEL, 0, 0);
        cy += lineH;

        /* Row 2: Device label + Channel ComboBox + Baud label + Baud ComboBox */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, lblW, CTRL_H, L"设备:", pData->hFont);
        pData->hComboChannel = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + lblW + 8, cy, 240, 200, hwnd, (HMENU)IDC_COMBO_CHANNEL, hInst, NULL);
        SendMessageW(pData->hComboChannel, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        CreateLabel(hwnd, hInst, -1, cx + lblW + 8 + 250, cy + 3, 72, CTRL_H, L"波特率:", pData->hFont);
        pData->hComboBaudrate = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + lblW + 8 + 250 + 72 + 8, cy, COMBO_W, 200,
            hwnd, (HMENU)IDC_COMBO_BAUDRATE, hInst, NULL);
        SendMessageW(pData->hComboBaudrate, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hComboUartBaudrate = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + lblW + 8 + 250 + 72 + 8, cy, COMBO_W, 200,
            hwnd, (HMENU)IDC_COMBO_UART_BAUDRATE, hInst, NULL);
        SendMessageW(pData->hComboUartBaudrate, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        cy += lineH;

        /* Row 3: Refresh + Connect buttons */
        pData->hBtnRefresh = CreateWindowExW(0, L"BUTTON", L"刷新",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + lblW + 8, cy, BTN_W, CTRL_H, hwnd, (HMENU)IDC_BUTTON_REFRESH, hInst, NULL);
        SendMessageW(pData->hBtnRefresh, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hBtnConnect = CreateWindowExW(0, L"BUTTON", L"连接",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + lblW + 8 + BTN_W + 10, cy, BTN_W, CTRL_H, hwnd, (HMENU)IDC_BUTTON_CONNECT, hInst, NULL);
        SendMessageW(pData->hBtnConnect, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        cy += lineH;

        /* Row 4: Firmware version label (spans full width) */
        pData->hLabelVersion = CreateWindowExW(0, L"STATIC",
            L"固件版本: 未获取",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx, cy + 3, grp1W - 28, CTRL_H, hwnd, (HMENU)IDC_LABEL_VERSION, hInst, NULL);
        SendMessageW(pData->hLabelVersion, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Populate baud rate combo boxes */
        for (int i = 0; i < 8; i++)
            SendMessageW(pData->hComboBaudrate, CB_ADDSTRING, 0, (LPARAM)baudNames[i]);
        SendMessageW(pData->hComboBaudrate, CB_SETCURSEL, 5, 0); /* default 250K */

        for (int i = 0; i < 8; i++)
            SendMessageW(pData->hComboUartBaudrate, CB_ADDSTRING, 0, (LPARAM)uartBaudNames[i]);
        SendMessageW(pData->hComboUartBaudrate, CB_SETCURSEL, 4, 0); /* default 115200 */

        /* ========== Group 2: Firmware Upgrade (left column, below Group 1) ========== */
        int grp2X = margin, grp2Y = grp1Y + grp1H + 8;
        int grp2W = 630, grp2H = 200;
        CreateWindowExW(0, L"BUTTON", L"固件升级",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            grp2X, grp2Y, grp2W, grp2H, hwnd, NULL, hInst, NULL);

        cx = grp2X + 14;
        cy = grp2Y + LINE_H;

        /* Row 1: Firmware file label + Edit + Browse button */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, lblW, CTRL_H, L"固件文件:", pData->hFont);
        pData->hEditFirmware = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
            cx + lblW + 8, cy, 350, CTRL_H, hwnd, (HMENU)IDC_EDIT_FIRMWARE, hInst, NULL);
        SendMessageW(pData->hEditFirmware, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hBtnBrowse = CreateWindowExW(0, L"BUTTON", L"浏览...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + lblW + 8 + 358, cy, BTN_W, CTRL_H, hwnd, (HMENU)IDC_BUTTON_BROWSE, hInst, NULL);
        SendMessageW(pData->hBtnBrowse, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        cy += lineH;

        /* Row 2: Test mode checkbox */
        pData->hCheckTestMode = CreateWindowExW(0, L"BUTTON", L"测试模式",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            cx, cy, 120, CTRL_H, hwnd, (HMENU)IDC_CHECK_TESTMODE, hInst, NULL);
        SendMessageW(pData->hCheckTestMode, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        cy += lineH;

        /* Row 3: Progress label + Progress bar + percentage + Start upgrade button */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, lblW, CTRL_H, L"进度:", pData->hFont);
        pData->hProgress = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE,
            cx + lblW + 8, cy, 290, CTRL_H, hwnd, (HMENU)IDC_PROGRESS, hInst, NULL);
        SendMessageW(pData->hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(pData->hProgress, PBM_SETPOS, 0, 0);

        pData->hLabelPercent = CreateWindowExW(0, L"STATIC", L"0%",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + lblW + 8 + 298, cy + 3, 44, CTRL_H, hwnd, (HMENU)IDC_LABEL_PERCENT, hInst, NULL);
        SendMessageW(pData->hLabelPercent, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hBtnFlash = CreateWindowExW(0, L"BUTTON", L"开始升级",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + lblW + 8 + 298 + 50, cy, BTN_W + 10, CTRL_H,
            hwnd, (HMENU)IDC_BUTTON_FLASH, hInst, NULL);
        SendMessageW(pData->hBtnFlash, WM_SETFONT, (WPARAM)pData->hFontBold, TRUE);
        EnableWindow(pData->hBtnFlash, FALSE);

        /* ========== Group 3: Board Commands (right column) ========== */
        int grp3X = grp1X + grp1W + 8, grp3Y = margin;
        int grp3W = WINDOW_WIDTH - margin - grp3X;
        int grp3H = grp1H;
        CreateWindowExW(0, L"BUTTON", L"板卡命令",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            grp3X, grp3Y, grp3W, grp3H, hwnd, NULL, hInst, NULL);

        int bx = grp3X + 14;
        int by = grp3Y + LINE_H;

        pData->hBtnGetVersion = CreateWindowExW(0, L"BUTTON", L"获取版本",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            bx, by, BTN_W + 10, CTRL_H, hwnd, (HMENU)IDC_BUTTON_GETVERSION, hInst, NULL);
        SendMessageW(pData->hBtnGetVersion, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        EnableWindow(pData->hBtnGetVersion, FALSE);

        pData->hBtnReboot = CreateWindowExW(0, L"BUTTON", L"重启板卡",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            bx + BTN_W + 20, by, BTN_W + 10, CTRL_H, hwnd, (HMENU)IDC_BUTTON_REBOOT, hInst, NULL);
        SendMessageW(pData->hBtnReboot, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        EnableWindow(pData->hBtnReboot, FALSE);

        /* ========== Group 4: Log Area (full width bottom section) ========== */
        int logGrpX = margin;
        int logGrpY = grp2Y + grp2H + 8;
        int logGrpW = WINDOW_WIDTH - 2 * margin;
        int logGrpH = WINDOW_HEIGHT - TAB_HEIGHT - STATUSBAR_HEIGHT - margin - logGrpY;
        if (logGrpH < 100) logGrpH = 100;
        CreateWindowExW(0, L"BUTTON", L"日志",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            logGrpX, logGrpY, logGrpW, logGrpH, hwnd, NULL, hInst, NULL);

        /* Log edit: multi-line, read-only, vertical scroll - fills groupbox interior */
        int logEditX = logGrpX + 8;
        int logEditY = logGrpY + LINE_H - 2;
        int logEditW = logGrpW - BTN_W - 30;   /* leave room for clear button */
        int logEditH = logGrpH - LINE_H - 8;
        if (logEditH < 50) logEditH = 50;
        pData->hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            logEditX, logEditY,
            logEditW, logEditH,
            hwnd, (HMENU)IDC_EDIT_LOG, hInst, NULL);
        SendMessageW(pData->hEditLog, WM_SETFONT,
            (WPARAM)GetStockObject(SYSTEM_FIXED_FONT), TRUE);

        pData->hBtnClearLog = CreateWindowExW(0, L"BUTTON", L"清除日志",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            logGrpX + logGrpW - BTN_W - 10, logEditY, BTN_W, CTRL_H,
            hwnd, (HMENU)IDC_BUTTON_CLEAR_LOG, hInst, NULL);
        SendMessageW(pData->hBtnClearLog, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* ========== Initialize state ========== */
        pData->transportMode = TRANSPORT_MODE_CAN;
        pData->isConnected   = 0;
        pData->isUpdating    = 0;

        g_ActiveDataForLog = pData;
        UpdateTransportModeUI(pData);
        UpdateFlashButtonState(pData);

        return 0;
    }

    /* ---- Command handling ---- */
    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        /* Transport mode: CAN radio button */
        case 0x9001:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (pData->isConnected) {
                    MessageBoxW(hwnd, L"请先断开当前连接", L"提示",
                               MB_OK | MB_ICONWARNING);
                    SendMessageW(pData->hBtnCan, BM_SETCHECK, BST_CHECKED, 0);
                    return 0;
                }
                pData->transportMode = TRANSPORT_MODE_CAN;
                UpdateTransportModeUI(pData);
            }
            return 0;

        /* Transport mode: UART radio button */
        case 0x9002:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (pData->isConnected) {
                    MessageBoxW(hwnd, L"请先断开当前连接", L"提示",
                               MB_OK | MB_ICONWARNING);
                    SendMessageW(pData->hBtnUart, BM_SETCHECK, BST_CHECKED, 0);
                    return 0;
                }
                pData->transportMode = TRANSPORT_MODE_UART;
                UpdateTransportModeUI(pData);
            }
            return 0;

        case IDC_BUTTON_REFRESH:
            g_ActiveDataForLog = pData;
            GetDeviceList(pData);
            return 0;

        case IDC_COMBO_ADAPTER:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                if (pData->isConnected) {
                    MessageBoxW(hwnd, L"请先断开当前连接", L"提示",
                               MB_OK | MB_ICONWARNING);
                    return 0;
                }
                int adapter_idx = (int)SendMessageW(pData->hComboAdapter, CB_GETCURSEL, 0, 0);
                CanHal *newHal = CanHal_Create(adapter_idx);
                if (newHal) {
                    CanManager *newMgr = CanManager_Create(newHal);
                    if (newMgr) {
                        CanManager_Destroy(pData->canMgr);
                        pData->canMgr = newMgr;
                        g_ActiveDataForLog = pData;
                        GetDeviceList(pData);
                    } else {
                        CanHal_Destroy(newHal);
                    }
                }
            }
            return 0;

        case IDC_BUTTON_CONNECT: {
            if (pData->isUpdating) return 0;

            if (pData->isConnected) {
                /* Disconnect */
                EnableWindow(pData->hBtnConnect, FALSE);
                if (pData->transportMode == TRANSPORT_MODE_CAN) {
                    CanManager_Disconnect(pData->canMgr);
                    /* 通知主窗口 CAN 已断开，以便同步 Tab1 */
                    if (pData->hNotifyWnd)
                        PostMessage(pData->hNotifyWnd, WM_CAN_DISCONNECTED, 0, 0);
                } else
                    UartManager_Disconnect(pData->uartMgr);

                pData->isConnected = 0;
                EnableWindow(pData->hBtnConnect, TRUE);
                SetWindowTextW(pData->hBtnConnect, L"连接");
                EnableWindow(pData->hBtnGetVersion, FALSE);
                EnableWindow(pData->hBtnReboot, FALSE);
                EnableWindow(pData->hBtnRefresh, TRUE);
                SetWindowTextW(pData->hLabelVersion, L"固件版本: 未获取");
                EnableWindow(pData->hComboChannel, TRUE);
                EnableWindow(pData->hComboBaudrate, TRUE);
                EnableWindow(pData->hComboUartBaudrate, TRUE);
                UpdateFlashButtonState(pData);
            } else {
                /* Connect */
                int res = 0;
                EnableWindow(pData->hBtnConnect, FALSE);

                if (pData->transportMode == TRANSPORT_MODE_CAN) {
                    int cnl_idx = SendMessageW(pData->hComboChannel, CB_GETCURSEL, 0, 0);
                    int baud_idx = SendMessageW(pData->hComboBaudrate, CB_GETCURSEL, 0, 0);
                    if (cnl_idx >= 0 && cnl_idx < pData->channelCount && baud_idx >= 0)
                        res = CanManager_Connect(pData->canMgr, pData->channels[cnl_idx], baud_idx);
                } else {
                    int port_idx = SendMessageW(pData->hComboChannel, CB_GETCURSEL, 0, 0);
                    int baud_idx = SendMessageW(pData->hComboUartBaudrate, CB_GETCURSEL, 0, 0);
                    if (port_idx >= 0 && port_idx < pData->serialPortCount && baud_idx >= 0)
                        res = UartManager_Connect(pData->uartMgr,
                            pData->serialPorts[port_idx].portName, UART_BAUD_RATES[baud_idx]);
                }

                if (res) {
                    pData->isConnected = 1;
                    EnableWindow(pData->hBtnConnect, TRUE);
                    SetWindowTextW(pData->hBtnConnect, L"断开");
                    EnableWindow(pData->hBtnGetVersion, TRUE);
                    EnableWindow(pData->hBtnReboot, TRUE);
                    EnableWindow(pData->hBtnRefresh, FALSE);
                    EnableWindow(pData->hComboChannel, FALSE);
                    EnableWindow(pData->hComboBaudrate, FALSE);
                    EnableWindow(pData->hComboUartBaudrate, FALSE);
                    UpdateFlashButtonState(pData);

                    /* 通知主窗口 CAN 已连接，传递通道号给 Tab1 (CAN命令) */
                    if (pData->transportMode == TRANSPORT_MODE_CAN && pData->hNotifyWnd) {
                        int cnl_idx = (int)SendMessageW(pData->hComboChannel, CB_GETCURSEL, 0, 0);
                        if (cnl_idx >= 0 && cnl_idx < pData->channelCount) {
                            PostMessage(pData->hNotifyWnd, WM_CAN_CONNECTED,
                                        (WPARAM)pData->channels[cnl_idx], 0);
                        }
                    }
                } else {
                    EnableWindow(pData->hBtnConnect, TRUE);
                    MessageBoxW(hwnd,
                        L"连接失败\n\n"
                        L"请查看设备是否接入\n"
                        L"或者设备是否被其他程序占用",
                        L"连接失败", MB_OK | MB_ICONWARNING);
                }
            }
            return 0;
        }

        case IDC_BUTTON_BROWSE: {
            wchar_t fileName[MAX_PATH] = L"";
            OPENFILENAMEW ofn;
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hwnd;
            ofn.lpstrFilter = L"固件文件 (*.bin)\0*.bin\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile   = fileName;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            ofn.lpstrTitle  = L"选择固件文件";
            if (GetOpenFileNameW(&ofn)) {
                SetWindowTextW(pData->hEditFirmware, fileName);
                UpdateFlashButtonState(pData);
            }
            return 0;
        }

        case IDC_BUTTON_GETVERSION: {
            EnableWindow(pData->hBtnGetVersion, FALSE);
            uint32_t version;
            if (pData->transportMode == TRANSPORT_MODE_CAN)
                version = CanManager_GetFirmwareVersion(pData->canMgr);
            else
                version = UartManager_GetFirmwareVersion(pData->uartMgr);

            if (version) {
                wchar_t verMsg[64];
                wsprintfW(verMsg, L"固件版本: v%u.%u.%u",
                    (version >> 24) & 0xFF,
                    (version >> 16) & 0xFF,
                    (version >> 8)  & 0xFF);
                SetWindowTextW(pData->hLabelVersion, verMsg);
            }
            EnableWindow(pData->hBtnGetVersion, TRUE);
            return 0;
        }

        case IDC_BUTTON_REBOOT: {
            int result = MessageBoxW(hwnd,
                L"确认要重启板卡吗？",
                L"确认重启",
                MB_OKCANCEL | MB_ICONINFORMATION);
            if (result == IDOK) {
                int status;
                if (pData->transportMode == TRANSPORT_MODE_CAN)
                    status = CanManager_BoardReboot(pData->canMgr);
                else
                    status = UartManager_BoardReboot(pData->uartMgr);
                if (status)
                    AppendLog(pData, "等待重启完成");
            }
            return 0;
        }

        case IDC_BUTTON_FLASH: {
            wchar_t fileName[MAX_PATH];
            GetWindowTextW(pData->hEditFirmware, fileName, MAX_PATH);

            if (wcslen(fileName) == 0) {
                MessageBoxW(hwnd, L"请先选择固件文件",
                            L"提示", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (pData->isUpdating) {
                MessageBoxW(hwnd, L"固件更新中，请等待完成",
                            L"提示", MB_OK | MB_ICONWARNING);
                return 0;
            }

            int testMode = SendMessageW(pData->hCheckTestMode, BM_GETCHECK, 0, 0) == BST_CHECKED;

            pData->isUpdating = 1;
            EnableWindow(pData->hBtnFlash, FALSE);
            EnableWindow(pData->hBtnBrowse, FALSE);
            EnableWindow(pData->hCheckTestMode, FALSE);
            SendMessageW(pData->hProgress, PBM_SETPOS, 0, 0);

            FirmwareUpdateParams *params = (FirmwareUpdateParams *)malloc(sizeof(FirmwareUpdateParams));
            params->hwnd = hwnd;
            wcscpy(params->fileName, fileName);
            params->testMode = testMode;

            g_ActiveDataForLog = pData;

            DWORD threadId;
            HANDLE hThread = CreateThread(NULL, 0, FirmwareUpdateThread, params, 0, &threadId);
            if (hThread) {
                CloseHandle(hThread);
            } else {
                free(params);
                pData->isUpdating = 0;
                EnableWindow(pData->hBtnFlash, TRUE);
                EnableWindow(pData->hBtnBrowse, TRUE);
                EnableWindow(pData->hCheckTestMode, TRUE);
                MessageBoxW(hwnd, L"创建更新线程失败",
                            L"错误", MB_OK | MB_ICONERROR);
            }
            return 0;
        }

        case IDC_BUTTON_CLEAR_LOG: {
            HWND hLog = pData->hEditLog;
            ShowWindow(hLog, SW_HIDE);
            SetWindowTextW(hLog, L"");
            RedrawWindow(hLog, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
            ShowWindow(hLog, SW_SHOW);
            return 0;
        }

        default:
            break;
        }
        break;

    /* ---- Custom progress messages ---- */
    case WM_UPDATE_PROGRESS: {
        SendMessageW(pData->hProgress, PBM_SETPOS, wParam, 0);
        wchar_t buf[16];
        wsprintfW(buf, L"%d%%", (int)wParam);
        SetWindowTextW(pData->hLabelPercent, buf);
        return 0;
    }

    case WM_UPDATE_COMPLETE: {
        int success = (wParam != 0);
        pData->isUpdating = 0;

        EnableWindow(pData->hBtnBrowse, TRUE);
        EnableWindow(pData->hCheckTestMode, TRUE);
        UpdateFlashButtonState(pData);

        if (success)
            MessageBoxW(hwnd, L"固件升级完成！请重启板卡",
                        L"成功", MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(hwnd, L"固件升级失败，请查看日志",
                        L"失败", MB_OK | MB_ICONERROR);
        return 0;
    }

    /* ---- Cleanup ---- */
    case WM_DESTROY:
        if (pData) {
            /* Disconnect if still connected */
            if (pData->isConnected) {
                if (pData->transportMode == TRANSPORT_MODE_CAN)
                    CanManager_Disconnect(pData->canMgr);
                else
                    UartManager_Disconnect(pData->uartMgr);
            }
            if (pData->hFont)     DeleteObject(pData->hFont);
            if (pData->hFontBold) DeleteObject(pData->hFontBold);
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

static const wchar_t *TAB_UPGRADE_CLASS = L"TabCanUpgradeClass";
static int g_classRegistered = 0;

HWND TabCanUpgrade_Create(HWND hParent, HINSTANCE hInst,
                           CanManager *can_mgr, UartManager *uart_mgr,
                           HWND hNotifyWnd)
{
    if (!g_classRegistered) {
        WNDCLASSEXW wc = { 0 };
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = TabCanUpgrade_WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = TAB_UPGRADE_CLASS;
        RegisterClassExW(&wc);
        g_classRegistered = 1;
    }

    /* We need to pass both pointers through lpCreateParams */
    TabCanUpgrade_InitParams init = { can_mgr, uart_mgr, hNotifyWnd };

    RECT rcParent;
    GetClientRect(hParent, &rcParent);
    TabCtrl_AdjustRect(hParent, FALSE, &rcParent);

    HWND hwnd = CreateWindowExW(
        0,
        TAB_UPGRADE_CLASS,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        rcParent.left, rcParent.top,
        rcParent.right - rcParent.left,
        rcParent.bottom - rcParent.top,
        hParent, NULL, hInst, &init);

    return hwnd;
}

void TabCanUpgrade_Destroy(HWND hwnd)
{
    if (hwnd)
        DestroyWindow(hwnd);
}
