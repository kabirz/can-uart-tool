/**
 * Tab 3: LoRa Data Page
 *
 * LoRa gateway TCP connection, telemetry display, raw log, and
 * history list view. Uses loralib SDK (lora_sdk.h) for all
 * gateway communication.
 */
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"
#include "lora_sdk.h"

/* ------------------------------------------------------------------ */
/*  Data types for marshaling from SDK callbacks to UI thread          */
/* ------------------------------------------------------------------ */

/* WM_LORA_FRAME (lParam = heap-allocated, receiver frees) */
typedef struct {
    uint32_t nid;
    uint16_t len;
    uint8_t  data[1]; /* variable length */
} LoraFrameMsg;

/* WM_LORA_HEX_DUMP (lParam = heap-allocated, receiver frees) */
typedef struct {
    char    prefix[64];
    int     len;
    uint8_t data[1]; /* variable length */
} LoraHexDumpMsg;

/* CSV test entry */
#define MAX_CSV_ENTRIES  2000
typedef struct {
    SYSTEMTIME timestamp;
    uint32_t   nid;
    uint8_t    type;
    uint8_t    data[64];
    int        data_len;
} CsvTestEntry;

/* ------------------------------------------------------------------ */
/*  Per-window instance data (stored via GWLP_USERDATA)                */
/* ------------------------------------------------------------------ */
typedef struct {
    lora_sdk_t *sdk;

    /* Connection group */
    HWND hEditIp;
    HWND hEditPort;
    HWND hBtnConnect;
    HWND hBtnDisconnect;
    HWND hStatusText;
    HWND hNidText;
    HWND hTestCheck;

    /* Telemetry group */
    HWND hXText;
    HWND hYText;
    HWND hBtnText;
    HWND hRxCount;
    HWND hTxCount;
    HWND hErrCount;

    /* Raw log */
    HWND hLogEdit;

    /* Operation group */
    HWND hSendEdit;
    HWND hBtnSend;
    HWND hBtnSaveCsv;
    HWND hBtnClear;

    /* History list view */
    HWND hHistoryList;

    /* Resizable group boxes */
    HWND hGrpConn;       /* Group 1: Connection */
    HWND hGrpTelemetry;  /* Group 2 Left: Telemetry */
    HWND hGrpLog;        /* Group 2 Right: Raw log */
    HWND hGrpOps;        /* Group 3: Operations */
    HWND hGrpHistory;    /* Group 4: History */

    /* Counters */
    int rxCount;
    int txCount;
    int errCount;

    /* Test mode flag */
    int testMode;

    /* CSV test entries */
    CsvTestEntry csvEntries[MAX_CSV_ENTRIES];
    int csvCount;

    /* Fonts */
    HFONT hFont;
    HFONT hFontBold;
    HFONT hFontMono;
} TAB_LORA_DATA;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */
static TAB_LORA_DATA *GetTabPageData(HWND hwnd)
{
    return (TAB_LORA_DATA *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
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

/* Parse hex data bytes from a space-separated string, return count */
static int ParseHexData(const wchar_t *str, uint8_t *out, int maxBytes)
{
    int count = 0;
    const wchar_t *p = str;

    while (*p && count < maxBytes) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        wchar_t *end;
        unsigned long val = wcstoul(p, &end, 16);
        if (end == p) break;
        out[count++] = (uint8_t)(val & 0xFF);
        p = end;
    }
    return count;
}

/* Get system time as formatted string "HH:MM:SS.mmm" */
static void GetTimestampStr(wchar_t *buf, int bufSize)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wsprintfW(buf, L"%02d:%02d:%02d.%03d",
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

/* Append text to the log edit control */
static void AppendLogText(TAB_LORA_DATA *pData, const wchar_t *text)
{
    HWND hLog = pData->hLogEdit;
    if (!hLog) return;

    int len = GetWindowTextLengthW(hLog);
    SendMessageW(hLog, EM_SETSEL, len, len);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(hLog, EM_SCROLLCARET, 0, 0);
}

/* Add an item to the history ListView */
static void AddHistoryEntry(TAB_LORA_DATA *pData, const wchar_t *timeStr,
                             uint32_t nid, const wchar_t *typeStr,
                             const wchar_t *dataStr)
{
    HWND hList = pData->hHistoryList;
    if (!hList) return;

    int count = (int)SendMessageW(hList, LVM_GETITEMCOUNT, 0, 0);

    /* Delete oldest if exceeding max */
    if (count >= 500) {
        SendMessageW(hList, LVM_DELETEITEM, 0, 0);
        count--;
    }

    /* Insert item at end */
    LVITEMW lvi = {0};
    lvi.mask     = LVIF_TEXT;
    lvi.iItem    = count;
    lvi.iSubItem = 0;
    lvi.pszText  = (wchar_t *)timeStr;
    int idx = (int)SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

    /* Sub-items */
    wchar_t nidStr[16];
    wsprintfW(nidStr, L"0x%08X", nid);

    SendMessageW(hList, LVM_SETITEMTEXT, idx, (LPARAM)&(LVITEMW){
        .iSubItem = 1, .pszText = nidStr });
    SendMessageW(hList, LVM_SETITEMTEXT, idx, (LPARAM)&(LVITEMW){
        .iSubItem = 2, .pszText = (wchar_t *)typeStr });
    SendMessageW(hList, LVM_SETITEMTEXT, idx, (LPARAM)&(LVITEMW){
        .iSubItem = 3, .pszText = (wchar_t *)dataStr });

    /* Scroll to bottom */
    SendMessageW(hList, LVM_ENSUREVISIBLE, idx, FALSE);
}

/* Update counter display */
static void UpdateCounters(TAB_LORA_DATA *pData)
{
    wchar_t buf[32];
    wsprintfW(buf, L"RX: %d", pData->rxCount);
    SetWindowTextW(pData->hRxCount, buf);
    wsprintfW(buf, L"TX: %d", pData->txCount);
    SetWindowTextW(pData->hTxCount, buf);
    wsprintfW(buf, L"ERR: %d", pData->errCount);
    SetWindowTextW(pData->hErrCount, buf);
}

/* ------------------------------------------------------------------ */
/*  WndProc for the tab page                                          */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK TabLoraData_WndProc(HWND hwnd, UINT uMsg,
                                             WPARAM wParam, LPARAM lParam)
{
    TAB_LORA_DATA *pData = (TAB_LORA_DATA *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (uMsg) {

    /* ---- Creation ---- */
    case WM_NCCREATE: {
        pData = (TAB_LORA_DATA *)calloc(1, sizeof(TAB_LORA_DATA));
        if (!pData) return FALSE;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pData);
        return TRUE;
    }

    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        HINSTANCE hInst = cs->hInstance;

        pData->sdk = (lora_sdk_t *)cs->lpCreateParams;
        pData->rxCount = 0;
        pData->txCount = 0;
        pData->errCount = 0;
        pData->testMode = 0;
        pData->csvCount = 0;

        /* Get actual client area */
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        int pageW = rcClient.right  > 0 ? rcClient.right  : WINDOW_WIDTH;
        int pageH = rcClient.bottom > 0 ? rcClient.bottom : WINDOW_HEIGHT;

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

        int margin = 14;
        int lineH = 30;
        int cx, cy;

        /* ========== Group 1: Connection (top, full width, ~80px) ========== */
        int grp1X = margin, grp1Y = margin;
        int grp1W = pageW - 2 * margin;
        int grp1H = 80;
        pData->hGrpConn = CreateWindowExW(0, L"BUTTON", L"连接",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            grp1X, grp1Y, grp1W, grp1H, hwnd, NULL, hInst, NULL);
        SendMessageW(pData->hGrpConn, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        cx = grp1X + 14;
        cy = grp1Y + 26;

        /* Row 1: IP + Port + Connect/Disconnect + Status */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 26, 22, L"IP:", pData->hFont);
        pData->hEditIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            L"192.168.2.100",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            cx + 30, cy, 160, 24, hwnd, (HMENU)IDC_LORA_IP_EDIT, hInst, NULL);
        SendMessageW(pData->hEditIp, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        int ox = cx + 198;
        CreateLabel(hwnd, hInst, -1, ox, cy + 3, 42, 22, L"端口:", pData->hFont);
        pData->hEditPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            L"1234",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            ox + 46, cy, 70, 24, hwnd, (HMENU)IDC_LORA_PORT_EDIT, hInst, NULL);
        SendMessageW(pData->hEditPort, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        ox += 122;
        pData->hBtnConnect = CreateWindowExW(0, L"BUTTON", L"连接",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            ox, cy, 80, 26,
            hwnd, (HMENU)IDC_LORA_CONNECT_BTN, hInst, NULL);
        SendMessageW(pData->hBtnConnect, WM_SETFONT, (WPARAM)pData->hFontBold, TRUE);

        ox += 86;
        pData->hBtnDisconnect = CreateWindowExW(0, L"BUTTON", L"断开",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            ox, cy, 80, 26,
            hwnd, (HMENU)IDC_LORA_DISCONNECT_BTN, hInst, NULL);
        SendMessageW(pData->hBtnDisconnect, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        EnableWindow(pData->hBtnDisconnect, FALSE);

        ox += 86;
        pData->hStatusText = CreateWindowExW(0, L"STATIC", L"已断开",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            ox, cy + 3, 160, 22,
            hwnd, (HMENU)IDC_LORA_STATUS_TEXT, hInst, NULL);
        SendMessageW(pData->hStatusText, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Row 2: NID + Test mode */
        cy += lineH;
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 34, 22, L"NID:", pData->hFont);
        pData->hNidText = CreateWindowExW(0, L"STATIC", L"00000000",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 38, cy + 3, 100, 22,
            hwnd, (HMENU)IDC_LORA_NID_TEXT, hInst, NULL);
        SendMessageW(pData->hNidText, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        ox = cx + 150;
        pData->hTestCheck = CreateWindowExW(0, L"BUTTON", L"测试模式",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            ox, cy + 2, 110, 24,
            hwnd, (HMENU)IDC_LORA_TEST_CHECK, hInst, NULL);
        SendMessageW(pData->hTestCheck, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* ========== Group 2: Middle (split left/right) ========== */
        int grp2Y = grp1Y + grp1H + 8;
        int grp2H = 180;

        /* Left: Telemetry (fixed width ~280px) */
        int teleW = 280;
        pData->hGrpTelemetry = CreateWindowExW(0, L"BUTTON", L"遥测",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, grp2Y, teleW, grp2H, hwnd, NULL, hInst, NULL);
        SendMessageW(pData->hGrpTelemetry, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        cx = margin + 14;
        cy = grp2Y + 28;

        /* X angle */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 80, 22, L"X角度:", pData->hFont);
        pData->hXText = CreateWindowExW(0, L"STATIC", L"--",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 84, cy + 3, 120, 22,
            hwnd, (HMENU)IDC_LORA_X_TEXT, hInst, NULL);
        SendMessageW(pData->hXText, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);
        cy += lineH;

        /* Y angle */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 80, 22, L"Y角度:", pData->hFont);
        pData->hYText = CreateWindowExW(0, L"STATIC", L"--",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 84, cy + 3, 120, 22,
            hwnd, (HMENU)IDC_LORA_Y_TEXT, hInst, NULL);
        SendMessageW(pData->hYText, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);
        cy += lineH;

        /* Button state */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 80, 22, L"按键状态:", pData->hFont);
        pData->hBtnText = CreateWindowExW(0, L"STATIC", L"--",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 84, cy + 3, 120, 22,
            hwnd, (HMENU)IDC_LORA_BTN_TEXT, hInst, NULL);
        SendMessageW(pData->hBtnText, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);
        cy += lineH;

        /* Counters */
        pData->hRxCount = CreateWindowExW(0, L"STATIC", L"RX: 0",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx, cy + 3, 80, 22,
            hwnd, (HMENU)IDC_LORA_RX_COUNT, hInst, NULL);
        SendMessageW(pData->hRxCount, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        pData->hTxCount = CreateWindowExW(0, L"STATIC", L"TX: 0",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 80, cy + 3, 80, 22,
            hwnd, (HMENU)IDC_LORA_TX_COUNT, hInst, NULL);
        SendMessageW(pData->hTxCount, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        pData->hErrCount = CreateWindowExW(0, L"STATIC", L"ERR: 0",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 160, cy + 3, 80, 22,
            hwnd, (HMENU)IDC_LORA_ERR_COUNT, hInst, NULL);
        SendMessageW(pData->hErrCount, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        /* Right: Raw log (stretch width) */
        int logX = margin + teleW + 8;
        int logW = pageW - margin - logX - margin;
        if (logW < 200) logW = 200;
        pData->hGrpLog = CreateWindowExW(0, L"BUTTON", L"原始日志",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            logX, grp2Y, logW, grp2H, hwnd, NULL, hInst, NULL);
        SendMessageW(pData->hGrpLog, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        int logEditX = logX + 10;
        int logEditY = grp2Y + 24;
        int logEditW = logW - 20;
        int logEditH = grp2H - 34;
        if (logEditW < 50) logEditW = 50;
        if (logEditH < 50) logEditH = 50;
        pData->hLogEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            logEditX, logEditY, logEditW, logEditH,
            hwnd, (HMENU)IDC_LORA_LOG_EDIT, hInst, NULL);
        SendMessageW(pData->hLogEdit, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        /* ========== Group 3: Operations (full width, ~40px) ========== */
        int grp3Y = grp2Y + grp2H + 8;
        int grp3H = 50;
        int grp3W = pageW - 2 * margin;
        pData->hGrpOps = CreateWindowExW(0, L"BUTTON", L"操作",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, grp3Y, grp3W, grp3H, hwnd, NULL, hInst, NULL);
        SendMessageW(pData->hGrpOps, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        cx = margin + 14;
        cy = grp3Y + 18;

        pData->hSendEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            cx, cy, 400, 24,
            hwnd, (HMENU)IDC_LORA_SEND_EDIT, hInst, NULL);
        SendMessageW(pData->hSendEdit, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        ox = cx + 408;
        pData->hBtnSend = CreateWindowExW(0, L"BUTTON", L"发送",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            ox, cy, 80, 26,
            hwnd, (HMENU)IDC_LORA_SEND_BTN, hInst, NULL);
        SendMessageW(pData->hBtnSend, WM_SETFONT, (WPARAM)pData->hFontBold, TRUE);

        ox += 86;
        pData->hBtnSaveCsv = CreateWindowExW(0, L"BUTTON", L"保存CSV",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            ox, cy, 90, 26,
            hwnd, (HMENU)IDC_LORA_SAVE_CSV_BTN, hInst, NULL);
        SendMessageW(pData->hBtnSaveCsv, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        ox += 96;
        pData->hBtnClear = CreateWindowExW(0, L"BUTTON", L"清除",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            ox, cy, 80, 26,
            hwnd, (HMENU)IDC_LORA_CLEAR_BTN, hInst, NULL);
        SendMessageW(pData->hBtnClear, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* ========== Group 4: History ListView (bottom, stretch height) ========== */
        int grp4Y = grp3Y + grp3H + 8;
        int grp4H = pageH - grp4Y - margin;
        if (grp4H < 100) grp4H = 100;
        int grp4W = pageW - 2 * margin;
        pData->hGrpHistory = CreateWindowExW(0, L"BUTTON", L"历史记录",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, grp4Y, grp4W, grp4H, hwnd, NULL, hInst, NULL);
        SendMessageW(pData->hGrpHistory, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        int listX = margin + 10;
        int listY = grp4Y + 24;
        int listW = grp4W - 20;
        int listH = grp4H - 34;
        if (listW < 100) listW = 100;
        if (listH < 50) listH = 50;

        pData->hHistoryList = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_VSCROLL,
            listX, listY, listW, listH,
            hwnd, (HMENU)IDC_LORA_HISTORY_LIST, hInst, NULL);
        SendMessageW(pData->hHistoryList, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        /* Enable full-row select and grid lines */
        SendMessageW(pData->hHistoryList, LVM_SETEXTENDEDLISTVIEWSTYLE,
                     LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
                     LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        /* Add columns */
        LVCOLUMNW lvc = {0};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        lvc.fmt  = LVCFMT_LEFT;

        lvc.pszText = L"时间";
        lvc.cx = 140;
        SendMessageW(pData->hHistoryList, LVM_INSERTCOLUMNW, 0, (LPARAM)&lvc);

        lvc.pszText = L"NID";
        lvc.cx = 100;
        SendMessageW(pData->hHistoryList, LVM_INSERTCOLUMNW, 1, (LPARAM)&lvc);

        lvc.pszText = L"类型";
        lvc.cx = 80;
        SendMessageW(pData->hHistoryList, LVM_INSERTCOLUMNW, 2, (LPARAM)&lvc);

        lvc.pszText = L"数据";
        lvc.cx = listW - 320;
        if (lvc.cx < 100) lvc.cx = 100;
        SendMessageW(pData->hHistoryList, LVM_INSERTCOLUMNW, 3, (LPARAM)&lvc);

        return 0;
    }

    /* ---- Connection state (from SDK via PostMessage) ---- */
    case WM_LORA_CONN_STATE: {
        enum lora_sdk_conn_state state = (enum lora_sdk_conn_state)wParam;
        switch (state) {
        case LORA_SDK_CONN_DISCONNECTED:
            SetWindowTextW(pData->hStatusText, L"已断开");
            EnableWindow(pData->hBtnConnect, TRUE);
            EnableWindow(pData->hBtnDisconnect, FALSE);
            break;
        case LORA_SDK_CONN_CONNECTING:
            SetWindowTextW(pData->hStatusText, L"连接中...");
            EnableWindow(pData->hBtnConnect, FALSE);
            EnableWindow(pData->hBtnDisconnect, FALSE);
            break;
        case LORA_SDK_CONN_CONNECTED:
            SetWindowTextW(pData->hStatusText, L"已连接");
            EnableWindow(pData->hBtnConnect, FALSE);
            EnableWindow(pData->hBtnDisconnect, TRUE);
            break;
        }
        return 0;
    }

    /* ---- Frame received (from SDK via PostMessage) ---- */
    case WM_LORA_FRAME: {
        LoraFrameMsg *msg = (LoraFrameMsg *)lParam;
        if (!msg) return 0;

        pData->rxCount++;
        UpdateCounters(pData);

        wchar_t timeStr[32];
        GetTimestampStr(timeStr, 32);

        /* Parse frame data by type */
        uint8_t type = (msg->len > 0) ? msg->data[0] : 0xFF;
        const wchar_t *typeStr = L"未知";
        wchar_t dataStr[256] = L"";
        wchar_t typeBuf[32];

        switch (type) {
        case 0x01: { /* HANDLER / Scanner merged data */
            if (msg->len >= 9 && msg->len < LORA_SCANNER_FRAME_SIZE) {
                /* Short telemetry: X(2B BE) + Y(2B BE) + btn(1B) */
                int16_t x = (int16_t)((uint16_t)msg->data[1] << 8 | msg->data[2]);
                int16_t y = (int16_t)((uint16_t)msg->data[3] << 8 | msg->data[4]);
                uint8_t btn = msg->data[5];

                wchar_t xBuf[16], yBuf[16];
                wsprintfW(xBuf, L"%.1f", x / 10.0);
                wsprintfW(yBuf, L"%.1f", y / 10.0);
                SetWindowTextW(pData->hXText, xBuf);
                SetWindowTextW(pData->hYText, yBuf);
                SetWindowTextW(pData->hBtnText, btn ? L"松开" : L"按下");

                typeStr = L"遥测";
                wsprintfW(dataStr, L"X=%s Y=%s btn=%d", xBuf, yBuf, btn);
            } else if (msg->len >= LORA_SCANNER_FRAME_SIZE) {
                /* Merged scanner data (20 bytes) */
                lora_scanner_data_t scanner;
                if (lora_scanner_parse(msg->data, msg->len, &scanner) == 0) {
                    wchar_t xBuf[16], yBuf[16];
                    wsprintfW(xBuf, L"%.1f",
                              (double)((int16_t)((uint16_t)msg->data[1] << 8 | msg->data[2])) / 10.0);
                    wsprintfW(yBuf, L"%.1f",
                              (double)((int16_t)((uint16_t)msg->data[3] << 8 | msg->data[4])) / 10.0);

                    /* Update telemetry display if short frame data present */
                    if (msg->len >= 9) {
                        int16_t tx = (int16_t)((uint16_t)msg->data[1] << 8 | msg->data[2]);
                        int16_t ty = (int16_t)((uint16_t)msg->data[3] << 8 | msg->data[4]);
                        uint8_t tbtn = msg->data[5];
                        wchar_t txBuf[16], tyBuf[16];
                        wsprintfW(txBuf, L"%.1f", tx / 10.0);
                        wsprintfW(tyBuf, L"%.1f", ty / 10.0);
                        SetWindowTextW(pData->hXText, txBuf);
                        SetWindowTextW(pData->hYText, tyBuf);
                        SetWindowTextW(pData->hBtnText, tbtn ? L"松开" : L"按下");
                    }

                    typeStr = L"扫描仪";
                    wsprintfW(dataStr,
                        L"ov=%d ls=%u x=%d y=%d z=%d flags=%02X",
                        scanner.overbreak, scanner.laser,
                        scanner.coord_x, scanner.coord_y, scanner.coord_z,
                        msg->data[1]);
                } else {
                    typeStr = L"数据";
                    /* Format as hex */
                    int pos = 0;
                    for (int i = 0; i < msg->len && pos < 200; i++)
                        pos += wsprintfW(dataStr + pos, L"%02X ", msg->data[i]);
                }
            } else {
                typeStr = L"遥测(短)";
                int pos = 0;
                for (int i = 0; i < msg->len && pos < 200; i++)
                    pos += wsprintfW(dataStr + pos, L"%02X ", msg->data[i]);
            }

            /* Record CSV entry */
            if (pData->csvCount < MAX_CSV_ENTRIES) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                CsvTestEntry *e = &pData->csvEntries[pData->csvCount++];
                e->timestamp = st;
                e->nid = msg->nid;
                e->type = type;
                e->data_len = msg->len > 64 ? 64 : msg->len;
                memcpy(e->data, msg->data, e->data_len);
            }
            break;
        }

        case 0x02: { /* TEST: echo frame back */
            typeStr = L"测试";
            int pos = 0;
            for (int i = 0; i < msg->len && pos < 200; i++)
                pos += wsprintfW(dataStr + pos, L"%02X ", msg->data[i]);

            /* Echo back */
            if (pData->sdk) {
                lora_sdk_send_frame(pData->sdk, msg->nid,
                                    msg->data, msg->len);
                pData->txCount++;
                UpdateCounters(pData);
            }

            /* Record CSV entry */
            if (pData->csvCount < MAX_CSV_ENTRIES) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                CsvTestEntry *e = &pData->csvEntries[pData->csvCount++];
                e->timestamp = st;
                e->nid = msg->nid;
                e->type = type;
                e->data_len = msg->len > 64 ? 64 : msg->len;
                memcpy(e->data, msg->data, e->data_len);
            }
            break;
        }

        case 0x03: { /* RSSI request */
            typeStr = L"RSSI";
            wsprintfW(dataStr, L"RSSI请求");

            /* Trigger SDK to send AT command for RSSI info */
            if (pData->sdk) {
                lora_sdk_send_at(pData->sdk, "AT+NINFO?\r\n");
            }
            break;
        }

        default: {
            wsprintfW(typeBuf, L"0x%02X", type);
            typeStr = typeBuf;
            int pos = 0;
            for (int i = 0; i < msg->len && pos < 200; i++)
                pos += wsprintfW(dataStr + pos, L"%02X ", msg->data[i]);
            break;
        }
        }

        /* Add to history */
        AddHistoryEntry(pData, timeStr, msg->nid, typeStr, dataStr);

        /* Update NID display */
        wchar_t nidBuf[16];
        wsprintfW(nidBuf, L"%08X", msg->nid);
        SetWindowTextW(pData->hNidText, nidBuf);

        free(msg);
        return 0;
    }

    /* ---- Log message (from SDK via PostMessage) ---- */
    case WM_LORA_LOG: {
        char *text = (char *)lParam;
        if (!text) return 0;

        wchar_t timeStr[32];
        GetTimestampStr(timeStr, 32);

        /* Convert UTF-8 to wide string */
        int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
        if (wlen > 0) {
            wchar_t *wtext = (wchar_t *)malloc(wlen * sizeof(wchar_t));
            if (wtext) {
                MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wlen);
                wchar_t line[1024];
                wsprintfW(line, L"[%s] %s\r\n", timeStr, wtext);
                AppendLogText(pData, line);
                free(wtext);
            }
        }

        free(text);
        return 0;
    }

    /* ---- Hex dump (from SDK via PostMessage) ---- */
    case WM_LORA_HEX_DUMP: {
        LoraHexDumpMsg *msg = (LoraHexDumpMsg *)lParam;
        if (!msg) return 0;

        wchar_t timeStr[32];
        GetTimestampStr(timeStr, 32);

        /* Convert prefix to wide */
        int pwlen = MultiByteToWideChar(CP_UTF8, 0, msg->prefix, -1, NULL, 0);
        wchar_t *wprefix = NULL;
        if (pwlen > 0) {
            wprefix = (wchar_t *)malloc(pwlen * sizeof(wchar_t));
            if (wprefix)
                MultiByteToWideChar(CP_UTF8, 0, msg->prefix, -1, wprefix, pwlen);
        }

        /* Format hex data */
        wchar_t hexBuf[1024];
        int pos = 0;
        for (int i = 0; i < msg->len && pos < 900; i++)
            pos += wsprintfW(hexBuf + pos, L"%02X ", msg->data[i]);

        wchar_t line[1200];
        wsprintfW(line, L"[%s] %s: %s\r\n", timeStr,
                  wprefix ? wprefix : L"", hexBuf);
        AppendLogText(pData, line);

        if (wprefix) free(wprefix);
        free(msg);
        return 0;
    }

    /* ---- Command handling ---- */
    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case IDC_LORA_CONNECT_BTN: {
            if (!pData->sdk) return 0;

            wchar_t ipBuf[64];
            GetWindowTextW(pData->hEditIp, ipBuf, 64);

            wchar_t portBuf[16];
            GetWindowTextW(pData->hEditPort, portBuf, 16);
            int port = (int)wcstol(portBuf, NULL, 10);

            /* Convert wide IP to UTF-8 for SDK */
            char ipA[64] = {0};
            WideCharToMultiByte(CP_UTF8, 0, ipBuf, -1, ipA, sizeof(ipA), NULL, NULL);

            lora_sdk_connect(pData->sdk, ipA, port);
            return 0;
        }

        case IDC_LORA_DISCONNECT_BTN: {
            if (pData->sdk)
                lora_sdk_disconnect(pData->sdk);
            return 0;
        }

        case IDC_LORA_SEND_BTN: {
            if (!pData->sdk) return 0;

            wchar_t hexStr[256];
            GetWindowTextW(pData->hSendEdit, hexStr, 256);

            uint8_t data[128];
            int dataLen = ParseHexData(hexStr, data, 128);

            if (dataLen > 0) {
                /* Read NID from display, default to 0 */
                wchar_t nidBuf[16];
                GetWindowTextW(pData->hNidText, nidBuf, 16);
                uint32_t nid = 0;
                if (wcslen(nidBuf) > 0)
                    nid = (uint32_t)wcstoul(nidBuf, NULL, 16);

                lora_sdk_send_frame(pData->sdk, nid, data, (uint16_t)dataLen);
                pData->txCount++;
                UpdateCounters(pData);
            }
            return 0;
        }

        case IDC_LORA_CLEAR_BTN: {
            /* Clear log */
            HWND hLog = pData->hLogEdit;
            ShowWindow(hLog, SW_HIDE);
            SetWindowTextW(hLog, L"");
            RedrawWindow(hLog, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
            ShowWindow(hLog, SW_SHOW);

            /* Reset counters */
            pData->rxCount = 0;
            pData->txCount = 0;
            pData->errCount = 0;
            UpdateCounters(pData);

            /* Clear history */
            SendMessageW(pData->hHistoryList, LVM_DELETEALLITEMS, 0, 0);

            /* Clear CSV entries */
            pData->csvCount = 0;

            /* Reset telemetry */
            SetWindowTextW(pData->hXText, L"--");
            SetWindowTextW(pData->hYText, L"--");
            SetWindowTextW(pData->hBtnText, L"--");
            return 0;
        }

        case IDC_LORA_SAVE_CSV_BTN: {
            if (pData->csvCount == 0) {
                MessageBoxW(hwnd, L"没有测试数据可保存", L"提示",
                            MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            /* Get save file name */
            wchar_t fileName[MAX_PATH] = L"lora_test.csv";
            OPENFILENAMEW ofn;
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hwnd;
            ofn.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile   = fileName;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
            ofn.lpstrTitle  = L"保存测试数据";
            ofn.lpstrDefExt = L"csv";

            if (GetSaveFileNameW(&ofn)) {
                FILE *fp = _wfopen(fileName, L"w");
                if (!fp) {
                    MessageBoxW(hwnd, L"无法创建文件", L"错误",
                                MB_OK | MB_ICONERROR);
                    return 0;
                }

                /* Write BOM for Excel UTF-8 compatibility */
                fprintf(fp, "\xEF\xBB\xBF");

                /* CSV header */
                fprintf(fp, "时间,NID,类型,数据长度,数据(Hex)\r\n");

                /* CSV rows */
                for (int i = 0; i < pData->csvCount; i++) {
                    CsvTestEntry *e = &pData->csvEntries[i];

                    fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d.%03d,",
                            e->timestamp.wYear, e->timestamp.wMonth,
                            e->timestamp.wDay, e->timestamp.wHour,
                            e->timestamp.wMinute, e->timestamp.wSecond,
                            e->timestamp.wMilliseconds);
                    fprintf(fp, "0x%08X,", e->nid);
                    fprintf(fp, "0x%02X,", e->type);
                    fprintf(fp, "%d,", e->data_len);

                    for (int j = 0; j < e->data_len; j++)
                        fprintf(fp, "%02X ", e->data[j]);
                    fprintf(fp, "\r\n");
                }

                fclose(fp);

                wchar_t msgBuf[256];
                wsprintfW(msgBuf, L"已保存 %d 条记录到:\n%s",
                          pData->csvCount, fileName);
                MessageBoxW(hwnd, msgBuf, L"保存成功",
                            MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }

        case IDC_LORA_TEST_CHECK: {
            pData->testMode = (SendMessageW(pData->hTestCheck,
                                             BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            return 0;
        }

        default:
            break;
        }
        break;

    /* ---- Resize: adapt group boxes ---- */
    case WM_SIZE: {
        int cx = LOWORD(lParam);
        int cy = HIWORD(lParam);
        if (cx < 100 || cy < 100) return 0;

        int margin = 14;
        int grp1H = 80;
        int grp2H = 180;
        int grp3H = 50;

        /* Group 1: Connection - full width, fixed height */
        int grp1W = cx - 2 * margin;
        MoveWindow(pData->hGrpConn, margin, margin, grp1W, grp1H, TRUE);

        /* Group 2: Middle split */
        int grp2Y = margin + grp1H + 8;
        int teleW = 280;
        int logW = cx - margin - (margin + teleW + 8) - margin;
        if (logW < 200) logW = 200;

        MoveWindow(pData->hGrpTelemetry, margin, grp2Y, teleW, grp2H, TRUE);
        MoveWindow(pData->hGrpLog, margin + teleW + 8, grp2Y, logW, grp2H, TRUE);

        /* Resize log edit */
        int logEditX = margin + teleW + 8 + 10;
        int logEditY = grp2Y + 24;
        int logEditW = logW - 20;
        int logEditH = grp2H - 34;
        if (logEditW < 50) logEditW = 50;
        if (logEditH < 50) logEditH = 50;
        MoveWindow(pData->hLogEdit, logEditX, logEditY, logEditW, logEditH, TRUE);

        /* Group 3: Operations */
        int grp3Y = grp2Y + grp2H + 8;
        int grp3W = cx - 2 * margin;
        MoveWindow(pData->hGrpOps, margin, grp3Y, grp3W, grp3H, TRUE);

        /* Group 4: History - stretch height */
        int grp4Y = grp3Y + grp3H + 8;
        int grp4H = cy - grp4Y - margin;
        if (grp4H < 100) grp4H = 100;
        int grp4W = cx - 2 * margin;
        MoveWindow(pData->hGrpHistory, margin, grp4Y, grp4W, grp4H, TRUE);

        /* Resize history list view */
        int listX = margin + 10;
        int listY = grp4Y + 24;
        int listW = grp4W - 20;
        int listH = grp4H - 34;
        if (listW < 100) listW = 100;
        if (listH < 50) listH = 50;
        MoveWindow(pData->hHistoryList, listX, listY, listW, listH, TRUE);

        /* Adjust data column width to stretch */
        int dataColW = listW - 320;
        if (dataColW < 100) dataColW = 100;
        SendMessageW(pData->hHistoryList, LVM_SETCOLUMNWIDTH, 3, dataColW);

        return 0;
    }

    /* ---- Cleanup ---- */
    case WM_DESTROY:
        if (pData) {
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
/*  Public API: Create / Destroy                                       */
/* ------------------------------------------------------------------ */

static const wchar_t *TAB_LORA_DATA_CLASS = L"TabLoraDataClass";
static int g_loraDataClassRegistered = 0;

HWND TabLoraData_Create(HWND hParent, HINSTANCE hInst, lora_sdk_t *sdk)
{
    if (!g_loraDataClassRegistered) {
        WNDCLASSEXW wc = { 0 };
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = TabLoraData_WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = TAB_LORA_DATA_CLASS;
        RegisterClassExW(&wc);
        g_loraDataClassRegistered = 1;
    }

    RECT rcParent;
    GetClientRect(hParent, &rcParent);
    TabCtrl_AdjustRect(hParent, FALSE, &rcParent);

    HWND hwnd = CreateWindowExW(
        0,
        TAB_LORA_DATA_CLASS,
        L"",
        WS_CHILD | WS_CLIPCHILDREN,
        rcParent.left, rcParent.top,
        rcParent.right - rcParent.left,
        rcParent.bottom - rcParent.top,
        hParent, NULL, hInst, sdk);

    return hwnd;
}

void TabLoraData_Destroy(HWND hwnd)
{
    if (hwnd)
        DestroyWindow(hwnd);
}
