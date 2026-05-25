/**
 * Tab 1: LoRa Data Page
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
#include "tab_base.h"

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

/* WM_LORA_SEND_FRAME (lParam = heap-allocated, receiver frees) */
typedef struct {
    uint32_t nid;
    uint16_t len;
    uint8_t  data[1]; /* variable length */
} LoraSendMsg;

/* WM_LORA_NET_PARAMS (lParam = heap-allocated, receiver frees) */
typedef struct {
    char ip[64];
    char mask[64];
    char gateway[64];
} LoraNetParamsMsg;

/* CSV test entry */
#define MAX_CSV_ENTRIES  50000
typedef struct {
    uint32_t nid;
    uint16_t index;
    uint32_t dev_ts; /* device uptime ms */
    char time[32];   /* HH:MM:SS.mmm */
} CsvTestEntry;

/* ------------------------------------------------------------------ */
/*  Per-window instance data (stored via GWLP_USERDATA)                */
/* ------------------------------------------------------------------ */
typedef struct {
    TAB_BASE base;          /* MUST be first member */
    lora_sdk_t *sdk;

    /* Connection group */
    HWND hEditIp;
    HWND hEditPort;
    HWND hBtnConnect;
    HWND hNidText;
    HWND hTestCheck;
    HWND hLabelIp;
    HWND hLabelPort;
    HWND hLabelNid;

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

} TAB_LORA_DATA;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */
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

static void LayoutConnControls(TAB_LORA_DATA *pData, int gX, int gW, int gY, int gH) {
    int y = gY + 22 + ((gH - 30) - 26) / 2;
    int lh = 22, ch = 24, bh = 26;
    int w1=28, e1=160, w2=44, e2=70, bw=100, w3=38, n3=100, tw=110;
    int sp=10;
    int x = gX + 14;
    MoveWindow(pData->hLabelIp, x, y+3, w1, lh, TRUE); x += w1;
    MoveWindow(pData->hEditIp, x, y, e1, ch, TRUE); x += e1 + sp;
    MoveWindow(pData->hLabelPort, x, y+3, w2, lh, TRUE); x += w2;
    MoveWindow(pData->hEditPort, x, y, e2, ch, TRUE); x += e2 + sp;
    MoveWindow(pData->hBtnConnect, x, y, bw, bh, TRUE); x += bw + sp;
    MoveWindow(pData->hLabelNid, x, y+3, w3, lh, TRUE); x += w3;
    MoveWindow(pData->hNidText, x, y+3, n3, lh, TRUE); x += n3 + sp;
    MoveWindow(pData->hTestCheck, x, y+2, tw, ch, TRUE);
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
    if (len > 100000) {
        SendMessageW(hLog, EM_SETSEL, 0, len / 4);
        SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(hLog);
    }

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
/*  Tab Base Framework Hooks                                         */
/* ------------------------------------------------------------------ */

static void lora_data_on_create(HWND hwnd, void *data, CREATESTRUCTW *cs)
{
    TAB_LORA_DATA *pData = (TAB_LORA_DATA *)data;
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

    int margin = 14;
    int lineH = 36;
    int cx, cy;

    /* ========== Group 1: Connection (top, full width, ~80px) ========== */
    int grp1X = margin, grp1Y = margin;
    int grp1W = pageW - 2 * margin;
    int grp1H = 70;
    pData->hGrpConn = CreateWindowExW(0, L"BUTTON", L"连接",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        grp1X, grp1Y, grp1W, grp1H, hwnd, NULL, hInst, NULL);

    pData->hLabelIp = CreateLabel(hwnd, hInst, -1, 0, 0, 28, 22, L"IP:", pData->base.hFont);
    pData->hEditIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"192.168.2.100",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 160, 24, hwnd, (HMENU)IDC_LORA_IP_EDIT, hInst, NULL);
    SendMessageW(pData->hEditIp, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);

    pData->hLabelPort = CreateLabel(hwnd, hInst, -1, 0, 0, 44, 22, L"端口:", pData->base.hFont);
    pData->hEditPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"1234",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 70, 24, hwnd, (HMENU)IDC_LORA_PORT_EDIT, hInst, NULL);
    SendMessageW(pData->hEditPort, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);

    pData->hBtnConnect = CreateWindowExW(0, L"BUTTON", L"连接",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 100, 26,
        hwnd, (HMENU)IDC_LORA_CONNECT_BTN, hInst, NULL);
    SendMessageW(pData->hBtnConnect, WM_SETFONT, (WPARAM)pData->base.hFontBold, TRUE);

    pData->hLabelNid = CreateLabel(hwnd, hInst, -1, 0, 0, 38, 22, L"NID:", pData->base.hFont);
    pData->hNidText = CreateWindowExW(0, L"STATIC", L"00000000",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 100, 22,
        hwnd, (HMENU)IDC_LORA_NID_TEXT, hInst, NULL);
    SendMessageW(pData->hNidText, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);

    pData->hTestCheck = CreateWindowExW(0, L"BUTTON", L"测试模式",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0, 0, 110, 24,
        hwnd, (HMENU)IDC_LORA_TEST_CHECK, hInst, NULL);
    SendMessageW(pData->hTestCheck, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    LayoutConnControls(pData, grp1X, grp1W, grp1Y, grp1H);

    /* ========== Group 2: Middle (split left/right) ========== */
    int grp2Y = grp1Y + grp1H + 8;
    int grp2H = 180;

    /* Left: Telemetry (fixed width ~280px) */
    int teleW = 280;
    pData->hGrpTelemetry = CreateWindowExW(0, L"BUTTON", L"手柄数据",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, grp2Y, teleW, grp2H, hwnd, NULL, hInst, NULL);

    cx = margin + 14;
    cy = grp2Y + 28;

    /* X angle */
    CreateLabel(hwnd, hInst, -1, cx, cy + 3, 80, 22, L"X角度:", pData->base.hFont);
    pData->hXText = CreateWindowExW(0, L"STATIC", L"--",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        cx + 84, cy + 3, 120, 22,
        hwnd, (HMENU)IDC_LORA_X_TEXT, hInst, NULL);
    SendMessageW(pData->hXText, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);
    cy += lineH;

    /* Y angle */
    CreateLabel(hwnd, hInst, -1, cx, cy + 3, 80, 22, L"Y角度:", pData->base.hFont);
    pData->hYText = CreateWindowExW(0, L"STATIC", L"--",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        cx + 84, cy + 3, 120, 22,
        hwnd, (HMENU)IDC_LORA_Y_TEXT, hInst, NULL);
    SendMessageW(pData->hYText, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);
    cy += lineH;

    /* Button state */
    CreateLabel(hwnd, hInst, -1, cx, cy + 3, 80, 22, L"按键状态:", pData->base.hFont);
    pData->hBtnText = CreateWindowExW(0, L"STATIC", L"--",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        cx + 84, cy + 3, 120, 22,
        hwnd, (HMENU)IDC_LORA_BTN_TEXT, hInst, NULL);
    SendMessageW(pData->hBtnText, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);
    cy += lineH;

    /* Counters */
    pData->hRxCount = CreateWindowExW(0, L"STATIC", L"RX: 0",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        cx, cy + 3, 80, 22,
        hwnd, (HMENU)IDC_LORA_RX_COUNT, hInst, NULL);
    SendMessageW(pData->hRxCount, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);

    pData->hTxCount = CreateWindowExW(0, L"STATIC", L"TX: 0",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        cx + 80, cy + 3, 80, 22,
        hwnd, (HMENU)IDC_LORA_TX_COUNT, hInst, NULL);
    SendMessageW(pData->hTxCount, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);

    pData->hErrCount = CreateWindowExW(0, L"STATIC", L"ERR: 0",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        cx + 160, cy + 3, 80, 22,
        hwnd, (HMENU)IDC_LORA_ERR_COUNT, hInst, NULL);
    SendMessageW(pData->hErrCount, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);

    /* Right: Raw log (stretch width) */
    int logX = margin + teleW + 8;
    int logW = pageW - margin - logX - margin;
    if (logW < 200) logW = 200;
    pData->hGrpLog = CreateWindowExW(0, L"BUTTON", L"原始日志",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        logX, grp2Y, logW, grp2H, hwnd, NULL, hInst, NULL);

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
    SendMessageW(pData->hLogEdit, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);
    SendMessageW(pData->hLogEdit, EM_LIMITTEXT, 0x7FFFFFFE, 0);

    /* ========== Group 3: Operations (full width, ~40px) ========== */
    int grp3Y = grp2Y + grp2H + 8;
    int grp3H = 50;
    int grp3W = pageW - 2 * margin;
    pData->hGrpOps = CreateWindowExW(0, L"BUTTON", L"操作",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, grp3Y, grp3W, grp3H, hwnd, NULL, hInst, NULL);

    cx = margin + 14;
    cy = grp3Y + 18;

    pData->hSendEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        cx, cy, 400, 24,
        hwnd, (HMENU)IDC_LORA_SEND_EDIT, hInst, NULL);
    SendMessageW(pData->hSendEdit, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);

    int ox = cx + 408;
    pData->hBtnSend = CreateWindowExW(0, L"BUTTON", L"发送",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        ox, cy, 80, 26,
        hwnd, (HMENU)IDC_LORA_SEND_BTN, hInst, NULL);
    SendMessageW(pData->hBtnSend, WM_SETFONT, (WPARAM)pData->base.hFontBold, TRUE);

    ox += 86;
    pData->hBtnSaveCsv = CreateWindowExW(0, L"BUTTON", L"保存CSV",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        ox, cy, 90, 26,
        hwnd, (HMENU)IDC_LORA_SAVE_CSV_BTN, hInst, NULL);
    SendMessageW(pData->hBtnSaveCsv, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    ox += 96;
    pData->hBtnClear = CreateWindowExW(0, L"BUTTON", L"清除",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        ox, cy, 80, 26,
        hwnd, (HMENU)IDC_LORA_CLEAR_BTN, hInst, NULL);
    SendMessageW(pData->hBtnClear, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    /* ========== Group 4: History ListView (bottom, stretch height) ========== */
    int grp4Y = grp3Y + grp3H + 8;
    int grp4H = pageH - grp4Y - margin;
    if (grp4H < 100) grp4H = 100;
    int grp4W = pageW - 2 * margin;
    pData->hGrpHistory = CreateWindowExW(0, L"BUTTON", L"历史记录",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, grp4Y, grp4W, grp4H, hwnd, NULL, hInst, NULL);

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
    SendMessageW(pData->hHistoryList, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

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
    lvc.cx = 200;
    SendMessageW(pData->hHistoryList, LVM_INSERTCOLUMNW, 3, (LPARAM)&lvc);

    RECT rcList0;
    GetClientRect(pData->hHistoryList, &rcList0);
    int cw0 = rcList0.right - rcList0.left;
    int dw0 = cw0 - 320;
    if (dw0 < 100) dw0 = 100;
    SendMessageW(pData->hHistoryList, LVM_SETCOLUMNWIDTH, 3, dw0);
}
static void lora_data_on_size(HWND hwnd, void *data, int cx, int cy)
{
    TAB_LORA_DATA *pData = (TAB_LORA_DATA *)data;

    int margin = 14;
    int grp1H = 70;
    int grp2H = 180;
    int grp3H = 50;

    int grp1W = cx - 2 * margin;
    MoveWindow(pData->hGrpConn, margin, margin, grp1W, grp1H, TRUE);
    LayoutConnControls(pData, margin, grp1W, margin, grp1H);

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

    RECT rcList;
    GetClientRect(pData->hHistoryList, &rcList);
    int clientW = rcList.right - rcList.left;
    int dataColW = clientW - 320;
    if (dataColW < 100) dataColW = 100;
    SendMessageW(pData->hHistoryList, LVM_SETCOLUMNWIDTH, 3, dataColW);
}

static void lora_data_on_destroy(HWND hwnd, void *data)
{
    (void)hwnd;
    (void)data;
    /* No special cleanup; framework handles font deletion and free */
}
static LRESULT lora_data_on_message(HWND hwnd, void *data, UINT uMsg,
                                     WPARAM wParam, LPARAM lParam)
{
    TAB_LORA_DATA *pData = (TAB_LORA_DATA *)data;

    switch (uMsg) {

    /* ---- Connection state (from SDK via PostMessage) ---- */
    case WM_LORA_CONN_STATE: {
        enum lora_sdk_conn_state state = (enum lora_sdk_conn_state)wParam;
        switch (state) {
        case LORA_SDK_CONN_DISCONNECTED:
            SetWindowTextW(pData->hBtnConnect, L"连接");
            EnableWindow(pData->hBtnConnect, TRUE);
            break;
        case LORA_SDK_CONN_CONNECTING:
            SetWindowTextW(pData->hBtnConnect, L"连接中");
            EnableWindow(pData->hBtnConnect, FALSE);
            break;
        case LORA_SDK_CONN_CONNECTED:
            SetWindowTextW(pData->hBtnConnect, L"断开");
            EnableWindow(pData->hBtnConnect, TRUE);
            break;
        }
        return TAB_MSG_HANDLED;
    }

    /* ---- Network params (from SDK via PostMessage) ---- */
    case WM_LORA_NET_PARAMS: {
        LoraNetParamsMsg *msg = (LoraNetParamsMsg *)lParam;
        if (msg) {
            wchar_t wbuf[128];
            MultiByteToWideChar(CP_ACP, 0, msg->ip, -1, wbuf, 128);
            SetWindowTextW(pData->hEditIp, wbuf);
            free(msg);
        }
        return TAB_MSG_HANDLED;
    }

    /* ---- Frame received (from SDK via PostMessage) ---- */
    case WM_LORA_FRAME: {
        LoraFrameMsg *msg = (LoraFrameMsg *)lParam;
        if (!msg) return TAB_MSG_HANDLED;

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
            uint16_t body_len = msg->len - 1;
            const uint8_t *body = msg->data + 1;

            if (body_len == 8 &&
                body[5] == 0xFF && body[6] == 0xFF && body[7] == 0xFF) {
                int16_t x = (int16_t)((uint16_t)body[0] << 8 | body[1]);
                int16_t y = (int16_t)((uint16_t)body[2] << 8 | body[3]);
                uint8_t btn = body[4] & 0x01;

                wchar_t xBuf[16], yBuf[16];
                wsprintfW(xBuf, L"%d", x);
                wsprintfW(yBuf, L"%d", y);
                SetWindowTextW(pData->hXText, xBuf);
                SetWindowTextW(pData->hYText, yBuf);
                SetWindowTextW(pData->hBtnText, btn ? L"松开" : L"按下");

                typeStr = L"Telemetry";
                wsprintfW(dataStr, L"X=%d Y=%d Btn=%s",
                          x, y, btn ? L"Released" : L"Pressed");

                /* 收到遥测后回发模拟扫描仪合并帧 */
                if (pData->sdk) {
                    lora_scanner_data_t scan = {
                        .overbreak_valid = 1,
                        .laser_valid     = 1,
                        .coord_z_valid   = 1,
                        .coord_xy_valid  = 1,
                        .overbreak = (int16_t)(rand() % 200 - 100),
                        .laser     = (uint32_t)(rand() % 50000 + 1000),
                        .coord_x   = (int32_t)(rand() % 10000 - 5000),
                        .coord_y   = (int32_t)(rand() % 10000 - 5000),
                        .coord_z   = (int32_t)(rand() % 5000),
                    };
                    LoraSendMsg *smsg = (LoraSendMsg *)malloc(
                        offsetof(LoraSendMsg, data) + LORA_SCANNER_FRAME_SIZE);
                    if (smsg) {
                        smsg->nid = msg->nid;
                        smsg->len = LORA_SCANNER_FRAME_SIZE;
                        lora_scanner_pack(smsg->data, LORA_SCANNER_FRAME_SIZE, &scan);
                        PostMessageW(hwnd, WM_LORA_SEND_FRAME, 0, (LPARAM)smsg);
                    }
                }
            }
            break;
        }

        case 0x02: { /* TEST: echo frame back */
            typeStr = L"测试";

            /* Echo back - deferred via PostMessage */
            if (pData->sdk) {
                LoraSendMsg *smsg = (LoraSendMsg *)malloc(
                    offsetof(LoraSendMsg, data) + msg->len);
                if (smsg) {
                    smsg->nid = msg->nid;
                    smsg->len = msg->len;
                    memcpy(smsg->data, msg->data, msg->len);
                    PostMessageW(hwnd, WM_LORA_SEND_FRAME, 0, (LPARAM)smsg);
                }
            }

            uint16_t body_len = msg->len - 1;
            const uint8_t *body = msg->data + 1;

            if (body_len >= 6) {
                uint16_t idx = (uint16_t)((uint16_t)body[0] << 8 | body[1]);
                uint32_t ts  = (uint32_t)((uint32_t)body[2] << 24 |
                                          (uint32_t)body[3] << 16 |
                                          (uint32_t)body[4] << 8 | body[5]);
                wsprintfW(dataStr, L"idx=%u ts=%u ms -> echo", idx, ts);

                if (pData->csvCount < MAX_CSV_ENTRIES) {
                    CsvTestEntry *e = &pData->csvEntries[pData->csvCount++];
                    e->nid = msg->nid;
                    e->index = idx;
                    e->dev_ts = ts;
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    snprintf(e->time, sizeof(e->time), "%02d:%02d:%02d.%03d",
                             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
                }
            } else if (body_len >= 2) {
                uint16_t idx = (uint16_t)((uint16_t)body[0] << 8 | body[1]);
                wsprintfW(dataStr, L"index=%u -> echo", idx);

                if (pData->csvCount < MAX_CSV_ENTRIES) {
                    CsvTestEntry *e = &pData->csvEntries[pData->csvCount++];
                    e->nid = msg->nid;
                    e->index = idx;
                    e->dev_ts = 0;
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    snprintf(e->time, sizeof(e->time), "%02d:%02d:%02d.%03d",
                             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
                }
            } else {
                wsprintfW(dataStr, L"TEST (short)");
                int pos = 0;
                for (int i = 0; i < msg->len && pos < 200; i++)
                    pos += wsprintfW(dataStr + pos, L"%02X ", msg->data[i]);
            }
            break;
        }

        case 0x03: { /* RSSI request */
            typeStr = L"RSSI";
            wsprintfW(dataStr, L"RSSI请求");

            if (pData->sdk) {
                lora_sdk_query_rssi(pData->sdk, msg->nid);
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

        AddHistoryEntry(pData, timeStr, msg->nid, typeStr, dataStr);

        wchar_t nidBuf[16];
        wsprintfW(nidBuf, L"%08X", msg->nid);
        SetWindowTextW(pData->hNidText, nidBuf);

        free(msg);
        return TAB_MSG_HANDLED;
    }

    /* ---- Deferred frame send (posted by WM_LORA_FRAME handler) ---- */
    case WM_LORA_SEND_FRAME: {
        LoraSendMsg *smsg = (LoraSendMsg *)lParam;
        if (!smsg) return TAB_MSG_HANDLED;
        if (pData && pData->sdk) {
            lora_sdk_send_frame(pData->sdk, smsg->nid,
                                smsg->data, smsg->len);
            pData->txCount++;
            UpdateCounters(pData);
        }
        free(smsg);
        return TAB_MSG_HANDLED;
    }

    /* ---- Log message (from SDK via PostMessage) ---- */
    case WM_LORA_LOG: {
        char *text = (char *)lParam;
        if (!text) return TAB_MSG_HANDLED;

        wchar_t timeStr[32];
        GetTimestampStr(timeStr, 32);

        int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
        if (wlen > 0) {
            wchar_t *wtext = (wchar_t *)malloc(wlen * sizeof(wchar_t));
            if (wtext) {
                MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wlen);
                int lineCap = 16 + wlen + 4;
                wchar_t *line = (wchar_t *)malloc(lineCap * sizeof(wchar_t));
                if (line) {
                    _snwprintf(line, lineCap, L"[%s] %s\r\n", timeStr, wtext);
                    line[lineCap - 1] = L'\0';
                    AppendLogText(pData, line);
                    free(line);
                }
                free(wtext);
            }
        }

        free(text);
        return TAB_MSG_HANDLED;
    }

    /* ---- Hex dump (from SDK via PostMessage) ---- */
    case WM_LORA_HEX_DUMP: {
        LoraHexDumpMsg *msg = (LoraHexDumpMsg *)lParam;
        if (!msg) return TAB_MSG_HANDLED;

        wchar_t timeStr[32];
        GetTimestampStr(timeStr, 32);

        int pwlen = MultiByteToWideChar(CP_UTF8, 0, msg->prefix, -1, NULL, 0);
        wchar_t *wprefix = NULL;
        if (pwlen > 0) {
            wprefix = (wchar_t *)malloc(pwlen * sizeof(wchar_t));
            if (wprefix)
                MultiByteToWideChar(CP_UTF8, 0, msg->prefix, -1, wprefix, pwlen);
        }

        int hexCap = msg->len * 3 + 4;
        wchar_t *hexBuf = (wchar_t *)malloc(hexCap * sizeof(wchar_t));
        if (hexBuf) {
            int pos = 0;
            for (int i = 0; i < msg->len && pos < hexCap - 4; i++)
                pos += _snwprintf(hexBuf + pos, hexCap - pos, L"%02X ", msg->data[i]);

            int lineCap = 16 + (wprefix ? pwlen : 1) + pos + 8;
            wchar_t *line = (wchar_t *)malloc(lineCap * sizeof(wchar_t));
            if (line) {
                _snwprintf(line, lineCap, L"[%s] %s: %s\r\n", timeStr,
                           wprefix ? wprefix : L"", hexBuf);
                line[lineCap - 1] = L'\0';
                AppendLogText(pData, line);
                free(line);
            }
            free(hexBuf);
        }

        if (wprefix) free(wprefix);
        free(msg);
        return TAB_MSG_HANDLED;
    }

    /* ---- Command handling ---- */
    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case IDC_LORA_CONNECT_BTN: {
            if (!pData->sdk) return TAB_MSG_HANDLED;

            wchar_t btnText[16] = {0};
            GetWindowTextW(pData->hBtnConnect, btnText, 16);

            if (wcsstr(btnText, L"连接") && !wcsstr(btnText, L"中")) {
                wchar_t ipBuf[64];
                GetWindowTextW(pData->hEditIp, ipBuf, 64);

                wchar_t portBuf[16];
                GetWindowTextW(pData->hEditPort, portBuf, 16);
                int port = (int)wcstol(portBuf, NULL, 10);

                char ipA[64] = {0};
                WideCharToMultiByte(CP_UTF8, 0, ipBuf, -1, ipA, sizeof(ipA), NULL, NULL);

                lora_sdk_connect(pData->sdk, ipA, port);
            } else {
                lora_sdk_disconnect(pData->sdk);
            }
            return TAB_MSG_HANDLED;
        }

        case IDC_LORA_SEND_BTN: {
            if (!pData->sdk) return TAB_MSG_HANDLED;

            wchar_t hexStr[256];
            GetWindowTextW(pData->hSendEdit, hexStr, 256);

            uint8_t data[128];
            int dataLen = ParseHexData(hexStr, data, 128);

            if (dataLen > 0) {
                wchar_t nidBuf[16];
                GetWindowTextW(pData->hNidText, nidBuf, 16);
                uint32_t nid = 0;
                if (wcslen(nidBuf) > 0)
                    nid = (uint32_t)wcstoul(nidBuf, NULL, 16);

                lora_sdk_send_frame(pData->sdk, nid, data, (uint16_t)dataLen);
                pData->txCount++;
                UpdateCounters(pData);
            }
            return TAB_MSG_HANDLED;
        }

        case IDC_LORA_CLEAR_BTN: {
            HWND hLog = pData->hLogEdit;
            ShowWindow(hLog, SW_HIDE);
            SetWindowTextW(hLog, L"");
            RedrawWindow(hLog, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
            ShowWindow(hLog, SW_SHOW);

            pData->rxCount = 0;
            pData->txCount = 0;
            pData->errCount = 0;
            UpdateCounters(pData);

            SendMessageW(pData->hHistoryList, LVM_DELETEALLITEMS, 0, 0);

            pData->csvCount = 0;

            SetWindowTextW(pData->hXText, L"--");
            SetWindowTextW(pData->hYText, L"--");
            SetWindowTextW(pData->hBtnText, L"--");
            return TAB_MSG_HANDLED;
        }

        case IDC_LORA_SAVE_CSV_BTN: {
            if (pData->csvCount == 0) {
                MessageBoxW(hwnd, L"没有测试数据可保存", L"提示",
                            MB_OK | MB_ICONINFORMATION);
                return TAB_MSG_HANDLED;
            }

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
                    return TAB_MSG_HANDLED;
                }

                fprintf(fp, "\xEF\xBB\xBF");
                fprintf(fp, "Index,Time,NID,DevTimestamp_ms\r\n");

                for (int i = 0; i < pData->csvCount; i++) {
                    CsvTestEntry *e = &pData->csvEntries[i];
                    fprintf(fp, "%u,%s,%08X,%u\n",
                            e->index, e->time, e->nid, e->dev_ts);
                }

                fclose(fp);

                wchar_t msgBuf[256];
                wsprintfW(msgBuf, L"已保存 %d 条记录到:\n%s",
                          pData->csvCount, fileName);
                MessageBoxW(hwnd, msgBuf, L"保存成功",
                            MB_OK | MB_ICONINFORMATION);
            }
            return TAB_MSG_HANDLED;
        }

        case IDC_LORA_TEST_CHECK: {
            pData->testMode = (SendMessageW(pData->hTestCheck,
                                             BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            if (pData->sdk)
                lora_sdk_set_test_flag(pData->sdk, pData->testMode);
            return TAB_MSG_HANDLED;
        }

        default:
            break;
        }
        return TAB_MSG_HANDLED;

    } /* end switch */

    return TAB_MSG_NOT_HANDLED;
}

/* ------------------------------------------------------------------ */
/*  Vtable + Public API                                               */
/* ------------------------------------------------------------------ */

static const TAB_IFACE g_lora_data_iface = {
    .data_size  = sizeof(TAB_LORA_DATA),
    .on_create  = lora_data_on_create,
    .on_size    = lora_data_on_size,
    .on_destroy = lora_data_on_destroy,
    .on_message = lora_data_on_message,
};

HWND TabLoraData_Create(HWND hParent, HINSTANCE hInst, lora_sdk_t *sdk)
{
    return TabBase_CreatePage(hParent, hInst, &g_lora_data_iface, sdk);
}
