/**
 * Tab 4: CAN Command Sending Page
 *
 * Custom frame sending, LoRa configuration, and bus monitor.
 * LoRa command codes match device firmware mod-can.h exactly:
 *   SET_MODE=0x01, QUERY_MODE=0x02, SET_CH1=0x03, QUERY_CH1=0x04,
 *   SET_CH2=0x05, QUERY_CH2=0x06, QUERY_NID=0x07, SET_NID=0x08,
 *   QUERY_GWID=0x09, SET_GWID=0x0A, QUERY_PNUM=0x0B, SET_PNUM=0x0C,
 *   SET_TEST=0x0D, SET_POWER=0x0F
 *
 * Response format (0x106): data[0] = command code (self-identifying),
 * remaining bytes carry command-specific data.
 */
#include <windows.h>
#include <commctrl.h>
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

/* LoRa config commands - match device mod-can.h exactly */
#define LORA_CMD_SET_MODE     0x01
#define LORA_CMD_QUERY_MODE   0x02
#define LORA_CMD_SET_CH1      0x03
#define LORA_CMD_QUERY_CH1    0x04
#define LORA_CMD_SET_CH2      0x05
#define LORA_CMD_QUERY_CH2    0x06
#define LORA_CMD_QUERY_NID    0x07
#define LORA_CMD_SET_NID      0x08
#define LORA_CMD_QUERY_GWID   0x09
#define LORA_CMD_SET_GWID     0x0A
#define LORA_CMD_QUERY_PNUM   0x0B
#define LORA_CMD_SET_PNUM     0x0C
#define LORA_CMD_SET_TEST     0x0D
#define LORA_CMD_SET_POWER    0x0F

/* ------------------------------------------------------------------ */
/*  Per-window instance data (stored via GWLP_USERDATA)                */
/* ------------------------------------------------------------------ */
typedef struct {
    CanCommand  *canCmd;
    int          channel;
    int          isActive;

    /* LoRa state tracking */
    int          loraPowered;    /* 1 = powered on, 0 = off/unknown */
    int          loraTestMode;   /* 1 = test mode active */

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
    HWND hBtnLoraPower;
    HWND hBtnLoraTest;
    HWND hComboProt;
    HWND hComboMode;
    HWND hComboSpd1;
    HWND hEditCh1;
    HWND hComboSpd2;
    HWND hEditCh2;
    HWND hComboPnum;
    HWND hEditNid;
    HWND hEditGwid;
    HWND hLabelLoraStatus;

    /* LoRa set buttons (need enable/disable with power) */
    HWND hBtnQueryCfg;
    HWND hBtnSetMode;
    HWND hBtnSetCh1;
    HWND hBtnSetCh2;
    HWND hBtnSetPnum;
    HWND hBtnQueryNid;
    HWND hBtnQueryGwid;
    HWND hBtnSetGwid;

    /* Resizable group boxes */
    HWND hGrpLora;      /* Group 2: LoRa Config (stretches width) */
    HWND hGrpMonitor;   /* Group 3: Bus Monitor (stretches both) */

    /* Pending query counter for "query all" batch */
    int  pendingQueryCount;

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

static uint16_t GetBE16(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

static void PutBE16(uint16_t val, uint8_t *buf)
{
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val);
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

    wcscat(line + pos, L"\r\n");

    int len = GetWindowTextLengthW(hMon);

    /* Trim old content when approaching Edit control limit (~32KB) */
    if (len > 30000) {
        /* Remove first quarter of text to make room */
        int cutLen = len / 4;
        SendMessageW(hMon, EM_SETSEL, 0, cutLen);
        SendMessageW(hMon, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(hMon);
    }

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

/* Frame callback -- called from dispatcher read thread */
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

/* ------------------------------------------------------------------ */
/*  LoRa power state: enable/disable all LoRa config controls         */
/* ------------------------------------------------------------------ */
static void UpdateLoraControlStates(TAB_CMD_DATA *pData)
{
    BOOL connected = (pData->channel != CAN_HAL_INVALID_HANDLE);
    BOOL powered   = pData->loraPowered;

    /* Power/test buttons: only need CAN connection */
    EnableWindow(pData->hBtnLoraPower, connected);
    EnableWindow(pData->hBtnLoraTest, connected && powered);
    SetWindowTextW(pData->hBtnLoraPower,
                   powered ? L"LoRa 断电" : L"LoRa 上电");
    SetWindowTextW(pData->hBtnLoraTest,
                   pData->loraTestMode ? L"退出测试" : L"测试模式");

    /* All other LoRa controls: need CAN connection AND LoRa powered */
    BOOL loraReady = connected && powered;
    EnableWindow(pData->hComboProt,   loraReady);
    EnableWindow(pData->hComboMode,   loraReady);
    EnableWindow(pData->hComboSpd1,   loraReady);
    EnableWindow(pData->hEditCh1,     loraReady);
    EnableWindow(pData->hComboSpd2,   loraReady);
    EnableWindow(pData->hEditCh2,     loraReady);
    EnableWindow(pData->hComboPnum,   loraReady);
    EnableWindow(pData->hEditNid,     loraReady);
    EnableWindow(pData->hEditGwid,    loraReady);
    EnableWindow(pData->hBtnQueryCfg, loraReady);
    EnableWindow(pData->hBtnSetMode,  loraReady);
    EnableWindow(pData->hBtnSetCh1,   loraReady);
    EnableWindow(pData->hBtnSetCh2,   loraReady);
    EnableWindow(pData->hBtnSetPnum,  loraReady);
    EnableWindow(pData->hBtnQueryNid, loraReady);
    EnableWindow(pData->hBtnQueryGwid,loraReady);
    EnableWindow(pData->hBtnSetGwid,  loraReady);

    /* Frame send controls: only need CAN connection */
    EnableWindow(pData->hBtnSend,     connected);
    EnableWindow(pData->hEditCanId,   connected);
    EnableWindow(pData->hEditCanData, connected);
}

/* Update enable/disable state of all controls */
static void UpdateControlStates(TAB_CMD_DATA *pData)
{
    UpdateLoraControlStates(pData);
}

/* ------------------------------------------------------------------ */
/*  GWID Input Dialog (modal popup for hex value entry)                */
/* ------------------------------------------------------------------ */
typedef struct {
    wchar_t value[32];   /* Input/output: hex string */
    int     confirmed;   /* Output: 1 = OK pressed */
    HFONT   hFont;       /* Dialog font, cleanup before EndDialog */
} GwidInputData;

static INT_PTR CALLBACK GwidInputDlgProc(HWND hDlg, UINT uMsg,
                                          WPARAM wParam, LPARAM lParam)
{
    GwidInputData *data = (GwidInputData *)GetWindowLongPtrW(hDlg, GWLP_USERDATA);

    switch (uMsg) {
    case WM_INITDIALOG: {
        data = (GwidInputData *)lParam;
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)data);

        HINSTANCE hInst = GetModuleHandleW(NULL);
        data->hFont = CreateFontW(
            24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");

        /* Label */
        HWND hLbl = CreateWindowExW(0, L"STATIC",
            L"请输入 GWID (十六进制):",
            WS_CHILD | WS_VISIBLE,
            16, 14, 260, 24, hDlg, NULL, hInst, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)data->hFont, TRUE);

        /* Edit control */
        HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", data->value,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            16, 46, 260, 28, hDlg, (HMENU)1001, hInst, NULL);
        SendMessageW(hEdit, WM_SETFONT, (WPARAM)data->hFont, TRUE);

        /* OK button (default) */
        HWND hOk = CreateWindowExW(0, L"BUTTON", L"确定",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            80, 86, 80, 30, hDlg, (HMENU)IDOK, hInst, NULL);
        SendMessageW(hOk, WM_SETFONT, (WPARAM)data->hFont, TRUE);

        /* Cancel button */
        HWND hCancel = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            180, 86, 80, 30, hDlg, (HMENU)IDCANCEL, hInst, NULL);
        SendMessageW(hCancel, WM_SETFONT, (WPARAM)data->hFont, TRUE);

        SetFocus(hEdit);
        return FALSE;  /* We set focus explicitly */
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            GetWindowTextW(GetDlgItem(hDlg, 1001), data->value, 32);
            data->confirmed = 1;
            if (data->hFont) { DeleteObject(data->hFont); data->hFont = NULL; }
            EndDialog(hDlg, IDOK);
            return TRUE;
        case IDCANCEL:
            data->confirmed = 0;
            if (data->hFont) { DeleteObject(data->hFont); data->hFont = NULL; }
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        data->confirmed = 0;
        if (data->hFont) { DeleteObject(data->hFont); data->hFont = NULL; }
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }

    return FALSE;
}

/* Show modal GWID input dialog. Returns 1 if confirmed, 0 if cancelled. */
static int ShowGwidInputDialog(HWND hParent, wchar_t *buf, int bufSize)
{
    /* Build minimal DLGTEMPLATE in memory */
    struct {
        DLGTEMPLATE hdr;
        WORD  menu;          /* 0 = no menu */
        WORD  cls;           /* 0 = default dialog class */
        wchar_t title[8];
    } tmpl;

    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.hdr.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    tmpl.hdr.cdit  = 0;     /* Controls created in WM_INITDIALOG */
    tmpl.hdr.cx    = 180;   /* dialog units */
    tmpl.hdr.cy    = 90;
    wcscpy(tmpl.title, L"设置 GWID");

    GwidInputData data = {0};
    wcsncpy(data.value, buf, 31);
    data.value[31] = L'\0';

    DialogBoxIndirectParamW(
        GetModuleHandleW(NULL),
        (LPCDLGTEMPLATE)&tmpl,
        hParent,
        GwidInputDlgProc,
        (LPARAM)&data);

    if (data.confirmed) {
        wcsncpy(buf, data.value, bufSize - 1);
        buf[bufSize - 1] = L'\0';
    }

    return data.confirmed;
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

/**
 * Send a LoRa config CAN frame to device.
 * Frame format matches can.c / mod-can.h:
 *   SET_MODE(0x01): data[0]=0x01, data[1]=prot[7:4]|mode[3:0]
 *   SET_CH1(0x03):  data[0]=0x03, data[1]=spd, data[2-3]=ch(BE16)
 *   SET_CH2(0x05):  data[0]=0x05, data[1]=spd, data[2-3]=ch(BE16)
 *   SET_PNUM(0x0C): data[0]=0x0C, data[1]=pnum
 *   SET_NID(0x08):  data[0]=0x08, data[4-7]=nid(BE32)
 *   SET_GWID(0x0A): data[0]=0x0A, data[4-7]=gwid(BE32)
 *   SET_TEST(0x0D): data[0]=0x0D, data[1]=1/0
 *   SET_POWER(0x0F):data[0]=0x0F, data[1]=1/0
 */
static void SendLoraCommand(TAB_CMD_DATA *pData, uint8_t cmd)
{
    if (!pData->canCmd || pData->channel == CAN_HAL_INVALID_HANDLE) return;

    uint8_t data[8] = {0};
    int dlc = 1;
    data[0] = cmd;

    wchar_t buf[32];

    switch (cmd) {
    case LORA_CMD_SET_MODE: {
        int prot = (int)SendMessageW(pData->hComboProt, CB_GETCURSEL, 0, 0);
        int mode = (int)SendMessageW(pData->hComboMode, CB_GETCURSEL, 0, 0);
        data[1] = (uint8_t)((prot << 4) | (mode & 0x0F));
        dlc = 2;
        break;
    }
    case LORA_CMD_SET_CH1: {
        int spd_idx = (int)SendMessageW(pData->hComboSpd1, CB_GETCURSEL, 0, 0);
        data[1] = (uint8_t)(spd_idx + 4);
        GetWindowTextW(pData->hEditCh1, buf, 32);
        uint16_t ch = (uint16_t)wcstoul(buf, NULL, 10);
        PutBE16(ch, &data[2]);
        dlc = 4;
        break;
    }
    case LORA_CMD_SET_CH2: {
        int spd_idx = (int)SendMessageW(pData->hComboSpd2, CB_GETCURSEL, 0, 0);
        data[1] = (uint8_t)(spd_idx + 4);
        GetWindowTextW(pData->hEditCh2, buf, 32);
        uint16_t ch = (uint16_t)wcstoul(buf, NULL, 10);
        PutBE16(ch, &data[2]);
        dlc = 4;
        break;
    }
    case LORA_CMD_SET_PNUM: {
        int pnum = (int)SendMessageW(pData->hComboPnum, CB_GETCURSEL, 0, 0);
        data[1] = (uint8_t)pnum;
        dlc = 2;
        break;
    }
    case LORA_CMD_SET_GWID: {
        GetWindowTextW(pData->hEditGwid, buf, 32);
        uint32_t gwid = (uint32_t)wcstoul(buf, NULL, 16);
        PutBE32(gwid, &data[4]);
        dlc = 8;
        break;
    }
    case LORA_CMD_SET_TEST: {
        data[1] = pData->loraTestMode ? 0 : 1;
        dlc = 2;
        break;
    }
    case LORA_CMD_SET_POWER: {
        data[1] = pData->loraPowered ? 0 : 1;
        dlc = 2;
        break;
    }
    default:
        break;
    }

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
        pData->loraPowered = 0;
        pData->loraTestMode = 0;
        pData->pendingQueryCount = 0;

        /* Get actual client area instead of hardcoded constants */
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

        /* ========== Group 1: Frame Configuration (left, upper) ========== */
        int grp1X = margin, grp1Y = margin;
        int grp1W = 480, grp1H = 300;
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
        int grp2W = pageW - margin - grp2X - margin;
        int grp2H = grp1H;
        pData->hGrpLora = CreateWindowExW(0, L"BUTTON", L"LoRa 配置",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            grp2X, grp2Y, grp2W, grp2H, hwnd, NULL, hInst, NULL);

        cx = grp2X + 14;
        cy = grp2Y + 30;

        /* Row 1: Power + Test + Query */
        int bw = 100, bh = 28, bg = 8;

        pData->hBtnLoraPower = CreateWindowExW(0, L"BUTTON",
            L"LoRa 上电",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx, cy, bw + 20, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_POWER, hInst, NULL);
        SendMessageW(pData->hBtnLoraPower, WM_SETFONT, (WPARAM)pData->hFontBold, TRUE);

        pData->hBtnLoraTest = CreateWindowExW(0, L"BUTTON",
            L"测试模式",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + bw + 28, cy, bw + 10, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_TEST, hInst, NULL);
        SendMessageW(pData->hBtnLoraTest, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hBtnQueryCfg = CreateWindowExW(0, L"BUTTON", L"查询配置",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + 2 * (bw + 18), cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_QUERY_CFG, hInst, NULL);
        SendMessageW(pData->hBtnQueryCfg, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        cy += bh + 8;

        /* Row 2: Protocol / Mode (same line) */
        int llW = 56, cbW = 150, cbGap = 20;

        CreateLabel(hwnd, hInst, -1, cx, cy + 3, llW, 22,
            L"协议:", pData->hFont);
        pData->hComboProt = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + llW + 4, cy, cbW, 200,
            hwnd, (HMENU)IDC_EDIT_LORA_PROT, hInst, NULL);
        SendMessageW(pData->hComboProt, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        SendMessageW(pData->hComboProt, CB_ADDSTRING, 0, (LPARAM)L"NODE (0)");
        SendMessageW(pData->hComboProt, CB_ADDSTRING, 0, (LPARAM)L"LG210 (1)");
        SendMessageW(pData->hComboProt, CB_ADDSTRING, 0, (LPARAM)L"LG220 (2)");
        SendMessageW(pData->hComboProt, CB_SETCURSEL, 1, 0);

        int ox2 = cx + llW + 4 + cbW + cbGap;

        CreateLabel(hwnd, hInst, -1, ox2, cy + 3, llW, 22,
            L"模式:", pData->hFont);
        pData->hComboMode = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ox2 + llW + 4, cy, cbW, 200,
            hwnd, (HMENU)IDC_EDIT_LORA_MODE, hInst, NULL);
        SendMessageW(pData->hComboMode, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        SendMessageW(pData->hComboMode, CB_ADDSTRING, 0, (LPARAM)L"FP (0)");
        SendMessageW(pData->hComboMode, CB_ADDSTRING, 0, (LPARAM)L"TRANS (1)");
        SendMessageW(pData->hComboMode, CB_ADDSTRING, 0, (LPARAM)L"NET (2)");
        SendMessageW(pData->hComboMode, CB_SETCURSEL, 1, 0);
        cy += lineH;

        /* Row 3: CH1 (SPD + CH) */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, llW, 22,
            L"CH1:", pData->hFont);
        CreateLabel(hwnd, hInst, -1, cx + llW + 4, cy + 3, 56, 22,
            L"SPD:", pData->hFont);
        pData->hComboSpd1 = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + llW + 62, cy, 80, 200,
            hwnd, (HMENU)IDC_COMBO_LORA_SPD1, hInst, NULL);
        SendMessageW(pData->hComboSpd1, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        for (int s = 4; s <= 11; s++) {
            wchar_t sbuf[8]; wsprintfW(sbuf, L"%d", s);
            SendMessageW(pData->hComboSpd1, CB_ADDSTRING, 0, (LPARAM)sbuf);
        }
        SendMessageW(pData->hComboSpd1, CB_SETCURSEL, 3, 0);

        CreateLabel(hwnd, hInst, -1, cx + llW + 150, cy + 3, 30, 22,
            L"CH:", pData->hFont);
        pData->hEditCh1 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"4800",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            cx + llW + 182, cy, 80, 24,
            hwnd, (HMENU)IDC_EDIT_LORA_CH1, hInst, NULL);
        SendMessageW(pData->hEditCh1, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);
        CreateLabel(hwnd, hInst, -1, cx + llW + 266, cy + 3, 150, 22,
            L"(4100~5100KHz)", pData->hFont);
        cy += lineH;

        /* Row 4: CH2 (SPD + CH) */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, llW, 22,
            L"CH2:", pData->hFont);
        CreateLabel(hwnd, hInst, -1, cx + llW + 4, cy + 3, 56, 22,
            L"SPD:", pData->hFont);
        pData->hComboSpd2 = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + llW + 62, cy, 80, 200,
            hwnd, (HMENU)IDC_COMBO_LORA_SPD2, hInst, NULL);
        SendMessageW(pData->hComboSpd2, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        for (int s = 4; s <= 11; s++) {
            wchar_t sbuf[8]; wsprintfW(sbuf, L"%d", s);
            SendMessageW(pData->hComboSpd2, CB_ADDSTRING, 0, (LPARAM)sbuf);
        }
        SendMessageW(pData->hComboSpd2, CB_SETCURSEL, 3, 0);

        CreateLabel(hwnd, hInst, -1, cx + llW + 150, cy + 3, 30, 22,
            L"CH:", pData->hFont);
        pData->hEditCh2 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"4800",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            cx + llW + 182, cy, 80, 24,
            hwnd, (HMENU)IDC_EDIT_LORA_CH2, hInst, NULL);
        SendMessageW(pData->hEditCh2, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);
        CreateLabel(hwnd, hInst, -1, cx + llW + 266, cy + 3, 150, 22,
            L"(4100~5100KHz)", pData->hFont);
        cy += lineH;

        /* Row 5: PNUM + GWID */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, llW, 22,
            L"PNUM:", pData->hFont);
        pData->hComboPnum = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + llW + 4, cy, 80, 200,
            hwnd, (HMENU)IDC_COMBO_LORA_PNUM, hInst, NULL);
        SendMessageW(pData->hComboPnum, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        SendMessageW(pData->hComboPnum, CB_ADDSTRING, 0, (LPARAM)L"0");
        SendMessageW(pData->hComboPnum, CB_ADDSTRING, 0, (LPARAM)L"1");
        SendMessageW(pData->hComboPnum, CB_ADDSTRING, 0, (LPARAM)L"2");
        SendMessageW(pData->hComboPnum, CB_SETCURSEL, 0, 0);

        CreateLabel(hwnd, hInst, -1, ox2, cy + 3, llW, 22,
            L"GWID:", pData->hFont);
        pData->hEditGwid = CreateWindowExW(0, L"STATIC", L"00000000",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            ox2 + llW + 4, cy + 3, 120, 22,
            hwnd, (HMENU)IDC_EDIT_LORA_GWID, hInst, NULL);
        SendMessageW(pData->hEditGwid, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);
        cy += lineH;

        /* Row 6: NID (read-only label) */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, llW, 22,
            L"NID:", pData->hFont);
        pData->hEditNid = CreateWindowExW(0, L"STATIC", L"00000000",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + llW + 4, cy + 3, 120, 22,
            hwnd, (HMENU)IDC_EDIT_LORA_NID, hInst, NULL);
        SendMessageW(pData->hEditNid, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);
        cy += lineH + 4;

        /* Row 7: Set buttons */

        pData->hBtnSetMode = CreateWindowExW(0, L"BUTTON", L"设置模式",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx, cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_SET_MODE, hInst, NULL);
        SendMessageW(pData->hBtnSetMode, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hBtnSetCh1 = CreateWindowExW(0, L"BUTTON", L"设置CH1",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + bw + bg, cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_SET_CH1, hInst, NULL);
        SendMessageW(pData->hBtnSetCh1, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hBtnSetCh2 = CreateWindowExW(0, L"BUTTON", L"设置CH2",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + 2 * (bw + bg), cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_SET_CH2, hInst, NULL);
        SendMessageW(pData->hBtnSetCh2, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hBtnSetPnum = CreateWindowExW(0, L"BUTTON", L"设置PNUM",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + 3 * (bw + bg), cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_SET_PNUM, hInst, NULL);
        SendMessageW(pData->hBtnSetPnum, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        cy += bh + 6;

        /* Row 8: NID query + GWID query/set */
        pData->hBtnQueryNid = CreateWindowExW(0, L"BUTTON", L"查询 NID",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx, cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_QUERY_NID, hInst, NULL);
        SendMessageW(pData->hBtnQueryNid, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hBtnQueryGwid = CreateWindowExW(0, L"BUTTON", L"查询 GWID",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + bw + bg, cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_QUERY_GWID, hInst, NULL);
        SendMessageW(pData->hBtnQueryGwid, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hBtnSetGwid = CreateWindowExW(0, L"BUTTON", L"设置 GWID",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + 2 * (bw + bg), cy, bw, bh,
            hwnd, (HMENU)IDC_BUTTON_LORA_SET_GWID, hInst, NULL);
        SendMessageW(pData->hBtnSetGwid, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        cy += bh + 6;

        /* Status label */
        pData->hLabelLoraStatus = CreateWindowExW(0, L"STATIC",
            L"请先上电 LoRa",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx, cy + 2, grp2W - 28, 22,
            hwnd, (HMENU)IDC_LABEL_LORA_STATUS, hInst, NULL);
        SendMessageW(pData->hLabelLoraStatus, WM_SETFONT,
            (WPARAM)pData->hFont, TRUE);

        /* ========== Group 3: Bus Monitor (full width, bottom) ========== */
        int grp3X = margin;
        int grp3Y = grp1Y + grp1H + 10;
        int grp3W = pageW - 2 * margin;
        int grp3H = pageH - TAB_HEIGHT - STATUSBAR_HEIGHT - margin - grp3Y;
        if (grp3H < 100) grp3H = 100;
        pData->hGrpMonitor = CreateWindowExW(0, L"BUTTON",
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

        /* LoRa power toggle */
        case IDC_BUTTON_LORA_POWER:
            SendLoraCommand(pData, LORA_CMD_SET_POWER);
            return 0;

        /* LoRa test mode toggle */
        case IDC_BUTTON_LORA_TEST:
            SendLoraCommand(pData, LORA_CMD_SET_TEST);
            return 0;

        /* Query all LoRa config */
        case IDC_BUTTON_LORA_QUERY_CFG:
            pData->pendingQueryCount = 6;
            SendLoraCommand(pData, LORA_CMD_QUERY_MODE);
            SendLoraCommand(pData, LORA_CMD_QUERY_CH1);
            SendLoraCommand(pData, LORA_CMD_QUERY_CH2);
            SendLoraCommand(pData, LORA_CMD_QUERY_PNUM);
            SendLoraCommand(pData, LORA_CMD_QUERY_NID);
            SendLoraCommand(pData, LORA_CMD_QUERY_GWID);
            return 0;

        case IDC_BUTTON_LORA_SET_MODE:
            SendLoraCommand(pData, LORA_CMD_SET_MODE);
            return 0;

        case IDC_BUTTON_LORA_SET_CH1:
            SendLoraCommand(pData, LORA_CMD_SET_CH1);
            return 0;

        case IDC_BUTTON_LORA_SET_CH2:
            SendLoraCommand(pData, LORA_CMD_SET_CH2);
            return 0;

        case IDC_BUTTON_LORA_SET_PNUM:
            SendLoraCommand(pData, LORA_CMD_SET_PNUM);
            return 0;

        case IDC_BUTTON_LORA_QUERY_NID:
            SendLoraCommand(pData, LORA_CMD_QUERY_NID);
            return 0;

        case IDC_BUTTON_LORA_QUERY_GWID:
            SendLoraCommand(pData, LORA_CMD_QUERY_GWID);
            return 0;

        case IDC_BUTTON_LORA_SET_GWID: {
            wchar_t gwidStr[32] = L"";
            GetWindowTextW(pData->hEditGwid, gwidStr, 32);
            if (ShowGwidInputDialog(hwnd, gwidStr, 32)) {
                uint32_t gwid = (uint32_t)wcstoul(gwidStr, NULL, 16);
                uint8_t tx[8] = {0};
                tx[0] = LORA_CMD_SET_GWID;
                PutBE32(gwid, &tx[4]);
                SetWindowTextW(pData->hLabelLoraStatus, L"等待响应...");
                CanCommand_SendFrame(pData->canCmd, CANID_LORA_RX, tx, 8, 0, 0);
            }
            return 0;
        }

        default:
            break;
        }
        break;

    /* ---- CAN frame received (from dispatcher via PostMessage) ---- */
    case WM_CAN_FRAME_RECEIVED: {
        FrameInfo *fi = (FrameInfo *)lParam;
        if (fi) {
            AppendMonitorLine(pData, fi->is_tx, fi->id, fi->data, fi->dlc);

            /* Parse LoRa response (0x106) — data[0] = command code */
            if (!fi->is_tx && fi->id == CANID_LORA_TX && fi->dlc >= 1) {
                uint8_t respCmd = fi->data[0];
                wchar_t status[128] = L"";

                switch (respCmd) {
                case LORA_CMD_SET_POWER:
                    if (fi->dlc >= 2) {
                        pData->loraPowered = (fi->data[1] != 0) ? 1 : 0;
                        wsprintfW(status, L"LoRa %s",
                                  pData->loraPowered ? L"已上电" : L"已断电");
                    }
                    UpdateLoraControlStates(pData);
                    break;

                case LORA_CMD_SET_TEST:
                    if (fi->dlc >= 2) {
                        pData->loraTestMode = (fi->data[1] != 0) ? 1 : 0;
                        wsprintfW(status, L"测试模式 %s",
                                  pData->loraTestMode ? L"已开启" : L"已关闭");
                    }
                    UpdateLoraControlStates(pData);
                    break;

                case LORA_CMD_QUERY_MODE:
                case LORA_CMD_SET_MODE:
                    if (fi->dlc > 1) {
                        int prot = fi->data[1] >> 4;
                        int mode = fi->data[1] & 0x0F;
                        wsprintfW(status, L"模式: prot=%d mode=%d", prot, mode);
                        SendMessageW(pData->hComboProt, CB_SETCURSEL, prot, 0);
                        SendMessageW(pData->hComboMode, CB_SETCURSEL, mode, 0);
                    }
                    break;

                case LORA_CMD_QUERY_CH1:
                case LORA_CMD_SET_CH1:
                    if (fi->dlc >= 4) {
                        int spd = fi->data[1];
                        uint16_t ch = GetBE16(&fi->data[2]);
                        wsprintfW(status, L"CH1: spd=%d ch=%d", spd, ch);
                        if (spd >= 4 && spd <= 11)
                            SendMessageW(pData->hComboSpd1, CB_SETCURSEL, spd - 4, 0);
                        wchar_t chbuf[16];
                        wsprintfW(chbuf, L"%d", ch);
                        SetWindowTextW(pData->hEditCh1, chbuf);
                    }
                    break;

                case LORA_CMD_QUERY_CH2:
                case LORA_CMD_SET_CH2:
                    if (fi->dlc >= 4) {
                        int spd = fi->data[1];
                        uint16_t ch = GetBE16(&fi->data[2]);
                        wsprintfW(status, L"CH2: spd=%d ch=%d", spd, ch);
                        if (spd >= 4 && spd <= 11)
                            SendMessageW(pData->hComboSpd2, CB_SETCURSEL, spd - 4, 0);
                        wchar_t chbuf[16];
                        wsprintfW(chbuf, L"%d", ch);
                        SetWindowTextW(pData->hEditCh2, chbuf);
                    }
                    break;

                case LORA_CMD_QUERY_PNUM:
                case LORA_CMD_SET_PNUM:
                    if (fi->dlc >= 2) {
                        int pnum = fi->data[1];
                        wsprintfW(status, L"PNUM: %d", pnum);
                        if (pnum >= 0 && pnum <= 2)
                            SendMessageW(pData->hComboPnum, CB_SETCURSEL, pnum, 0);
                    }
                    break;

                case LORA_CMD_QUERY_NID:
                case LORA_CMD_SET_NID:
                    if (fi->dlc >= 8) {
                        uint32_t nid = GetBE32(&fi->data[4]);
                        wsprintfW(status, L"NID: 0x%08X", nid);
                        wchar_t nbuf[16];
                        wsprintfW(nbuf, L"%08X", nid);
                        SetWindowTextW(pData->hEditNid, nbuf);
                    }
                    break;

                case LORA_CMD_QUERY_GWID:
                case LORA_CMD_SET_GWID:
                    if (fi->dlc >= 8) {
                        uint32_t gwid = GetBE32(&fi->data[4]);
                        wsprintfW(status, L"GWID: 0x%08X", gwid);
                        wchar_t gbuf[16];
                        wsprintfW(gbuf, L"%08X", gwid);
                        SetWindowTextW(pData->hEditGwid, gbuf);
                    }
                    break;

                default:
                    wsprintfW(status, L"未知响应: 0x%02X", respCmd);
                    break;
                }

                /* Track batch query completion */
                if (pData->pendingQueryCount > 0) {
                    pData->pendingQueryCount--;
                    if (pData->pendingQueryCount == 0) {
                        wcscat(status, L" [查询完成]");
                    }
                }

                if (status[0])
                    SetWindowTextW(pData->hLabelLoraStatus, status);
            }

            free(fi);
        }
        return 0;
    }

    /* ---- Resize: adapt group boxes and monitor area ---- */
    case WM_SIZE: {
        int cx = LOWORD(lParam);
        int cy = HIWORD(lParam);
        if (cx < 100 || cy < 100) return 0;

        int margin = 14;
        int grp1W = 480, grp1H = 300;

        /* Group 2: LoRa Config — stretch width */
        int grp2X = margin + grp1W + 10;
        int grp2W = cx - margin - grp2X - margin;
        if (grp2W < 200) grp2W = 200;
        MoveWindow(pData->hGrpLora, grp2X, margin, grp2W, grp1H, TRUE);

        /* Group 3: Bus Monitor — stretch both */
        int grp3X = margin;
        int grp3Y = margin + grp1H + 10;
        int grp3W = cx - 2 * margin;
        int grp3H = cy - grp3Y;
        if (grp3H < 100) grp3H = 100;
        if (grp3W < 200) grp3W = 200;
        MoveWindow(pData->hGrpMonitor, grp3X, grp3Y, grp3W, grp3H, TRUE);

        /* Monitor edit — fill group interior */
        int monX = grp3X + 10;
        int monY = grp3Y + 24;
        int monW = grp3W - 20;
        int monH = grp3H - 68;
        if (monW < 50) monW = 50;
        if (monH < 50) monH = 50;
        MoveWindow(pData->hEditMonitor, monX, monY, monW, monH, TRUE);

        /* Bottom toolbar — reposition with group */
        int tbY = grp3Y + grp3H - 34;
        MoveWindow(pData->hBtnClearMonitor,
                   grp3X + grp3W - 100, tbY, 80, 28, TRUE);
        MoveWindow(pData->hCheckAutoScroll,
                   grp3X + 10, tbY, 110, 24, TRUE);

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
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
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
