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
#include "tab_base.h"

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
    TAB_BASE     base;
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
    HWND hComboFreq1;
    HWND hComboSpd2;
    HWND hComboFreq2;
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

    /* Additional query buttons for the per-parameter layout (can query without power) */
    HWND hBtnQueryMode;
    HWND hBtnQueryCh1;
    HWND hBtnQueryCh2;
    HWND hBtnQueryPnum;

    /* Resizable group boxes */
    HWND hGrpLora;      /* Group 2: LoRa Config (stretches width) */
    HWND hGrpMonitor;   /* Group 3: Bus Monitor (stretches both) */

    /* Pending query counter for "query all" batch */
    int  pendingQueryCount;
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
        if (can_id == CANID_HANDLER_STATE && dlc >= 5) {
            int16_t x = (int16_t)((data[0] << 8) | data[1]);
            int16_t y = (int16_t)((data[2] << 8) | data[3]);
            pos += wsprintfW(line + pos, L"  X=%s%d° Y=%s%d° 按键=%s",
                             x >= 0 ? L"+" : L"", x,
                             y >= 0 ? L"+" : L"", y,
                             data[4] == 0 ? L"按下" : L"松开");
        } else {
            for (int i = 0; i < dlc && i < 8; i++) {
                pos += wsprintfW(line + pos, L" %02X", data[i]);
            }
        }
    }

    wcscat(line + pos, L"\r\n");

    int len = GetWindowTextLengthW(hMon);

    /* Trim old content when approaching Edit control limit (~32KB) */
    if (len > 100000) {
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

    BOOL canQuery  = connected;           // 查询操作不需要上电
    BOOL loraReady = connected && powered; // 设置和配置控件需要上电

    /* Config controls (combos/edits): require power */
    EnableWindow(pData->hComboProt,   loraReady);
    EnableWindow(pData->hComboMode,   loraReady);
    EnableWindow(pData->hComboSpd1,   loraReady);
    EnableWindow(pData->hComboFreq1,  loraReady);
    EnableWindow(pData->hComboSpd2,   loraReady);
    EnableWindow(pData->hComboFreq2,  loraReady);
    EnableWindow(pData->hComboPnum,   loraReady);
    EnableWindow(pData->hEditNid,     loraReady);
    EnableWindow(pData->hEditGwid,    loraReady);

    /* Query buttons: only need CAN connection (can query even if not powered) */
    EnableWindow(pData->hBtnQueryCfg,  canQuery);
    EnableWindow(pData->hBtnQueryNid,  canQuery);
    EnableWindow(pData->hBtnQueryGwid, canQuery);
    EnableWindow(pData->hBtnQueryMode, canQuery);
    EnableWindow(pData->hBtnQueryCh1,  canQuery);
    EnableWindow(pData->hBtnQueryCh2,  canQuery);
    EnableWindow(pData->hBtnQueryPnum, canQuery);

    /* Set buttons: require power */
    EnableWindow(pData->hBtnSetMode,  loraReady);
    EnableWindow(pData->hBtnSetCh1,   loraReady);
    EnableWindow(pData->hBtnSetCh2,   loraReady);
    EnableWindow(pData->hBtnSetPnum,  loraReady);
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
            FONT_SIZE_UI, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            FONT_FACE_UI);

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
        GetWindowTextW(pData->hComboFreq1, buf, 32);
        uint16_t ch = (uint16_t)wcstoul(buf, NULL, 10);
        PutBE16(ch, &data[2]);
        dlc = 4;
        break;
    }
    case LORA_CMD_SET_CH2: {
        int spd_idx = (int)SendMessageW(pData->hComboSpd2, CB_GETCURSEL, 0, 0);
        data[1] = (uint8_t)(spd_idx + 4);
        GetWindowTextW(pData->hComboFreq2, buf, 32);
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
/*  Tab base framework hooks                                          */
/* ------------------------------------------------------------------ */

static void can_command_on_create(HWND hwnd, void *data, CREATESTRUCTW *cs)
{
    TAB_CMD_DATA *pData = (TAB_CMD_DATA *)data;
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

    int margin = 14;
    int lineH = 36;
    int cx, cy;

    /* ========== Group 1: Frame Configuration (left, upper) ========== */
    int grp1X = margin, grp1Y = margin;
    int grp1W = 480, grp1H = 420;
    CreateWindowExW(0, L"BUTTON", L"帧配置",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        grp1X, grp1Y, grp1W, grp1H, hwnd, NULL, hInst, NULL);

    cx = grp1X + 14;
    cy = grp1Y + 30;

    /* Row 1: CAN ID */
    CreateLabel(hwnd, hInst, -1, cx, cy + 3, 70, 24, L"CAN ID:", pData->base.hFont);
    pData->hEditCanId = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"101",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        cx + 78, cy, 200, 24, hwnd, (HMENU)IDC_EDIT_CAN_ID, hInst, NULL);
    SendMessageW(pData->hEditCanId, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    CreateLabel(hwnd, hInst, -1, cx + 286, cy + 3, 50, 24, L"(Hex)", pData->base.hFont);
    cy += lineH;

    /* Row 2: Frame Format */
    CreateLabel(hwnd, hInst, -1, cx, cy + 3, 70, 24,
        L"帧格式:", pData->base.hFont);
    pData->hRadioStdFrame = CreateWindowExW(0, L"BUTTON",
        L"标准帧",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        cx + 78, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_STD_FRAME, hInst, NULL);
    SendMessageW(pData->hRadioStdFrame, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    pData->hRadioExtFrame = CreateWindowExW(0, L"BUTTON",
        L"扩展帧",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        cx + 174, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_EXT_FRAME, hInst, NULL);
    SendMessageW(pData->hRadioExtFrame, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    /* Default: 标准帧 */
    SendMessageW(pData->hRadioStdFrame, BM_SETCHECK, BST_CHECKED, 0);

    cy += lineH;

    /* Row 3: Frame Type */
    CreateLabel(hwnd, hInst, -1, cx, cy + 3, 70, 24,
        L"帧类型:", pData->base.hFont);
    pData->hRadioDataFrame = CreateWindowExW(0, L"BUTTON",
        L"数据帧",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        cx + 78, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_DATA_FRAME, hInst, NULL);
    SendMessageW(pData->hRadioDataFrame, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    pData->hRadioRemoteFrame = CreateWindowExW(0, L"BUTTON",
        L"远程帧",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        cx + 174, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_REMOTE_FRAME, hInst, NULL);
    SendMessageW(pData->hRadioRemoteFrame, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    /* Default: 数据帧 */
    SendMessageW(pData->hRadioDataFrame, BM_SETCHECK, BST_CHECKED, 0);

    cy += lineH;

    /* Row 4: Data bytes */
    CreateLabel(hwnd, hInst, -1, cx, cy + 3, 70, 24,
        L"数据:", pData->base.hFont);
    pData->hEditCanData = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"01 02 03 04 05 06 07 08",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        cx + 78, cy, 300, 24, hwnd, (HMENU)IDC_EDIT_CAN_DATA, hInst, NULL);
    SendMessageW(pData->hEditCanData, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);
    cy += lineH + 4;

    /* Row 5: Send button */
    pData->hBtnSend = CreateWindowExW(0, L"BUTTON",
        L"发送帧",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        grp1X + grp1W - 14 - 140, cy, 140, 32,
        hwnd, (HMENU)IDC_BUTTON_CAN_SEND, hInst, NULL);
    SendMessageW(pData->hBtnSend, WM_SETFONT, (WPARAM)pData->base.hFontBold, TRUE);

    /* ========== Group 2: LoRa Configuration (right, upper) ========== */
    int grp2X = grp1X + grp1W + 10;
    int grp2Y = grp1Y;
    int grp2W = pageW - margin - grp2X - margin;
    int grp2H = grp1H;
    pData->hGrpLora = CreateWindowExW(0, L"BUTTON", L"LoRa 配置",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        grp2X, grp2Y, grp2W, grp2H, hwnd, NULL, hInst, NULL);

    cx = grp2X + 14;
    cy = grp2Y + 28;

    /* Top row: Special controls */
    int bw = 88, bh = 26;

    pData->hBtnLoraPower = CreateWindowExW(0, L"BUTTON", L"LoRa 上电",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        cx, cy, bw + 8, bh,
        hwnd, (HMENU)IDC_BUTTON_LORA_POWER, hInst, NULL);
    SendMessageW(pData->hBtnLoraPower, WM_SETFONT, (WPARAM)pData->base.hFontBold, TRUE);

    pData->hBtnLoraTest = CreateWindowExW(0, L"BUTTON", L"测试模式",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        cx + bw + 12, cy, bw, bh,
        hwnd, (HMENU)IDC_BUTTON_LORA_TEST, hInst, NULL);
    SendMessageW(pData->hBtnLoraTest, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    pData->hBtnQueryCfg = CreateWindowExW(0, L"BUTTON", L"查询所有",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        cx + 2 * (bw + 8), cy, bw, bh,
        hwnd, (HMENU)IDC_BUTTON_LORA_QUERY_CFG, hInst, NULL);
    SendMessageW(pData->hBtnQueryCfg, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    cy += bh + 14;

    /* === 协议 + 模式 (同一行，只用一组按钮) === */
    int labelW = 56;   // 统一 label 宽度，解决遮挡
    int btnW   = 48;
    int rowGap = 38;   // 加大行间距

    // 协议
    CreateLabel(hwnd, hInst, -1, cx, cy + 1, labelW, 24, L"协议:", pData->base.hFont);
    pData->hComboProt = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        cx + labelW + 4, cy, 100, 180, hwnd, (HMENU)IDC_EDIT_LORA_PROT, hInst, NULL);
    SendMessageW(pData->hComboProt, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    SendMessageW(pData->hComboProt, CB_ADDSTRING, 0, (LPARAM)L"NODE");
    SendMessageW(pData->hComboProt, CB_ADDSTRING, 0, (LPARAM)L"LG210");
    SendMessageW(pData->hComboProt, CB_ADDSTRING, 0, (LPARAM)L"LG220");
    SendMessageW(pData->hComboProt, CB_SETCURSEL, 1, 0);

    // 模式（同一行）
    int modeX = cx + labelW + 110;
    CreateLabel(hwnd, hInst, -1, modeX, cy + 1, 40, 24, L"模式:", pData->base.hFont);
    pData->hComboMode = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        modeX + 44, cy, 100, 160, hwnd, (HMENU)IDC_EDIT_LORA_MODE, hInst, NULL);
    SendMessageW(pData->hComboMode, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    SendMessageW(pData->hComboMode, CB_ADDSTRING, 0, (LPARAM)L"FP");
    SendMessageW(pData->hComboMode, CB_ADDSTRING, 0, (LPARAM)L"TRANS");
    SendMessageW(pData->hComboMode, CB_ADDSTRING, 0, (LPARAM)L"NET");
    SendMessageW(pData->hComboMode, CB_SETCURSEL, 1, 0);

    // 协议+模式共享一组按钮（对齐）
    int btnX = cx + 320;
    pData->hBtnQueryMode = CreateWindowExW(0, L"BUTTON", L"查询",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX, cy, btnW, 22,
        hwnd, (HMENU)IDC_BUTTON_LORA_SET_MODE, hInst, NULL);
    SendMessageW(pData->hBtnQueryMode, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    pData->hBtnSetMode = CreateWindowExW(0, L"BUTTON", L"设置",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX + btnW + 4, cy, btnW, 24,
        hwnd, (HMENU)IDC_BUTTON_LORA_SET_MODE, hInst, NULL);
    SendMessageW(pData->hBtnSetMode, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    cy += rowGap;

    /* CH1 */
    CreateLabel(hwnd, hInst, -1, cx, cy + 1, labelW + 16, 24, L"通道1:", pData->base.hFont);
    CreateLabel(hwnd, hInst, -1, cx + labelW + 20, cy + 1, 40, 24, L"速度:", pData->base.hFont);
    pData->hComboSpd1 = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        cx + labelW + 62, cy, 56, 140, hwnd, (HMENU)IDC_COMBO_LORA_SPD1, hInst, NULL);
    SendMessageW(pData->hComboSpd1, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    for (int s = 4; s <= 11; s++) {
        wchar_t sbuf[8]; wsprintfW(sbuf, L"%d", s);
        SendMessageW(pData->hComboSpd1, CB_ADDSTRING, 0, (LPARAM)sbuf);
    }
    SendMessageW(pData->hComboSpd1, CB_SETCURSEL, 3, 0);

    CreateLabel(hwnd, hInst, -1, cx + labelW + 122, cy + 1, 40, 24, L"频率:", pData->base.hFont);
    pData->hComboFreq1 = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        cx + labelW + 164, cy, 80, 200, hwnd, (HMENU)IDC_COMBO_LORA_FREQ1, hInst, NULL);
    SendMessageW(pData->hComboFreq1, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    for (int f = 4100; f <= 5100; f += 100) {
        wchar_t fbuf[8]; wsprintfW(fbuf, L"%d", f);
        SendMessageW(pData->hComboFreq1, CB_ADDSTRING, 0, (LPARAM)fbuf);
    }
    SendMessageW(pData->hComboFreq1, CB_SETCURSEL, 7, 0); /* 4800 */

    pData->hBtnQueryCh1 = CreateWindowExW(0, L"BUTTON", L"查询",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX, cy, btnW, 22,
        hwnd, (HMENU)IDC_BUTTON_LORA_SET_CH1, hInst, NULL);
    SendMessageW(pData->hBtnQueryCh1, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    pData->hBtnSetCh1 = CreateWindowExW(0, L"BUTTON", L"设置",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX + btnW + 4, cy, btnW, 24,
        hwnd, (HMENU)IDC_BUTTON_LORA_SET_CH1, hInst, NULL);
    SendMessageW(pData->hBtnSetCh1, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    cy += rowGap;

    /* CH2 */
    CreateLabel(hwnd, hInst, -1, cx, cy + 1, labelW + 16, 24, L"通道2:", pData->base.hFont);
    CreateLabel(hwnd, hInst, -1, cx + labelW + 20, cy + 1, 40, 24, L"速度:", pData->base.hFont);
    pData->hComboSpd2 = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        cx + labelW + 62, cy, 56, 140, hwnd, (HMENU)IDC_COMBO_LORA_SPD2, hInst, NULL);
    SendMessageW(pData->hComboSpd2, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    for (int s = 4; s <= 11; s++) {
        wchar_t sbuf[8]; wsprintfW(sbuf, L"%d", s);
        SendMessageW(pData->hComboSpd2, CB_ADDSTRING, 0, (LPARAM)sbuf);
    }
    SendMessageW(pData->hComboSpd2, CB_SETCURSEL, 3, 0);

    CreateLabel(hwnd, hInst, -1, cx + labelW + 122, cy + 1, 40, 24, L"频率:", pData->base.hFont);
    pData->hComboFreq2 = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        cx + labelW + 164, cy, 80, 200, hwnd, (HMENU)IDC_COMBO_LORA_FREQ2, hInst, NULL);
    SendMessageW(pData->hComboFreq2, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    for (int f = 4100; f <= 5100; f += 100) {
        wchar_t fbuf[8]; wsprintfW(fbuf, L"%d", f);
        SendMessageW(pData->hComboFreq2, CB_ADDSTRING, 0, (LPARAM)fbuf);
    }
    SendMessageW(pData->hComboFreq2, CB_SETCURSEL, 7, 0); /* 4800 */

    pData->hBtnQueryCh2 = CreateWindowExW(0, L"BUTTON", L"查询",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX, cy, btnW, 22,
        hwnd, (HMENU)IDC_BUTTON_LORA_SET_CH2, hInst, NULL);
    SendMessageW(pData->hBtnQueryCh2, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    pData->hBtnSetCh2 = CreateWindowExW(0, L"BUTTON", L"设置",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX + btnW + 4, cy, btnW, 24,
        hwnd, (HMENU)IDC_BUTTON_LORA_SET_CH2, hInst, NULL);
    SendMessageW(pData->hBtnSetCh2, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    cy += rowGap;

    /* PNUM */
    CreateLabel(hwnd, hInst, -1, cx, cy + 1, labelW, 24, L"PNUM:", pData->base.hFont);
    pData->hComboPnum = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        cx + labelW + 4, cy, 64, 140, hwnd, (HMENU)IDC_COMBO_LORA_PNUM, hInst, NULL);
    SendMessageW(pData->hComboPnum, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    SendMessageW(pData->hComboPnum, CB_ADDSTRING, 0, (LPARAM)L"0");
    SendMessageW(pData->hComboPnum, CB_ADDSTRING, 0, (LPARAM)L"1");
    SendMessageW(pData->hComboPnum, CB_ADDSTRING, 0, (LPARAM)L"2");
    SendMessageW(pData->hComboPnum, CB_SETCURSEL, 0, 0);

    pData->hBtnQueryPnum = CreateWindowExW(0, L"BUTTON", L"查询",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX, cy, btnW, 22,
        hwnd, (HMENU)IDC_BUTTON_LORA_SET_PNUM, hInst, NULL);
    SendMessageW(pData->hBtnQueryPnum, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    pData->hBtnSetPnum = CreateWindowExW(0, L"BUTTON", L"设置",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX + btnW + 4, cy, btnW, 24,
        hwnd, (HMENU)IDC_BUTTON_LORA_SET_PNUM, hInst, NULL);
    SendMessageW(pData->hBtnSetPnum, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    cy += rowGap;

    /* GWID */
    CreateLabel(hwnd, hInst, -1, cx, cy + 1, labelW, 24, L"GWID:", pData->base.hFont);
    pData->hEditGwid = CreateWindowExW(0, L"STATIC", L"00000000",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        cx + labelW + 4, cy + 1, 90, 20, hwnd, (HMENU)IDC_EDIT_LORA_GWID, hInst, NULL);
    SendMessageW(pData->hEditGwid, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);

    pData->hBtnQueryGwid = CreateWindowExW(0, L"BUTTON", L"查询",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX, cy, btnW, 24,
        hwnd, (HMENU)IDC_BUTTON_LORA_QUERY_GWID, hInst, NULL);
    SendMessageW(pData->hBtnQueryGwid, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    pData->hBtnSetGwid = CreateWindowExW(0, L"BUTTON", L"设置",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX + btnW + 4, cy, btnW, 24,
        hwnd, (HMENU)IDC_BUTTON_LORA_SET_GWID, hInst, NULL);
    SendMessageW(pData->hBtnSetGwid, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    cy += rowGap;

    /* NID */
    CreateLabel(hwnd, hInst, -1, cx, cy + 1, labelW, 24, L"NID:", pData->base.hFont);
    pData->hEditNid = CreateWindowExW(0, L"STATIC", L"00000000",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        cx + labelW + 4, cy + 1, 90, 20, hwnd, (HMENU)IDC_EDIT_LORA_NID, hInst, NULL);
    SendMessageW(pData->hEditNid, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);

    pData->hBtnQueryNid = CreateWindowExW(0, L"BUTTON", L"查询",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, btnX, cy, btnW, 24,
        hwnd, (HMENU)IDC_BUTTON_LORA_QUERY_NID, hInst, NULL);
    SendMessageW(pData->hBtnQueryNid, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    /* Status label 放到 Group 最底部 */
    int statusY = grp2Y + grp2H - 26;
    pData->hLabelLoraStatus = CreateWindowExW(0, L"STATIC",
        L"请先上电 LoRa",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        cx, statusY, 220, 20,
        hwnd, (HMENU)IDC_LABEL_LORA_STATUS, hInst, NULL);
    SendMessageW(pData->hLabelLoraStatus, WM_SETFONT,
        (WPARAM)pData->base.hFont, TRUE);

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
    SendMessageW(pData->hEditMonitor, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);
    SendMessageW(pData->hEditMonitor, EM_LIMITTEXT, 0x7FFFFFFE, 0);
    cx = grp3X + grp3W - 10;
    cy = grp3Y + grp3H - 34;

    pData->hBtnClearMonitor = CreateWindowExW(0, L"BUTTON",
        L"清除",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        cx - 90, cy, 80, 28,
        hwnd, (HMENU)IDC_BUTTON_CLEAR_MONITOR, hInst, NULL);
    SendMessageW(pData->hBtnClearMonitor, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);

    pData->hCheckAutoScroll = CreateWindowExW(0, L"BUTTON",
        L"自动滚动",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        grp3X + 10, cy, 110, 24,
        hwnd, (HMENU)IDC_CHECK_AUTOSCROLL, hInst, NULL);
    SendMessageW(pData->hCheckAutoScroll, WM_SETFONT, (WPARAM)pData->base.hFont, TRUE);
    SendMessageW(pData->hCheckAutoScroll, BM_SETCHECK, BST_CHECKED, 0);

    /* Set up frame callback */
    CanCommand_SetFrameCallback(pData->canCmd, CanFrameCb, (void *)hwnd);

    /* Initial control states */
    UpdateControlStates(pData);
}

static void can_command_on_size(HWND hwnd, void *data, int cx, int cy)
{
    TAB_CMD_DATA *pData = (TAB_CMD_DATA *)data;

    int margin = 14;
    int grp1W = 480, grp1H = 420;

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
}

static void can_command_on_destroy(HWND hwnd, void *data)
{
    TAB_CMD_DATA *pData = (TAB_CMD_DATA *)data;
    if (pData->isActive) {
        CanCommand_StopMonitor(pData->canCmd);
        pData->isActive = 0;
    }
    CanCommand_SetFrameCallback(pData->canCmd, NULL, NULL);
}

static LRESULT can_command_on_message(HWND hwnd, void *data, UINT uMsg,
                                       WPARAM wParam, LPARAM lParam)
{
    TAB_CMD_DATA *pData = (TAB_CMD_DATA *)data;

    switch (uMsg) {

    case WM_SHOWWINDOW: {
        if (wParam && pData->channel != CAN_HAL_INVALID_HANDLE && !pData->isActive) {
            pData->isActive = 1;
            CanCommand_StartMonitor(pData->canCmd);
        } else if (!wParam && pData->isActive) {
            pData->isActive = 0;
            CanCommand_StopMonitor(pData->canCmd);
        }
        return TAB_MSG_HANDLED;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case IDC_BUTTON_CAN_SEND: {
            if (!pData->canCmd || pData->channel == CAN_HAL_INVALID_HANDLE)
                return TAB_MSG_HANDLED;

            wchar_t idStr[32];
            GetWindowTextW(pData->hEditCanId, idStr, 32);
            uint32_t can_id = (uint32_t)wcstoul(idStr, NULL, 16);

            wchar_t dataStr[128];
            GetWindowTextW(pData->hEditCanData, dataStr, 128);
            uint8_t fdata[8] = {0};
            int dlc = ParseHexData(dataStr, fdata, 8);

            int is_extended = (SendMessageW(pData->hRadioExtFrame, BM_GETCHECK, 0, 0) == BST_CHECKED);
            int is_remote = (SendMessageW(pData->hRadioRemoteFrame, BM_GETCHECK, 0, 0) == BST_CHECKED);

            int result = CanCommand_SendFrame(pData->canCmd, can_id, fdata, dlc,
                                               is_extended, is_remote);
            if (!result) {
                MessageBoxW(hwnd,
                    L"发送失败",
                    L"错误",
                    MB_OK | MB_ICONERROR);
            }
            return TAB_MSG_HANDLED;
        }

        case IDC_BUTTON_CLEAR_MONITOR: {
            HWND hMon = pData->hEditMonitor;
            ShowWindow(hMon, SW_HIDE);
            SetWindowTextW(hMon, L"");
            RedrawWindow(hMon, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
            ShowWindow(hMon, SW_SHOW);
            return TAB_MSG_HANDLED;
        }

        /* LoRa power toggle */
        case IDC_BUTTON_LORA_POWER:
            SendLoraCommand(pData, LORA_CMD_SET_POWER);
            return TAB_MSG_HANDLED;

        /* LoRa test mode toggle */
        case IDC_BUTTON_LORA_TEST:
            SendLoraCommand(pData, LORA_CMD_SET_TEST);
            return TAB_MSG_HANDLED;

        /* Query all LoRa config */
        case IDC_BUTTON_LORA_QUERY_CFG:
            pData->pendingQueryCount = 6;
            SendLoraCommand(pData, LORA_CMD_QUERY_MODE);
            SendLoraCommand(pData, LORA_CMD_QUERY_CH1);
            SendLoraCommand(pData, LORA_CMD_QUERY_CH2);
            SendLoraCommand(pData, LORA_CMD_QUERY_PNUM);
            SendLoraCommand(pData, LORA_CMD_QUERY_NID);
            SendLoraCommand(pData, LORA_CMD_QUERY_GWID);
            return TAB_MSG_HANDLED;

        case IDC_BUTTON_LORA_SET_MODE:
            SendLoraCommand(pData, LORA_CMD_SET_MODE);
            return TAB_MSG_HANDLED;

        case IDC_BUTTON_LORA_SET_CH1:
            SendLoraCommand(pData, LORA_CMD_SET_CH1);
            return TAB_MSG_HANDLED;

        case IDC_BUTTON_LORA_SET_CH2:
            SendLoraCommand(pData, LORA_CMD_SET_CH2);
            return TAB_MSG_HANDLED;

        case IDC_BUTTON_LORA_SET_PNUM:
            SendLoraCommand(pData, LORA_CMD_SET_PNUM);
            return TAB_MSG_HANDLED;

        case IDC_BUTTON_LORA_QUERY_NID:
            SendLoraCommand(pData, LORA_CMD_QUERY_NID);
            return TAB_MSG_HANDLED;

        case IDC_BUTTON_LORA_QUERY_GWID:
            SendLoraCommand(pData, LORA_CMD_QUERY_GWID);
            return TAB_MSG_HANDLED;

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
            return TAB_MSG_HANDLED;
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
                        int freqSel = (ch - 4100) / 100;
                        if (freqSel >= 0 && freqSel <= 10)
                            SendMessageW(pData->hComboFreq1, CB_SETCURSEL, freqSel, 0);
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
                        int freqSel = (ch - 4100) / 100;
                        if (freqSel >= 0 && freqSel <= 10)
                            SendMessageW(pData->hComboFreq2, CB_SETCURSEL, freqSel, 0);
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
        return TAB_MSG_HANDLED;
    }

    } /* switch (uMsg) */

    return TAB_MSG_NOT_HANDLED;
}

/* ------------------------------------------------------------------ */
/*  Vtable + Public API                                               */
/* ------------------------------------------------------------------ */

static const TAB_IFACE g_can_cmd_iface = {
    .data_size  = sizeof(TAB_CMD_DATA),
    .on_create  = can_command_on_create,
    .on_size    = can_command_on_size,
    .on_destroy = can_command_on_destroy,
    .on_message = can_command_on_message,
};

HWND TabCanCommand_Create(HWND hParent, HINSTANCE hInst, CanCommand *cmd)
{
    return TabBase_CreatePage(hParent, hInst, &g_can_cmd_iface, cmd);
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
