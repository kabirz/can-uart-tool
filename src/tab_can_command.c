/**
 * Tab 2: CAN Command Sending Page
 *
 * Custom frame sending, LoRa configuration, and bus monitor
 * with heartbeat frame display and LoRa response parsing.
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"
#include "can_command.h"

/* ------------------------------------------------------------------ */
/*  Known CAN IDs from mod-can.h protocol                              */
/* ------------------------------------------------------------------ */
#define CANID_PLATFORM_RX     0x101
#define CANID_PLATFORM_TX     0x102
#define CANID_FW_DATA_RX      0x103
#define CANID_HEARTBEAT       0x763
#define CANID_HANDLER_STATE   0x1E3
#define CANID_LASER           0x263
#define CANID_COORD_XY        0x363
#define CANID_COORD_Z         0x463
#define CANID_LORA_RX         0x105
#define CANID_LORA_TX         0x106

/* LoRa config commands (from mod-can.h) */
#define LORA_CMD_SET          0x01
#define LORA_CMD_QUERY        0x02
#define LORA_CMD_QUERY_NID    0x03
#define LORA_CMD_SET_NID      0x04
#define LORA_CMD_QUERY_GWID   0x05
#define LORA_CMD_SET_GWID     0x06

/* ------------------------------------------------------------------ */
/*  Per-window instance data (stored via GWLP_USERDATA)                */
/* ------------------------------------------------------------------ */
typedef struct {
    CanCommand  *canCmd;
    int          channel;
    int          isActive;

    /* Frame config controls */
    HWND hEditCanId;
    HWND hRadioStdFrame;
    HWND hRadioExtFrame;
    HWND hRadioDataFrame;
    HWND hRadioRemoteFrame;
    HWND hEditCanData;
    HWND hBtnSend;

    /* Bus monitor controls */
    HWND hEditMonitor;
    HWND hCheckAutoScroll;
    HWND hBtnClearMonitor;

    /* LoRa config controls */
    HWND hComboProt;
    HWND hComboMode;
    HWND hComboSpd;
    HWND hComboCh;
    HWND hEditNid;
    HWND hEditGwid;
    HWND hLabelLoraStatus;
    uint8_t lastLoraCmd;

    /* Fonts */
    HFONT hFont;
    HFONT hFontBold;
    HFONT hFontMono;
} TAB_CMD_DATA;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */
static TAB_CMD_DATA *GetTabPageData(HWND hwnd)
{
    return (TAB_CMD_DATA *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
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

/* Big-endian helpers */
static void PutBE32(uint32_t val, uint8_t *buf)
{
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
}

static uint32_t GetBE32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
}

/* Get description string for known CAN IDs */
static const wchar_t *GetFrameDesc(uint32_t can_id)
{
    switch (can_id) {
    case CANID_HEARTBEAT:     return L" [心跳]";
    case CANID_LORA_RX:       return L" [LoRa发送]";
    case CANID_LORA_TX:       return L" [LoRa响应]";
    case CANID_PLATFORM_RX:   return L" [平台RX]";
    case CANID_PLATFORM_TX:   return L" [平台TX]";
    case CANID_FW_DATA_RX:    return L" [固件数据]";
    case CANID_HANDLER_STATE: return L" [手柄状态]";
    case CANID_LASER:         return L" [激光]";
    case CANID_COORD_XY:      return L" [坐标XY]";
    case CANID_COORD_Z:       return L" [坐标Z]";
    default:                  return L"";
    }
}

/* Append a timestamped CAN frame line to the monitor edit control */
static void AppendMonitorLine(TAB_CMD_DATA *pData, int is_tx,
                               uint32_t can_id, const uint8_t *data, int dlc)
{
    HWND hMon = pData->hEditMonitor;
    if (!hMon) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t line[512];
    int pos = 0;

    /* Direction + timestamp + CAN ID + description */
    pos += wsprintfW(line + pos, L"%s %02d:%02d:%02d  0x%03X",
                     is_tx ? L"TX" : L"RX",
                     st.wHour, st.wMinute, st.wSecond, can_id);
    pos += wsprintfW(line + pos, L"%s", GetFrameDesc(can_id));

    /* Data bytes */
    if (data && dlc > 0) {
        for (int i = 0; i < dlc && i < 8; i++) {
            pos += wsprintfW(line + pos, L" %02X", data[i]);
        }
    }

    /* LoRa response: show OK/FAIL */
    if (can_id == CANID_LORA_TX && dlc > 0) {
        pos += wsprintfW(line + pos, L" %s",
                         data[0] == 0x00 ? L"OK" : L"FAIL");
    }

    wcscat(line + pos, L"\r\n");

    int len = GetWindowTextLengthW(hMon);
    SendMessageW(hMon, EM_SETSEL, len, len);
    SendMessageW(hMon, EM_REPLACESEL, FALSE, (LPARAM)line);

    /* Auto-scroll if checked */
    if (SendMessageW(pData->hCheckAutoScroll, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        SendMessageW(hMon, EM_SCROLLCARET, 0, 0);
    }
}

/* Frame info struct for passing data from callback to UI thread */
typedef struct {
    uint32_t id;
    uint8_t  data[8];
    int      dlc;
    int      is_tx;
} FrameInfo;

/* Frame callback -- called from monitor thread */
static void CanFrameCb(uint32_t id, const uint8_t *data,
                        int dlc, int is_tx, void *context)
{
    HWND hwnd = (HWND)context;

    FrameInfo *fi = (FrameInfo *)malloc(sizeof(FrameInfo));
    if (!fi) return;

    fi->id = id;
    fi->dlc = dlc;
    fi->is_tx = is_tx;
    memset(fi->data, 0, 8);
    if (data && dlc > 0)
        memcpy(fi->data, data, dlc > 8 ? 8 : dlc);

    PostMessage(hwnd, WM_CAN_FRAME_RECEIVED, 0, (LPARAM)fi);
}

/* Update enable/disable state of controls based on channel */
static void UpdateControlStates(TAB_CMD_DATA *pData)
{
    BOOL connected = (pData->channel != CAN_HAL_INVALID_HANDLE);
    EnableWindow(pData->hBtnSend, connected);
    EnableWindow(pData->hEditCanId, connected);
    EnableWindow(pData->hEditCanData, connected);
    EnableWindow(pData->hComboProt, connected);
    EnableWindow(pData->hComboMode, connected);
    EnableWindow(pData->hComboSpd, connected);
    EnableWindow(pData->hComboCh, connected);
    EnableWindow(pData->hEditNid, connected);
    EnableWindow(pData->hEditGwid, connected);
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

/* Send a LoRa config CAN frame to device */
static void SendLoraCommand(TAB_CMD_DATA *pData, uint8_t cmd)
{
    if (!pData->canCmd || pData->channel == CAN_HAL_INVALID_HANDLE) return;

    uint8_t data[8] = {0};
    int dlc = 1;
    data[0] = cmd;

    wchar_t buf[32];

    if (cmd == LORA_CMD_SET) {
        int prot = (int)SendMessageW(pData->hComboProt, CB_GETCURSEL, 0, 0);
        int mode = (int)SendMessageW(pData->hComboMode, CB_GETCURSEL, 0, 0);
        int spd_idx = (int)SendMessageW(pData->hComboSpd, CB_GETCURSEL, 0, 0);
        int spd = spd_idx + 4;  /* values 4-11 */
        int ch = (int)SendMessageW(pData->hComboCh, CB_GETCURSEL, 0, 0);
        data[1] = (uint8_t)((prot << 4) | (mode & 0x0F));
        data[2] = spd;
        data[3] = ch;
        dlc = 4;
    } else if (cmd == LORA_CMD_SET_NID) {
        GetWindowTextW(pData->hEditNid, buf, 32);
        uint32_t nid = (uint32_t)wcstoul(buf, NULL, 16);
        PutBE32(nid, &data[4]);
        dlc = 8;
    } else if (cmd == LORA_CMD_SET_GWID) {
        GetWindowTextW(pData->hEditGwid, buf, 32);
        uint32_t gwid = (uint32_t)wcstoul(buf, NULL, 16);
        PutBE32(gwid, &data[4]);
        dlc = 8;
    }

    pData->lastLoraCmd = cmd;
    SetWindowTextW(pData->hLabelLoraStatus, L"等待响应...");

    CanCommand_SendFrame(pData->canCmd, CANID_LORA_RX, data, dlc, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  WndProc for the tab page                                          */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK TabCanCommand_WndProc(HWND hwnd, UINT uMsg,
                                               WPARAM wParam, LPARAM lParam)
{
    TAB_CMD_DATA *pData = GetTabPageData(hwnd);

    switch (uMsg) {

    /* ---- Creation ---- */
    case WM_NCCREATE: {
        pData = (TAB_CMD_DATA *)calloc(1, sizeof(TAB_CMD_DATA));
        if (!pData) return FALSE;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pData);
        return TRUE;
    }

    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        HINSTANCE hInst = cs->hInstance;

        pData->canCmd = (CanCommand *)cs->lpCreateParams;
        pData->channel = CAN_HAL_INVALID_HANDLE;
        pData->isActive = 0;
        pData->lastLoraCmd = 0;

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

        /* ========== Group 1: Frame Configuration (left, upper) ========== */
        int grp1X = margin, grp1Y = margin;
        int grp1W = 480, grp1H = 260;
        CreateWindowExW(0, L"BUTTON", L"帧配置",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            grp1X, grp1Y, grp1W, grp1H, hwnd, NULL, hInst, NULL);

        cx = grp1X + 14;
        cy = grp1Y + 30;

        /* Row 1: CAN ID */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 70, 22, L"CAN ID:", pData->hFont);
        pData->hEditCanId = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"101",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            cx + 78, cy, 200, 24, hwnd, (HMENU)IDC_EDIT_CAN_ID, hInst, NULL);
        SendMessageW(pData->hEditCanId, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        CreateLabel(hwnd, hInst, -1, cx + 286, cy + 3, 50, 22, L"(Hex)", pData->hFont);
        cy += lineH;

        /* Row 2: Frame Format */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 70, 22,
            L"帧格式:", pData->hFont);
        pData->hRadioStdFrame = CreateWindowExW(0, L"BUTTON",
            L"标准帧",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            cx + 78, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_STD_FRAME, hInst, NULL);
        SendMessageW(pData->hRadioStdFrame, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        pData->hRadioExtFrame = CreateWindowExW(0, L"BUTTON",
            L"扩展帧",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            cx + 174, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_EXT_FRAME, hInst, NULL);
        SendMessageW(pData->hRadioExtFrame, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        cy += lineH;

        /* Row 3: Frame Type */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 70, 22,
            L"帧类型:", pData->hFont);
        pData->hRadioDataFrame = CreateWindowExW(0, L"BUTTON",
            L"数据帧",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            cx + 78, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_DATA_FRAME, hInst, NULL);
        SendMessageW(pData->hRadioDataFrame, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        pData->hRadioRemoteFrame = CreateWindowExW(0, L"BUTTON",
            L"远程帧",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            cx + 174, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_REMOTE_FRAME, hInst, NULL);
        SendMessageW(pData->hRadioRemoteFrame, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        cy += lineH;

        /* Row 4: Data bytes */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 70, 22,
            L"数据:", pData->hFont);
        pData->hEditCanData = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            L"01 02 03 04 05 06 07 08",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            cx + 78, cy, 300, 24, hwnd, (HMENU)IDC_EDIT_CAN_DATA, hInst, NULL);
        SendMessageW(pData->hEditCanData, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);
        cy += lineH + 4;

        /* Row 5: Send button */
        pData->hBtnSend = CreateWindowExW(0, L"BUTTON",
            L"发送帧",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            grp1X + grp1W - 14 - 140, cy, 140, 32,
            hwnd, (HMENU)IDC_BUTTON_CAN_SEND, hInst, NULL);
        SendMessageW(pData->hBtnSend, WM_SETFONT, (WPARAM)pData->hFontBold, TRUE);

        /* ========== Group 2: LoRa Configuration (right, upper) ========== */
        int grp2X = grp1X + grp1W + 10;
        int grp2Y = grp1Y;
        int grp2W = WINDOW_WIDTH - margin - grp2X - margin;
        int grp2H = grp1H;
        CreateWindowExW(0, L"BUTTON", L"LoRa 配置",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            grp2X, grp2Y, grp2W, grp2H, hwnd, NULL, hInst, NULL);

        cx = grp2X + 14;
        cy = grp2Y + 30;

        /* Row 1: Protocol / Mode */
        int llW = 60, cbW = 220, cbGap = 20;

        CreateLabel(hwnd, hInst, -1, cx, cy + 3, llW, 22,
            L"协议:", pData->hFont);
        pData->hComboProt = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + llW + 6, cy, cbW, 200,
            hwnd, (HMENU)IDC_EDIT_LORA_PROT, hInst, NULL);
        SendMessageW(pData->hComboProt, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        SendMessageW(pData->hComboProt, CB_ADDSTRING, 0, (LPARAM)L"节点 (0)");
        SendMessageW(pData->hComboProt, CB_ADDSTRING, 0, (LPARAM)L"LG210 (1)");
        SendMessageW(pData->hComboProt, CB_ADDSTRING, 0, (LPARAM)L"LG220 (2)");
        SendMessageW(pData->hComboProt, CB_SETCURSEL, 1, 0); /* default LG210 */

        int ox2 = cx + llW + 6 + cbW + cbGap;

        CreateLabel(hwnd, hInst, -1, ox2, cy + 3, llW, 22,
            L"模式:", pData->hFont);
        pData->hComboMode = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ox2 + llW + 6, cy, cbW, 200,
            hwnd, (HMENU)IDC_EDIT_LORA_MODE, hInst, NULL);
        SendMessageW(pData->hComboMode, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        SendMessageW(pData->hComboMode, CB_ADDSTRING, 0, (LPARAM)L"点对点 (0)");
        SendMessageW(pData->hComboMode, CB_ADDSTRING, 0, (LPARAM)L"LG210组网 (1)");
        SendMessageW(pData->hComboMode, CB_ADDSTRING, 0, (LPARAM)L"LG220组网 (2)");
        SendMessageW(pData->hComboMode, CB_SETCURSEL, 1, 0); /* default LG210组网 */
        cy += lineH;

        /* Row 2: Speed / Channel */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, llW, 22,
            L"速率:", pData->hFont);
        pData->hComboSpd = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + llW + 6, cy, cbW, 200,
            hwnd, (HMENU)IDC_EDIT_LORA_SPD, hInst, NULL);
        SendMessageW(pData->hComboSpd, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        for (int s = 4; s <= 11; s++) {
            wchar_t sbuf[8];
            wsprintfW(sbuf, L"%d", s);
            SendMessageW(pData->hComboSpd, CB_ADDSTRING, 0, (LPARAM)sbuf);
        }
        SendMessageW(pData->hComboSpd, CB_SETCURSEL, 3, 0); /* default 7 */

        CreateLabel(hwnd, hInst, -1, ox2, cy + 3, llW, 22,
            L"通道:", pData->hFont);
        pData->hComboCh = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ox2 + llW + 6, cy, cbW, 200,
            hwnd, (HMENU)IDC_EDIT_LORA_CH, hInst, NULL);
        SendMessageW(pData->hComboCh, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        SendMessageW(pData->hComboCh, CB_ADDSTRING, 0, (LPARAM)L"0");
        SendMessageW(pData->hComboCh, CB_ADDSTRING, 0, (LPARAM)L"1");
        SendMessageW(pData->hComboCh, CB_ADDSTRING, 0, (LPARAM)L"2");
        SendMessageW(pData->hComboCh, CB_SETCURSEL, 0, 0); /* default 0 */
        cy += lineH;

        /* Row 2: NID / GWID (hex input) */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 40, 22,
            L"NID:", pData->hFont);
        pData->hEditNid = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"00000000",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            cx + 46, cy, 140, 24,
            hwnd, (HMENU)IDC_EDIT_LORA_NID, hInst, NULL);
        SendMessageW(pData->hEditNid, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        CreateLabel(hwnd, hInst, -1, cx + 200, cy + 3, 50, 22,
            L"GWID:", pData->hFont);
        pData->hEditGwid = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"00000000",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            cx + 256, cy, 140, 24,
            hwnd, (HMENU)IDC_EDIT_LORA_GWID, hInst, NULL);
        SendMessageW(pData->hEditGwid, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);
        cy += lineH + 4;

        /* Row 3: Config + NID buttons */
        int bw = 140, bh = 30, bg = 10;
        HWND hBtn;

        hBtn = CreateWindowExW(0, L"BUTTON",
            L"查询配置",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx, cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_QUERY_CFG, hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        hBtn = CreateWindowExW(0, L"BUTTON",
            L"设置配置",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + bw + bg, cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_SET_CFG, hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        hBtn = CreateWindowExW(0, L"BUTTON",
            L"查询 NID",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + 2 * (bw + bg), cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_QUERY_NID, hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        cy += bh + 6;

        /* Row 4: GWID + Set NID buttons */
        hBtn = CreateWindowExW(0, L"BUTTON",
            L"设置 NID",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx, cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_SET_NID, hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        hBtn = CreateWindowExW(0, L"BUTTON",
            L"查询 GWID",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + bw + bg, cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_QUERY_GWID, hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        hBtn = CreateWindowExW(0, L"BUTTON",
            L"设置 GWID",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + 2 * (bw + bg), cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_SET_GWID, hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        cy += bh + 6;

        /* Status label */
        pData->hLabelLoraStatus = CreateWindowExW(0, L"STATIC",
            L"就绪",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx, cy + 2, grp2W - 28, 22,
            hwnd, (HMENU)IDC_LABEL_LORA_STATUS, hInst, NULL);
        SendMessageW(pData->hLabelLoraStatus, WM_SETFONT,
            (WPARAM)pData->hFont, TRUE);

        /* ========== Group 3: Bus Monitor (full width, bottom) ========== */
        int grp3X = margin;
        int grp3Y = grp1Y + grp1H + 10;
        int grp3W = WINDOW_WIDTH - 2 * margin;
        int grp3H = WINDOW_HEIGHT - TAB_HEIGHT - STATUSBAR_HEIGHT - margin - grp3Y;
        if (grp3H < 100) grp3H = 100;
        CreateWindowExW(0, L"BUTTON",
            L"总线监视",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            grp3X, grp3Y, grp3W, grp3H, hwnd, NULL, hInst, NULL);

        int monX = grp3X + 10;
        int monY = grp3Y + 24;
        int monW = grp3W - 20;
        int monH = grp3H - 68;
        if (monH < 50) monH = 50;
        pData->hEditMonitor = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            monX, monY, monW, monH,
            hwnd, (HMENU)IDC_EDIT_CAN_MONITOR, hInst, NULL);
        SendMessageW(pData->hEditMonitor, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        /* Auto-scroll checkbox + Clear button */
        cx = grp3X + grp3W - 10;
        cy = grp3Y + grp3H - 34;

        pData->hBtnClearMonitor = CreateWindowExW(0, L"BUTTON",
            L"清除",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx - 90, cy, 80, 28,
            hwnd, (HMENU)IDC_BUTTON_CLEAR_MONITOR, hInst, NULL);
        SendMessageW(pData->hBtnClearMonitor, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hCheckAutoScroll = CreateWindowExW(0, L"BUTTON",
            L"自动滚动",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            grp3X + 10, cy, 110, 24,
            hwnd, (HMENU)IDC_CHECK_AUTOSCROLL, hInst, NULL);
        SendMessageW(pData->hCheckAutoScroll, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        SendMessageW(pData->hCheckAutoScroll, BM_SETCHECK, BST_CHECKED, 0);

        /* Set up frame callback */
        CanCommand_SetFrameCallback(pData->canCmd, CanFrameCb, (void *)hwnd);

        /* Initial control states */
        UpdateControlStates(pData);

        return 0;
    }

    /* ---- Command handling ---- */
    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case IDC_BUTTON_CAN_SEND: {
            if (!pData->canCmd || pData->channel == CAN_HAL_INVALID_HANDLE)
                return 0;

            wchar_t idStr[32];
            GetWindowTextW(pData->hEditCanId, idStr, 32);
            uint32_t can_id = (uint32_t)wcstoul(idStr, NULL, 16);

            wchar_t dataStr[128];
            GetWindowTextW(pData->hEditCanData, dataStr, 128);
            uint8_t data[8] = {0};
            int dlc = ParseHexData(dataStr, data, 8);

            int is_extended = (SendMessageW(pData->hRadioExtFrame, BM_GETCHECK, 0, 0) == BST_CHECKED);
            int is_remote = (SendMessageW(pData->hRadioRemoteFrame, BM_GETCHECK, 0, 0) == BST_CHECKED);

            int result = CanCommand_SendFrame(pData->canCmd, can_id, data, dlc,
                                               is_extended, is_remote);
            if (!result) {
                MessageBoxW(hwnd,
                    L"发送失败",
                    L"错误",
                    MB_OK | MB_ICONERROR);
            }
            return 0;
        }

        case IDC_BUTTON_CLEAR_MONITOR: {
            HWND hMon = pData->hEditMonitor;
            ShowWindow(hMon, SW_HIDE);
            SetWindowTextW(hMon, L"");
            RedrawWindow(hMon, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
            ShowWindow(hMon, SW_SHOW);
            return 0;
        }

        case IDC_BUTTON_LORA_QUERY_CFG:
            SendLoraCommand(pData, LORA_CMD_QUERY);
            return 0;

        case IDC_BUTTON_LORA_SET_CFG:
            SendLoraCommand(pData, LORA_CMD_SET);
            return 0;

        case IDC_BUTTON_LORA_QUERY_NID:
            SendLoraCommand(pData, LORA_CMD_QUERY_NID);
            return 0;

        case IDC_BUTTON_LORA_SET_NID:
            SendLoraCommand(pData, LORA_CMD_SET_NID);
            return 0;

        case IDC_BUTTON_LORA_QUERY_GWID:
            SendLoraCommand(pData, LORA_CMD_QUERY_GWID);
            return 0;

        case IDC_BUTTON_LORA_SET_GWID:
            SendLoraCommand(pData, LORA_CMD_SET_GWID);
            return 0;

        default:
            break;
        }
        break;

    /* ---- CAN frame received (from monitor thread via PostMessage) ---- */
    case WM_CAN_FRAME_RECEIVED: {
        FrameInfo *fi = (FrameInfo *)lParam;
        if (fi) {
            AppendMonitorLine(pData, fi->is_tx, fi->id, fi->data, fi->dlc);

            /* Parse LoRa response (0x106) */
            if (!fi->is_tx && fi->id == CANID_LORA_TX && fi->dlc > 0) {
                wchar_t status[128];
                int ok = (fi->data[0] == 0x00);

                switch (pData->lastLoraCmd) {
                case LORA_CMD_QUERY:
                    if (ok && fi->dlc > 3) {
                        int prot = fi->data[1] >> 4;
                        int mode = fi->data[1] & 0x0F;
                        int spd = fi->data[2];
                        int ch = fi->data[3];
                        wsprintfW(status,
                            L"查询成功: prot=%d mode=%d spd=%d ch=%d",
                            prot, mode, spd, ch);
                        /* Update combo selections */
                        SendMessageW(pData->hComboProt, CB_SETCURSEL, prot, 0);
                        SendMessageW(pData->hComboMode, CB_SETCURSEL, mode, 0);
                        if (spd >= 4 && spd <= 11)
                            SendMessageW(pData->hComboSpd, CB_SETCURSEL, spd - 4, 0);
                        SendMessageW(pData->hComboCh, CB_SETCURSEL, ch, 0);
                    } else if (!ok) {
                        wcscpy(status, L"查询失败");
                    }
                    break;
                case LORA_CMD_SET:
                    if (ok)
                        wcscpy(status, L"设置成功");
                    else
                        wcscpy(status, L"设置失败");
                    break;
                case LORA_CMD_QUERY_NID:
                case LORA_CMD_SET_NID:
                    if (ok && fi->dlc >= 8) {
                        uint32_t nid = GetBE32(&fi->data[4]);
                        wsprintfW(status, L"NID: 0x%08X", nid);
                        wchar_t nbuf[16];
                        wsprintfW(nbuf, L"%08X", nid);
                        SetWindowTextW(pData->hEditNid, nbuf);
                    } else if (!ok) {
                        wcscpy(status, L"NID 操作失败");
                    }
                    break;
                case LORA_CMD_QUERY_GWID:
                case LORA_CMD_SET_GWID:
                    if (ok && fi->dlc >= 8) {
                        uint32_t gwid = GetBE32(&fi->data[4]);
                        wsprintfW(status, L"GWID: 0x%08X", gwid);
                        wchar_t gbuf[16];
                        wsprintfW(gbuf, L"%08X", gwid);
                        SetWindowTextW(pData->hEditGwid, gbuf);
                    } else if (!ok) {
                        wcscpy(status, L"GWID 操作失败");
                    }
                    break;
                default:
                    if (ok)
                        wcscpy(status, L"成功");
                    else
                        wcscpy(status, L"失败");
                    break;
                }
                SetWindowTextW(pData->hLabelLoraStatus, status);
            }

            free(fi);
        }
        return 0;
    }

    /* ---- Show/Hide handling for monitor start/stop ---- */
    case WM_SHOWWINDOW: {
        if (wParam && pData->channel != CAN_HAL_INVALID_HANDLE && !pData->isActive) {
            pData->isActive = 1;
            CanCommand_StartMonitor(pData->canCmd);
        } else if (!wParam && pData->isActive) {
            pData->isActive = 0;
            CanCommand_StopMonitor(pData->canCmd);
        }
        break;
    }

    /* ---- Cleanup ---- */
    case WM_DESTROY:
        if (pData) {
            if (pData->isActive) {
                CanCommand_StopMonitor(pData->canCmd);
                pData->isActive = 0;
            }
            CanCommand_SetFrameCallback(pData->canCmd, NULL, NULL);
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
/*  Public API: Create / Destroy / UpdateChannel                      */
/* ------------------------------------------------------------------ */

static const wchar_t *TAB_CMD_CLASS = L"TabCanCommandClass";
static int g_cmdClassRegistered = 0;

HWND TabCanCommand_Create(HWND hParent, HINSTANCE hInst, CanCommand *cmd)
{
    if (!g_cmdClassRegistered) {
        WNDCLASSEXW wc = { 0 };
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = TabCanCommand_WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = TAB_CMD_CLASS;
        RegisterClassExW(&wc);
        g_cmdClassRegistered = 1;
    }

    RECT rcParent;
    GetClientRect(hParent, &rcParent);
    TabCtrl_AdjustRect(hParent, FALSE, &rcParent);

    HWND hwnd = CreateWindowExW(
        0,
        TAB_CMD_CLASS,
        L"",
        WS_CHILD | WS_CLIPCHILDREN,
        rcParent.left, rcParent.top,
        rcParent.right - rcParent.left,
        rcParent.bottom - rcParent.top,
        hParent, NULL, hInst, cmd);

    return hwnd;
}

void TabCanCommand_Destroy(HWND hwnd)
{
    if (hwnd)
        DestroyWindow(hwnd);
}

void TabCanCommand_UpdateChannel(HWND hwnd, int channel)
{
    TAB_CMD_DATA *pData = GetTabPageData(hwnd);
    if (!pData) return;

    pData->channel = channel;
    CanCommand_SetChannel(pData->canCmd, channel);
    UpdateControlStates(pData);

    if (pData->isActive && channel != CAN_HAL_INVALID_HANDLE) {
        CanCommand_StartMonitor(pData->canCmd);
    } else if (channel == CAN_HAL_INVALID_HANDLE) {
        CanCommand_StopMonitor(pData->canCmd);
    }
}
