/**
 * Tab 2: CAN Command Sending Page
 *
 * Provides custom frame sending, quick commands, and a bus monitor.
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"
#include "can_command.h"

/* ------------------------------------------------------------------ */
/*  Control IDs for dynamically-created quick-command buttons          */
/* ------------------------------------------------------------------ */
#define IDC_QUICK_CMD_BASE  0x8000

/* ------------------------------------------------------------------ */
/*  Per-window instance data (stored via GWLP_USERDATA)                */
/* ------------------------------------------------------------------ */
typedef struct {
    CanCommand  *canCmd;
    TPCANHandle  channel;
    int          isActive;

    /* Child control handles */
    HWND hEditCanId;
    HWND hRadioStdFrame;
    HWND hRadioExtFrame;
    HWND hRadioDataFrame;
    HWND hRadioRemoteFrame;
    HWND hEditCanData;
    HWND hBtnSend;
    HWND hEditMonitor;
    HWND hCheckAutoScroll;
    HWND hBtnClearMonitor;
    HWND hQuickButtons[MAX_QUICK_COMMANDS];
    int  quickButtonCount;

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

/* Append a timestamped CAN frame line to the monitor edit control */
static void AppendMonitorLine(TAB_CMD_DATA *pData, int is_tx,
                               uint32_t can_id, const uint8_t *data, int dlc)
{
    HWND hMon = pData->hEditMonitor;
    if (!hMon) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t line[256];
    int pos = 0;

    /* Direction + timestamp + CAN ID */
    pos += wsprintfW(line + pos, L"%s %02d:%02d:%02d  0x%03X",
                     is_tx ? L"TX" : L"RX",
                     st.wHour, st.wMinute, st.wSecond,
                     can_id);

    /* Data bytes */
    if (data && dlc > 0) {
        for (int i = 0; i < dlc && i < 8; i++) {
            pos += wsprintfW(line + pos, L" %02X", data[i]);
        }
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
    BOOL connected = (pData->channel != PCAN_NONEBUS);
    EnableWindow(pData->hBtnSend, connected);
    EnableWindow(pData->hEditCanId, connected);
    EnableWindow(pData->hEditCanData, connected);
    for (int i = 0; i < pData->quickButtonCount; i++)
        EnableWindow(pData->hQuickButtons[i], connected);
}

/* Parse hex data bytes from a space-separated string, return count */
static int ParseHexData(const wchar_t *str, uint8_t *out, int maxBytes)
{
    int count = 0;
    const wchar_t *p = str;

    while (*p && count < maxBytes) {
        /* Skip leading whitespace */
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;

        /* Parse one hex byte */
        wchar_t *end;
        unsigned long val = wcstoul(p, &end, 16);
        if (end == p) break;  /* no conversion */
        out[count++] = (uint8_t)(val & 0xFF);
        p = end;
    }
    return count;
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

        /* Recover the CanCommand pointer passed via lpCreateParams */
        pData->canCmd = (CanCommand *)cs->lpCreateParams;
        pData->channel = PCAN_NONEBUS;
        pData->isActive = 0;
        pData->quickButtonCount = 0;

        /* Create fonts */
        pData->hFont = CreateFontW(
            16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");
        pData->hFontBold = CreateFontW(
            16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");
        pData->hFontMono = CreateFontW(
            13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
            L"Consolas");

        /* ---- Layout coordinates ---- */
        int margin = 14;
        int lineH = 30;
        int cx, cy;

        /* ========== Group 1: Frame Configuration (left side, upper) ========== */
        int grp1X = margin, grp1Y = margin;
        int grp1W = 480, grp1H = 260;
        CreateWindowExW(0, L"BUTTON", L"\x5E27\x914D\x7F6E",
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
            L"\x5E27\x683C\x5F0F:", pData->hFont);
        pData->hRadioStdFrame = CreateWindowExW(0, L"BUTTON",
            L"\x6807\x51C6\x5E27",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            cx + 78, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_STD_FRAME, hInst, NULL);
        SendMessageW(pData->hRadioStdFrame, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        pData->hRadioExtFrame = CreateWindowExW(0, L"BUTTON",
            L"\x6269\x5C55\x5E27",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            cx + 174, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_EXT_FRAME, hInst, NULL);
        SendMessageW(pData->hRadioExtFrame, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        cy += lineH;

        /* Row 3: Frame Type */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 70, 22,
            L"\x5E27\x7C7B\x578B:", pData->hFont);
        pData->hRadioDataFrame = CreateWindowExW(0, L"BUTTON",
            L"\x6570\x636E\x5E27",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            cx + 78, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_DATA_FRAME, hInst, NULL);
        SendMessageW(pData->hRadioDataFrame, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        pData->hRadioRemoteFrame = CreateWindowExW(0, L"BUTTON",
            L"\x8FDC\x7A0B\x5E27",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            cx + 174, cy, 90, 24, hwnd, (HMENU)IDC_RADIO_REMOTE_FRAME, hInst, NULL);
        SendMessageW(pData->hRadioRemoteFrame, WM_SETFONT, (WPARAM)pData->hFont, TRUE);
        cy += lineH;

        /* Row 4: Data bytes */
        CreateLabel(hwnd, hInst, -1, cx, cy + 3, 70, 22,
            L"\x6570\x636E:", pData->hFont);
        pData->hEditCanData = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            L"01 02 03 04 05 06 07 08",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            cx + 78, cy, 300, 24, hwnd, (HMENU)IDC_EDIT_CAN_DATA, hInst, NULL);
        SendMessageW(pData->hEditCanData, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);
        cy += lineH + 4;

        /* Row 5: Send button (right-aligned within group) */
        pData->hBtnSend = CreateWindowExW(0, L"BUTTON",
            L"\x53D1\x9001\x5E27",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            grp1X + grp1W - 14 - 140, cy, 140, 32, hwnd, (HMENU)IDC_BUTTON_CAN_SEND, hInst, NULL);
        SendMessageW(pData->hBtnSend, WM_SETFONT, (WPARAM)pData->hFontBold, TRUE);

        /* ========== Group 2: Quick Commands (right side, upper) ========== */
        int grp2X = 510;
        int grp2Y = grp1Y;
        int grp2W = 648, grp2H = 260;
        CreateWindowExW(0, L"BUTTON",
            L"\x5FEB\x6377\x547D\x4EE4",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            grp2X, grp2Y, grp2W, grp2H, hwnd, NULL, hInst, NULL);

        /* Create quick command buttons dynamically */
        cx = grp2X + 14;
        cy = grp2Y + 30;
        int btnW = 145, btnH = 36, btnGap = 10;

        int cmdCount = CanCommand_GetQuickCommandCount(pData->canCmd);
        for (int i = 0; i < cmdCount && i < MAX_QUICK_COMMANDS; i++) {
            const CanQuickCommand *qc = CanCommand_GetQuickCommand(pData->canCmd, i);
            if (!qc) break;

            /* Convert name from UTF-8 to wide string */
            wchar_t wname[QUICK_CMD_NAME_LEN];
            MultiByteToWideChar(CP_UTF8, 0, qc->name, -1, wname, QUICK_CMD_NAME_LEN);

            int col = i % 4;
            int row = i / 4;
            pData->hQuickButtons[i] = CreateWindowExW(0, L"BUTTON", wname,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                cx + col * (btnW + btnGap),
                cy + row * (btnH + btnGap),
                btnW, btnH,
                hwnd, (HMENU)(INT_PTR)(IDC_QUICK_CMD_BASE + i), hInst, NULL);
            SendMessageW(pData->hQuickButtons[i], WM_SETFONT, (WPARAM)pData->hFont, TRUE);
            pData->quickButtonCount++;
        }

        /* ========== Group 3: Bus Monitor (full width, bottom) ========== */
        int grp3X = margin;
        int grp3Y = grp1Y + grp1H + 10;
        int grp3W = 1144;
        int grp3H = 1010 - grp3Y - 10;
        CreateWindowExW(0, L"BUTTON",
            L"\x603B\x7EBF\x76D1\x89C6",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            grp3X, grp3Y, grp3W, grp3H, hwnd, NULL, hInst, NULL);

        /* Monitor edit: multi-line, read-only, vertical+horizontal scroll */
        int monX = grp3X + 10;
        int monY = grp3Y + 24;
        int monW = grp3W - 20;
        int monH = grp3H - 68;
        pData->hEditMonitor = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            monX, monY,
            monW, monH,
            hwnd, (HMENU)IDC_EDIT_CAN_MONITOR, hInst, NULL);
        SendMessageW(pData->hEditMonitor, WM_SETFONT, (WPARAM)pData->hFontMono, TRUE);

        /* Auto-scroll checkbox + Clear button (right-aligned) */
        cx = grp3X + grp3W - 10;
        cy = grp3Y + grp3H - 34;

        pData->hBtnClearMonitor = CreateWindowExW(0, L"BUTTON",
            L"\x6E05\x9664",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx - 90, cy, 80, 28, hwnd, (HMENU)IDC_BUTTON_CLEAR_MONITOR, hInst, NULL);
        SendMessageW(pData->hBtnClearMonitor, WM_SETFONT, (WPARAM)pData->hFont, TRUE);

        pData->hCheckAutoScroll = CreateWindowExW(0, L"BUTTON",
            L"\x81EA\x52A8\x6EDA\x52A8",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            grp3X + 10, cy, 110, 24, hwnd, (HMENU)IDC_CHECK_AUTOSCROLL, hInst, NULL);
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
            if (!pData->canCmd || pData->channel == PCAN_NONEBUS)
                return 0;

            /* Parse CAN ID */
            wchar_t idStr[32];
            GetWindowTextW(pData->hEditCanId, idStr, 32);
            uint32_t can_id = (uint32_t)wcstoul(idStr, NULL, 16);

            /* Parse data bytes */
            wchar_t dataStr[128];
            GetWindowTextW(pData->hEditCanData, dataStr, 128);
            uint8_t data[8] = {0};
            int dlc = ParseHexData(dataStr, data, 8);

            /* Frame format */
            int is_extended = (SendMessageW(pData->hRadioExtFrame, BM_GETCHECK, 0, 0) == BST_CHECKED);
            int is_remote = (SendMessageW(pData->hRadioRemoteFrame, BM_GETCHECK, 0, 0) == BST_CHECKED);

            int result = CanCommand_SendFrame(pData->canCmd, can_id, data, dlc,
                                               is_extended, is_remote);
            if (!result) {
                MessageBoxW(hwnd,
                    L"\x53D1\x9001\x5931\x8D25",
                    L"\x9519\x8BEF",
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

        default:
            /* Quick command buttons */
            if (LOWORD(wParam) >= IDC_QUICK_CMD_BASE &&
                LOWORD(wParam) < IDC_QUICK_CMD_BASE + (UINT)pData->quickButtonCount) {

                if (!pData->canCmd || pData->channel == PCAN_NONEBUS)
                    return 0;

                int idx = LOWORD(wParam) - IDC_QUICK_CMD_BASE;
                const CanQuickCommand *qc = CanCommand_GetQuickCommand(pData->canCmd, idx);
                if (!qc) return 0;

                int result = CanCommand_SendFrame(pData->canCmd,
                    qc->can_id, qc->data, qc->dlc,
                    qc->is_extended, qc->is_remote);
                if (!result) {
                    MessageBoxW(hwnd,
                        L"\x53D1\x9001\x5931\x8D25",
                        L"\x9519\x8BEF",
                        MB_OK | MB_ICONERROR);
                }
                return 0;
            }
            break;
        }
        break;

    /* ---- CAN frame received (from monitor thread via PostMessage) ---- */
    case WM_CAN_FRAME_RECEIVED: {
        FrameInfo *fi = (FrameInfo *)lParam;
        if (fi) {
            AppendMonitorLine(pData, fi->is_tx, fi->id, fi->data, fi->dlc);
            free(fi);
        }
        return 0;
    }

    /* ---- Show/Hide handling for monitor start/stop ---- */
    case WM_SHOWWINDOW: {
        if (wParam && pData->channel != PCAN_NONEBUS && !pData->isActive) {
            /* Tab becoming visible and connected -- start monitor */
            pData->isActive = 1;
            CanCommand_StartMonitor(pData->canCmd);
        } else if (!wParam && pData->isActive) {
            /* Tab becoming hidden -- stop monitor */
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

void TabCanCommand_UpdateChannel(HWND hwnd, TPCANHandle channel)
{
    TAB_CMD_DATA *pData = GetTabPageData(hwnd);
    if (!pData) return;

    pData->channel = channel;
    CanCommand_SetChannel(pData->canCmd, channel);
    UpdateControlStates(pData);

    /* If tab is active and channel just became connected, start monitor */
    if (pData->isActive && channel != PCAN_NONEBUS) {
        CanCommand_StartMonitor(pData->canCmd);
    } else if (channel == PCAN_NONEBUS) {
        CanCommand_StopMonitor(pData->canCmd);
    }
}
