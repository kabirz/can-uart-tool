/**
 * Tab 2: LoRa 配置 (via UDP AT commands)
 *
 * Device discovery, network settings, LoRa protocol parameters,
 * AT command console, and response log.
 * Uses loralib SDK (lora_sdk.h) for all UDP communication.
 */
#include <windows.h>
#include <commctrl.h>
#include <setupapi.h>
#include <devguid.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "resource.h"
#include "lora_sdk.h"
#include "tab_base.h"

/* ------------------------------------------------------------------ */
/*  Message payload structures (heap-allocated, receiver frees)        */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  Per-window instance data (stored via GWLP_USERDATA)                */
/* ------------------------------------------------------------------ */
typedef struct {
    TAB_BASE base;              /* MUST be first member */

    lora_sdk_t *sdk;

    /* Group 1: Device discovery */
    HWND hBtnSearch;
    HWND hBtnGetNet;
    HWND hBtnQueryGwid;
    HWND hBtnQueryCsq;
    HWND hMacText;
    HWND hDevText;
    HWND hSwText;
    HWND hGwidText;
    HWND hCsqText;

    /* Group 2: Network settings */
    HWND hDhcpText;
    HWND hBtnDhcpQuery;
    HWND hBtnDhcpOn;
    HWND hBtnDhcpOff;
    HWND hComboOption;
    HWND hBtnOptionSet;
    HWND hBtnOptionQuery;
    HWND hEditIp;
    HWND hBtnIpSet;
    HWND hBtnIpQuery;
    HWND hEditMask;
    HWND hBtnMaskSet;
    HWND hBtnMaskQuery;
    HWND hEditGw;
    HWND hBtnGwSet;
    HWND hBtnGwQuery;

    /* Group 3: LoRa protocol */
    HWND hComboNwmode;
    HWND hBtnNwmodeSet;
    HWND hBtnNwmodeQuery;
    HWND hComboTtmode;
    HWND hBtnTtmodeSet;
    HWND hBtnTtmodeQuery;
    HWND hComboWmode;
    HWND hBtnWmodeSet;
    HWND hBtnWmodeQuery;
    HWND hUpwidText;
    HWND hBtnUpwidQuery;
    HWND hBtnUpwidOn;
    HWND hBtnUpwidOff;
    HWND hComboCh;
    HWND hComboFreq;
    HWND hBtnChSet;
    HWND hBtnChQuery;
    HWND hComboSpd;
    HWND hBtnSpdSet;
    HWND hBtnSpdQuery;
    HWND hComboPwr;
    HWND hBtnPwrSet;
    HWND hBtnPwrQuery;

    /* Group 4: AT command */
    HWND hEditAtCmd;
    HWND hBtnAtSend;
    HWND hBtnQueryVer;
    HWND hBtnReboot;

    /* Group 5: Log */
    HWND hLogEdit;
    HWND hBtnClear;

    /* Group 2 extras: Socket (SOCKA) — inside Network Settings group */
    HWND hComboSockaMode;
    HWND hEditSockaIp;
    HWND hEditSockaRPort;
    HWND hEditSockaLPort;
    HWND hBtnSockaSet;
    HWND hBtnSockaQuery;

    /* Resizable group boxes */
    HWND hGrpDev;
    HWND hGrpNet;
    HWND hGrpProto;
    HWND hGrpAt;
    HWND hGrpLog;
    HWND hGrpTransport;

    /* Transport & serial controls */
    HWND hComboTransport;
    HWND hComboComPort;
    HWND hComboBaud;
    HWND hBtnSerialOpen;
    HWND hBtnSerialRefresh;
    HWND hSerialStatus;
    HWND hLblComPort;
    HWND hLblBaud;
} TAB_LORA_CFG;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/* Create a static label (forces height to 24 for 24px font) */
static HWND CreateLabel(HWND hParent, HINSTANCE hInst, int id,
                         int x, int y, int w, int h,
                         const wchar_t *text, HFONT hFont)
{
    (void)h;
    HWND hw = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, 24, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

/* Create a push button */
static HWND CreateBtn(HWND hParent, HINSTANCE hInst, int id,
                       int x, int y, int w, int h,
                       const wchar_t *text, HFONT hFont)
{
    HWND hw = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, w, h, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

/* Create a combo box (dropdown list) */
static HWND CreateCombo(HWND hParent, HINSTANCE hInst, int id,
                         int x, int y, int w, int h,
                         HFONT hFont)
{
    HWND hw = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, y, w, h, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

/* Create an edit control (forces height to 26 for 24px font) */
static HWND CreateEdit(HWND hParent, HINSTANCE hInst, int id,
                        int x, int y, int w, int h,
                        const wchar_t *text, HFONT hFont, DWORD extraStyle)
{
    (void)h;
    HWND hw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extraStyle,
        x, y, w, 26, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

/* Create a read-only static text field (forces height to 26 for 24px font) */
static HWND CreateStaticText(HWND hParent, HINSTANCE hInst, int id,
                              int x, int y, int w, int h,
                              const wchar_t *text, HFONT hFont)
{
    (void)h;
    HWND hw = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_SUNKEN,
        x, y, w, 26, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

/* Send AT command via SDK */
static void SendAtCmd(TAB_LORA_CFG *pData, const char *cmd)
{
    if (pData->sdk)
        lora_sdk_send_at(pData->sdk, cmd);
}

/* Append timestamped text to log edit */
static void AppendLog(TAB_LORA_CFG *pData, const char *text)
{
    if (!pData->hLogEdit) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t ts[32];
    _snwprintf(ts, 32, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    /* Convert UTF-8 text to wide string */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wlen <= 0) return;
    wchar_t *wtext = (wchar_t *)malloc(wlen * sizeof(wchar_t));
    if (!wtext) return;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wlen);

    int totalLen = GetWindowTextLengthW(pData->hLogEdit);
    if (totalLen > 100000) {
        SendMessageW(pData->hLogEdit, EM_SETSEL, 0, totalLen / 4);
        SendMessageW(pData->hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
        totalLen = GetWindowTextLengthW(pData->hLogEdit);
    }

    SendMessageW(pData->hLogEdit, EM_SETSEL, totalLen, totalLen);
    SendMessageW(pData->hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)ts);
    SendMessageW(pData->hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)wtext);
    /* Add newline if not already present */
    int textLen = (int)wcslen(wtext);
    if (textLen == 0 || (wtext[textLen - 1] != L'\n' && wtext[textLen - 1] != L'\r')) {
        SendMessageW(pData->hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    }
    SendMessageW(pData->hLogEdit, EM_SCROLLCARET, 0, 0);
    free(wtext);
}

/* Parse AT response and update corresponding controls — matches tools' lora_udp.c */
static void ParseAtResponse(TAB_LORA_CFG *pData, const char *resp)
{
    if (!resp) return;

    const char *p;
    int val;
    wchar_t wbuf[128];

    /* +NWMODE: */
    p = strstr(resp, "+NWMODE:");
    if (p) {
        if (sscanf(p + 8, "%d", &val) == 1) {
            if (val >= 0 && val <= 1)
                SendMessageW(pData->hComboNwmode, CB_SETCURSEL, val, 0);
        }
    }

    /* +TTMODE: */
    p = strstr(resp, "+TTMODE:");
    if (p) {
        if (sscanf(p + 8, "%d", &val) == 1) {
            if (val >= 0 && val <= 1)
                SendMessageW(pData->hComboTtmode, CB_SETCURSEL, val, 0);
        }
    }

    /* +WMODE: */
    p = strstr(resp, "+WMODE:");
    if (p) {
        if (sscanf(p + 7, "%d", &val) == 1) {
            if (val >= 0 && val <= 2)
                SendMessageW(pData->hComboWmode, CB_SETCURSEL, val, 0);
        }
    }

    /* UPWID: */
    p = strstr(resp, "UPWID:");
    if (p) {
        const char *valStart = p + 6;
        while (*valStart == ' ') valStart++;
        MultiByteToWideChar(CP_ACP, 0, valStart, -1, wbuf, 128);
        for (wchar_t *c = wbuf; *c; c++) {
            if (*c == L'\r' || *c == L'\n' || *c == L' ') { *c = L'\0'; break; }
        }
        wchar_t txt[64];
        wsprintfW(txt, L"UPWID: %s", wbuf);
        SetWindowTextW(pData->hUpwidText, txt);
    }

    /* +DHCP: */
    p = strstr(resp, "+DHCP:");
    if (p) {
        const char *valStart = p + 6;
        while (*valStart == ' ') valStart++;
        MultiByteToWideChar(CP_ACP, 0, valStart, -1, wbuf, 128);
        for (wchar_t *c = wbuf; *c; c++) {
            if (*c == L'\r' || *c == L'\n' || *c == L' ') { *c = L'\0'; break; }
        }
        SetWindowTextW(pData->hDhcpText, wbuf);
    }

    /* +OPTION: */
    p = strstr(resp, "+OPTION:");
    if (p) {
        if (sscanf(p + 8, "%d", &val) == 1) {
            if (val >= 0 && val <= 4)
                SendMessageW(pData->hComboOption, CB_SETCURSEL, val, 0);
        }
    }

    /* GWID: */
    p = strstr(resp, "GWID:");
    if (p) {
        const char *valStart = p + 5;
        while (*valStart == ' ') valStart++;
        MultiByteToWideChar(CP_ACP, 0, valStart, -1, wbuf, 128);
        for (wchar_t *c = wbuf; *c; c++) {
            if (*c == L'\r' || *c == L'\n' || *c == L' ') { *c = L'\0'; break; }
        }
        SetWindowTextW(pData->hGwidText, wbuf);
    }

    /* +CSQ: */
    p = strstr(resp, "+CSQ:");
    if (p) {
        const char *valStart = p + 5;
        while (*valStart == ' ') valStart++;
        MultiByteToWideChar(CP_ACP, 0, valStart, -1, wbuf, 128);
        for (wchar_t *c = wbuf; *c; c++) {
            if (*c == L'\r' || *c == L'\n' || *c == L' ') { *c = L'\0'; break; }
        }
        SetWindowTextW(pData->hCsqText, wbuf);
    }

    /* +GWIP: — update IP edit */
    {
        char v[64];
        const char *gp = strstr(resp, "+GWIP:");
        if (gp) {
            gp += 6;
            int i = 0;
            while (gp[i] && gp[i] != '\r' && gp[i] != '\n' && gp[i] != ' ' && i < 63) i++;
            if (i > 0 && strncmp(gp, "OK", 2) != 0) {
                MultiByteToWideChar(CP_ACP, 0, gp, i, wbuf, 64);
                wbuf[i] = L'\0';
                SetWindowTextW(pData->hEditIp, wbuf);
            }
        }
    }

    /* +GW: — update gateway edit */
    {
        const char *gp = strstr(resp, "+GW:");
        if (gp) {
            gp += 4;
            int i = 0;
            while (gp[i] && gp[i] != '\r' && gp[i] != '\n' && gp[i] != ' ' && i < 63) i++;
            if (i > 0 && strncmp(gp, "OK", 2) != 0) {
                MultiByteToWideChar(CP_ACP, 0, gp, i, wbuf, 64);
                wbuf[i] = L'\0';
                SetWindowTextW(pData->hEditGw, wbuf);
            }
        }
    }

    /* +MASK: — update mask edit */
    {
        const char *mp = strstr(resp, "+MASK:");
        if (mp) {
            mp += 6;
            int i = 0;
            while (mp[i] && mp[i] != '\r' && mp[i] != '\n' && mp[i] != ' ' && i < 63) i++;
            if (i > 0 && strncmp(mp, "OK", 2) != 0) {
                MultiByteToWideChar(CP_ACP, 0, mp, i, wbuf, 64);
                wbuf[i] = L'\0';
                SetWindowTextW(pData->hEditMask, wbuf);
            }
        }
    }

    /* +CH<n>:<freq> — update freq combo */
    {
        const char *cp = strstr(resp, "+CH");
        if (cp && cp[3] >= '0' && cp[3] <= '9') {
            const char *colon = strchr(cp + 3, ':');
            if (colon) {
                const char *v = colon + 1;
                if (strncmp(v, "OK", 2) != 0) {
                    int freq = atoi(v);
                    int idx = (freq - 4100) / 100;
                    if (idx >= 0 && idx <= 10)
                        SendMessageW(pData->hComboFreq, CB_SETCURSEL, idx, 0);
                }
            }
        }
    }

    /* +SPD<n>:<spd> — update spd combo */
    {
        const char *sp = strstr(resp, "+SPD");
        if (sp && sp[4] >= '0' && sp[4] <= '9') {
            const char *colon = strchr(sp + 4, ':');
            if (colon) {
                const char *v = colon + 1;
                if (strncmp(v, "OK", 2) != 0) {
                    int spd = atoi(v);
                    int idx = spd - 4;
                    if (idx >= 0 && idx <= 7)
                        SendMessageW(pData->hComboSpd, CB_SETCURSEL, idx, 0);
                }
            }
        }
    }

    /* +PWR<n>:<pwr> — update pwr combo */
    {
        const char *pp = strstr(resp, "+PWR");
        if (pp && pp[4] >= '0' && pp[4] <= '9') {
            const char *colon = strchr(pp + 4, ':');
            if (colon) {
                const char *v = colon + 1;
                if (strncmp(v, "OK", 2) != 0) {
                    int pwr = atoi(v);
                    int idx = pwr - 24;
                    if (idx >= 0 && idx <= 6)
                        SendMessageW(pData->hComboPwr, CB_SETCURSEL, idx, 0);
                }
            }
        }
    }

    /* +SOCKA:<mode>,<ip>,<remote_port>,<local_port> */
    {
        const char *sp = strstr(resp, "+SOCKA:");
        if (sp) {
            sp += 7;
            char mode[8] = {0}, ip[64] = {0};
            int rport = 0, lport = 0;
            if (sscanf(sp, "%7[^,],%63[^,],%d,%d", mode, ip, &rport, &lport) >= 4) {
                if (strcmp(mode, "TCPC") == 0)
                    SendMessageW(pData->hComboSockaMode, CB_SETCURSEL, 0, 0);
                else if (strcmp(mode, "TCPS") == 0)
                    SendMessageW(pData->hComboSockaMode, CB_SETCURSEL, 1, 0);

                wchar_t wip[64], wrp[16], wlp[16];
                MultiByteToWideChar(CP_ACP, 0, ip, -1, wip, 64);
                wsprintfW(wrp, L"%d", rport);
                wsprintfW(wlp, L"%d", lport);
                SetWindowTextW(pData->hEditSockaIp, wip);
                SetWindowTextW(pData->hEditSockaRPort, wrp);
                SetWindowTextW(pData->hEditSockaLPort, wlp);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Transport / Serial helpers                                        */
/* ------------------------------------------------------------------ */

/* Enable or disable controls based on selected transport */
static void UpdateTransportUI(TAB_LORA_CFG *pData)
{
    int sel = (int)SendMessageW(pData->hComboTransport, CB_GETCURSEL, 0, 0);
    int isSerial = (sel == 1); /* index 0=UDP, 1=串口 */
    int serialOpen = 0;
    int showSerial = isSerial ? SW_SHOW : SW_HIDE;

    if (pData->sdk)
        serialOpen = lora_sdk_serial_is_open(pData->sdk);

    /* Serial port controls: show only when serial transport selected */
    ShowWindow(pData->hLblComPort,         showSerial);
    ShowWindow(pData->hComboComPort,       showSerial);
    ShowWindow(pData->hBtnSerialRefresh,   showSerial);
    ShowWindow(pData->hLblBaud,            showSerial);
    ShowWindow(pData->hComboBaud,          showSerial);
    ShowWindow(pData->hBtnSerialOpen,      showSerial);
    ShowWindow(pData->hSerialStatus,       showSerial);

    /* Update open/close button text */
    if (serialOpen)
        SetWindowTextW(pData->hBtnSerialOpen, L"关闭串口");
    else
        SetWindowTextW(pData->hBtnSerialOpen, L"打开串口");

    /* Update status text */
    if (serialOpen) {
        SetWindowTextW(pData->hSerialStatus, L"已连接");
    } else {
        SetWindowTextW(pData->hSerialStatus, L"未连接");
    }

    /* Update SDK transport */
    if (pData->sdk) {
        if (isSerial)
            lora_sdk_set_at_transport(pData->sdk, LORA_SDK_AT_TRANSPORT_SERIAL);
        else
            lora_sdk_set_at_transport(pData->sdk, LORA_SDK_AT_TRANSPORT_UDP);
    }
}

/* Enumerate available COM ports using SetupAPI (same approach as firmware upgrade) */
static void EnumerateComPorts(HWND hCombo)
{
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

    HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, 0, 0, DIGCF_PRESENT);
    if (deviceInfoSet == INVALID_HANDLE_VALUE)
        return;

    SP_DEVINFO_DATA deviceInfoData;
    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; i < 128; i++) {
        if (!SetupDiEnumDeviceInfo(deviceInfoSet, i, &deviceInfoData))
            break;

        /* Get friendly name */
        WCHAR descW[256] = {0};
        DWORD dataType = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &deviceInfoData,
                SPDRP_FRIENDLYNAME, &dataType, (PBYTE)descW, sizeof(descW), NULL)) {
            /* Fallback to device description */
            if (!SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &deviceInfoData,
                    SPDRP_DEVICEDESC, &dataType, (PBYTE)descW, sizeof(descW), NULL))
                continue;
        }

        /* Filter out Bluetooth serial ports */
        WCHAR descLower[256] = {0};
        for (int j = 0; descW[j] && j < 255; j++)
            descLower[j] = (WCHAR)tolower(descW[j]);
        if (wcsstr(descLower, L"bluetooth") || wcsstr(descLower, L"蓝牙"))
            continue;

        /* Extract COM port number from "(COMx)" */
        WCHAR *comStart = wcsstr(descW, L"(COM");
        if (comStart) {
            comStart += 4; /* skip "(COM" */
            wchar_t port[16];
            wsprintfW(port, L"COM%d", _wtoi(comStart));
            /* Avoid duplicates */
            if (SendMessageW(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)port) == CB_ERR)
                SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)port);
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    /* Select first item if any ports found */
    if (SendMessageW(hCombo, CB_GETCOUNT, 0, 0) > 0)
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  Tab Base Framework hooks                                          */
/* ------------------------------------------------------------------ */

static void lora_cfg_on_create(HWND hwnd, void *data, CREATESTRUCTW *cs)
{
    TAB_LORA_CFG *pData = (TAB_LORA_CFG *)data;
    HINSTANCE hInst = cs->hInstance;

    pData->sdk = (lora_sdk_t *)cs->lpCreateParams;

    /* Get actual client area */
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    int pageW = rcClient.right  > 0 ? rcClient.right  : WINDOW_WIDTH;
    int pageH = rcClient.bottom > 0 ? rcClient.bottom : WINDOW_HEIGHT;

    int margin = 14;
    int lineH = 34;
    int btnH = 28;
    int smallBtnW = 80;
    int smallBtnH = 28;
    int cx, cy, ox;

    /* ========== Group 0: Transport Selection ========== */
    int grp0Y = margin;
    int grp0W = pageW - 2 * margin;
    int grp0H = 62;
    pData->hGrpTransport = CreateWindowExW(0, L"BUTTON", L"连接方式",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, grp0Y, grp0W, grp0H, hwnd, NULL, hInst, NULL);

    cx = margin + 14;
    cy = grp0Y + 26;

    /* Transport combo */
    ox = cx;
    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 96, 24, L"传输方式:", pData->base.hFont);
    ox += 100;
    pData->hComboTransport = CreateCombo(hwnd, hInst, IDC_CFG_TRANSPORT_COMBO,
        ox, cy, 140, 240, pData->base.hFont);
    SendMessageW(pData->hComboTransport, CB_ADDSTRING, 0, (LPARAM)L"UDP (网络)");
    SendMessageW(pData->hComboTransport, CB_ADDSTRING, 0, (LPARAM)L"串口 (COM)");
    SendMessageW(pData->hComboTransport, CB_SETCURSEL, 0, 0);
    ox += 156;

    /* COM port */
    pData->hLblComPort = CreateLabel(hwnd, hInst, -1, ox, cy + 2, 76, 24,
                                      L"COM口:", pData->base.hFont);
    ox += 80;
    pData->hComboComPort = CreateCombo(hwnd, hInst, IDC_CFG_COMPORT_COMBO,
        ox, cy, 110, 200, pData->base.hFont);
    EnumerateComPorts(pData->hComboComPort);
    SendMessageW(pData->hComboComPort, CB_SETDROPPEDWIDTH, 140, 0);
    ox += 116;
    pData->hBtnSerialRefresh = CreateBtn(hwnd, hInst, IDC_CFG_SERIAL_REFRESH_BTN,
        ox, cy, 50, btnH, L"刷新", pData->base.hFont);
    ox += 62;

    /* Baud rate */
    pData->hLblBaud = CreateLabel(hwnd, hInst, -1, ox, cy + 2, 80, 24,
                                   L"波特率:", pData->base.hFont);
    ox += 84;
    pData->hComboBaud = CreateCombo(hwnd, hInst, IDC_CFG_BAUD_COMBO,
        ox, cy, 120, 200, pData->base.hFont);
    {
        wchar_t baudStrs[][8] = {
            L"9600", L"19200", L"38400", L"57600",
            L"115200", L"230400", L"460800", L"921600"
        };
        int baudVals[] = {
            9600, 19200, 38400, 57600,
            115200, 230400, 460800, 921600
        };
        for (int i = 0; i < 8; i++) {
            int idx = (int)SendMessageW(pData->hComboBaud, CB_ADDSTRING,
                                         0, (LPARAM)baudStrs[i]);
            SendMessageW(pData->hComboBaud, CB_SETITEMDATA, idx, (LPARAM)baudVals[i]);
        }
    }
    SendMessageW(pData->hComboBaud, CB_SETCURSEL, 4, 0); /* default 115200 */
    SendMessageW(pData->hComboBaud, CB_SETDROPPEDWIDTH, 160, 0);
    ox += 132;

    /* Open/Close button */
    pData->hBtnSerialOpen = CreateBtn(hwnd, hInst, IDC_CFG_SERIAL_OPEN_BTN,
        ox, cy, 82, btnH, L"打开串口", pData->base.hFont);
    ox += 92;

    /* Status */
    pData->hSerialStatus = CreateLabel(hwnd, hInst, IDC_CFG_SERIAL_STATUS,
        ox, cy + 2, 80, 24, L"未连接", pData->base.hFont);

    /* Apply initial transport UI state */
    UpdateTransportUI(pData);

    /* ========== Group 1: Device Discovery ========== */
    int grp1Y = grp0Y + grp0H + 6;
    int grp1W = pageW - 2 * margin;
    int grp1H = 94;
    pData->hGrpDev = CreateWindowExW(0, L"BUTTON", L"设备发现",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, grp1Y, grp1W, grp1H, hwnd, NULL, hInst, NULL);

    cx = margin + 14;
    cy = grp1Y + 26;

    /* Row 1: Action buttons */
    ox = cx;
    pData->hBtnSearch = CreateBtn(hwnd, hInst, IDC_CFG_SEARCH_BTN,
        ox, cy, 110, btnH, L"搜索设备", pData->base.hFont);
    ox += 116;
    pData->hBtnGetNet = CreateBtn(hwnd, hInst, IDC_CFG_GETNET_BTN,
        ox, cy, 110, btnH, L"获取网络", pData->base.hFont);
    ox += 116;
    pData->hBtnQueryGwid = CreateBtn(hwnd, hInst, IDC_CFG_QUERY_GWID,
        ox, cy, 110, btnH, L"查询GWID", pData->base.hFont);
    ox += 116;
    pData->hBtnQueryCsq = CreateBtn(hwnd, hInst, IDC_CFG_QUERY_CSQ,
        ox, cy, 110, btnH, L"查询信号", pData->base.hFont);
    ox += 116;
    pData->hBtnReboot = CreateBtn(hwnd, hInst, IDC_CFG_REBOOT,
        ox, cy, 110, btnH, L"重启网关", pData->base.hFont);
    cy += lineH;

    /* Row 2: All info in one line */
    ox = cx;
    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 72, 24, L"MAC:", pData->base.hFont);
    pData->hMacText = CreateStaticText(hwnd, hInst, IDC_CFG_MAC_TEXT,
        ox + 72, cy, 140, 24, L"-", pData->base.hFontMono);
    ox += 220;
    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 72, 24, L"设备:", pData->base.hFont);
    pData->hDevText = CreateStaticText(hwnd, hInst, IDC_CFG_DEV_TEXT,
        ox + 72, cy, 140, 24, L"-", pData->base.hFontMono);
    ox += 220;
    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 72, 24, L"SW:", pData->base.hFont);
    pData->hSwText = CreateStaticText(hwnd, hInst, IDC_CFG_SW_TEXT,
        ox + 72, cy, 100, 24, L"-", pData->base.hFontMono);
    ox += 180;
    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 72, 24, L"GWID:", pData->base.hFont);
    pData->hGwidText = CreateStaticText(hwnd, hInst, IDC_CFG_GWID_TEXT,
        ox + 72, cy, 100, 24, L"-", pData->base.hFontMono);
    ox += 180;
    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 72, 24, L"信号:", pData->base.hFont);
    pData->hCsqText = CreateStaticText(hwnd, hInst, IDC_CFG_CSQ_TEXT,
        ox + 72, cy, 80, 24, L"-", pData->base.hFontMono);

    /* ========== Group 2: Network Settings ========== */
    int grp2Y = grp1Y + grp1H + 6;
    int grp2W = pageW - 2 * margin;
    int grp2H = 174;
    pData->hGrpNet = CreateWindowExW(0, L"BUTTON", L"网络设置",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, grp2Y, grp2W, grp2H, hwnd, NULL, hInst, NULL);

    cx = margin + 14;
    cy = grp2Y + 26;

    /* Row 1: DHCP + Connection mode */
    ox = cx;
    CreateLabel(hwnd, hInst, -1, ox, cy + 4, 72, 20, L"DHCP:", pData->base.hFont);
    pData->hDhcpText = CreateStaticText(hwnd, hInst, IDC_CFG_DHCP_TEXT,
        ox + 76, cy, 80, 24, L"-", pData->base.hFontMono);
    ox += 164;
    pData->hBtnDhcpQuery = CreateBtn(hwnd, hInst, IDC_CFG_DHCP_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnDhcpOn = CreateBtn(hwnd, hInst, IDC_CFG_DHCP_ON,
        ox, cy, smallBtnW, smallBtnH, L"开启", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnDhcpOff = CreateBtn(hwnd, hInst, IDC_CFG_DHCP_OFF,
        ox, cy, smallBtnW, smallBtnH, L"关闭", pData->base.hFont);
    ox += smallBtnW + 24;
    CreateLabel(hwnd, hInst, -1, ox, cy + 4, 100, 20,
        L"连接模式:", pData->base.hFont);
    ox += 104;
    pData->hComboOption = CreateCombo(hwnd, hInst, IDC_CFG_OPTION_COMBO,
        ox, cy, 130, 200, pData->base.hFont);
    SendMessageW(pData->hComboOption, CB_ADDSTRING, 0, (LPARAM)L"socket");
    SendMessageW(pData->hComboOption, CB_ADDSTRING, 0, (LPARAM)L"serial");
    SendMessageW(pData->hComboOption, CB_ADDSTRING, 0, (LPARAM)L"mqtt");
    SendMessageW(pData->hComboOption, CB_ADDSTRING, 0, (LPARAM)L"ali_cloud");
    SendMessageW(pData->hComboOption, CB_ADDSTRING, 0, (LPARAM)L"usr_cloud");
    SendMessageW(pData->hComboOption, CB_SETCURSEL, 0, 0);
    ox += 136;
    pData->hBtnOptionSet = CreateBtn(hwnd, hInst, IDC_CFG_OPTION_SET,
        ox, cy, smallBtnW, smallBtnH, L"设置", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnOptionQuery = CreateBtn(hwnd, hInst, IDC_CFG_OPTION_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);
    cy += lineH;

    /* Row 2: IP + Mask */
    ox = cx;
    CreateLabel(hwnd, hInst, -1, ox, cy + 4, 72, 20, L"IP:", pData->base.hFont);
    pData->hEditIp = CreateEdit(hwnd, hInst, IDC_CFG_IP_EDIT,
        ox + 76, cy, 140, 26, L"192.168.1.100", pData->base.hFontMono, 0);
    ox += 224;
    pData->hBtnIpSet = CreateBtn(hwnd, hInst, IDC_CFG_IP_SET,
        ox, cy, smallBtnW, smallBtnH, L"设置", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnIpQuery = CreateBtn(hwnd, hInst, IDC_CFG_IP_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);
    ox += smallBtnW + 24;
    CreateLabel(hwnd, hInst, -1, ox, cy + 4, 72, 20, L"掩码:", pData->base.hFont);
    ox += 76;
    pData->hEditMask = CreateEdit(hwnd, hInst, IDC_CFG_MASK_EDIT,
        ox, cy, 140, 26, L"255.255.255.0", pData->base.hFontMono, 0);
    ox += 146;
    pData->hBtnMaskSet = CreateBtn(hwnd, hInst, IDC_CFG_MASK_SET,
        ox, cy, smallBtnW, smallBtnH, L"设置", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnMaskQuery = CreateBtn(hwnd, hInst, IDC_CFG_MASK_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);
    cy += lineH;

    /* Row 3: Gateway */
    ox = cx;
    CreateLabel(hwnd, hInst, -1, ox, cy + 4, 72, 20, L"网关:", pData->base.hFont);
    pData->hEditGw = CreateEdit(hwnd, hInst, IDC_CFG_GW_EDIT,
        ox + 76, cy, 140, 26, L"192.168.1.1", pData->base.hFontMono, 0);
    ox += 224;
    pData->hBtnGwSet = CreateBtn(hwnd, hInst, IDC_CFG_GW_SET,
        ox, cy, smallBtnW, smallBtnH, L"设置", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnGwQuery = CreateBtn(hwnd, hInst, IDC_CFG_GW_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);
    cy += lineH;

    /* Row 4: SOCKA — mode + IP + remote port + local port + buttons */
    ox = cx;
    CreateLabel(hwnd, hInst, -1, ox, cy + 4, 72, 20, L"SOCKA:", pData->base.hFont);
    ox += 76;
    pData->hComboSockaMode = CreateCombo(hwnd, hInst, IDC_CFG_SOCKA_MODE_COMBO,
        ox, cy, 90, 200, pData->base.hFont);
    SendMessageW(pData->hComboSockaMode, CB_ADDSTRING, 0, (LPARAM)L"TCPC");
    SendMessageW(pData->hComboSockaMode, CB_ADDSTRING, 0, (LPARAM)L"TCPS");
    SendMessageW(pData->hComboSockaMode, CB_SETCURSEL, 0, 0);
    ox += 96;
    CreateLabel(hwnd, hInst, -1, ox, cy + 4, 72, 20, L"IP:", pData->base.hFont);
    ox += 76;
    pData->hEditSockaIp = CreateEdit(hwnd, hInst, IDC_CFG_SOCKA_IP_EDIT,
        ox, cy, 150, 26, L"192.168.1.100", pData->base.hFontMono, 0);
    ox += 156;
    CreateLabel(hwnd, hInst, -1, ox, cy + 4, 88, 20, L"远程端口:", pData->base.hFont);
    ox += 92;
    pData->hEditSockaRPort = CreateEdit(hwnd, hInst, IDC_CFG_SOCKA_RPORT_EDIT,
        ox, cy, 70, 26, L"1883", pData->base.hFontMono, 0);
    ox += 76;
    CreateLabel(hwnd, hInst, -1, ox, cy + 4, 88, 20, L"本地端口:", pData->base.hFont);
    ox += 92;
    pData->hEditSockaLPort = CreateEdit(hwnd, hInst, IDC_CFG_SOCKA_LPORT_EDIT,
        ox, cy, 70, 26, L"1234", pData->base.hFontMono, 0);
    ox += 76;
    pData->hBtnSockaSet = CreateBtn(hwnd, hInst, IDC_CFG_SOCKA_SET,
        ox, cy, smallBtnW, smallBtnH, L"设置", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnSockaQuery = CreateBtn(hwnd, hInst, IDC_CFG_SOCKA_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);

    /* ========== Group 3: LoRa Protocol ========== */
    int grp3Y = grp2Y + grp2H + 6;
    int grp3W = pageW - 2 * margin;
    int grp3H = 130;
    pData->hGrpProto = CreateWindowExW(0, L"BUTTON", L"LoRa 协议",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, grp3Y, grp3W, grp3H, hwnd, NULL, hInst, NULL);

    cx = margin + 14;
    cy = grp3Y + 26;

    /* Row 1: NWMODE + 工作模式 */
    ox = cx;
    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 96, 24, L"是否组网:", pData->base.hFont);
    ox += 100;
    pData->hComboNwmode = CreateCombo(hwnd, hInst, IDC_CFG_NWMODE_COMBO,
        ox, cy, 170, 200, pData->base.hFont);
    SendMessageW(pData->hComboNwmode, CB_ADDSTRING, 0, (LPARAM)L"否");
    SendMessageW(pData->hComboNwmode, CB_ADDSTRING, 0, (LPARAM)L"是");
    SendMessageW(pData->hComboNwmode, CB_SETCURSEL, 0, 0);
    ox += 176;
    pData->hBtnNwmodeSet = CreateBtn(hwnd, hInst, IDC_CFG_NWMODE_SET,
        ox, cy, smallBtnW, smallBtnH, L"设置", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnNwmodeQuery = CreateBtn(hwnd, hInst, IDC_CFG_NWMODE_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);
    ox += smallBtnW + 14;

    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 96, 24, L"工作模式:", pData->base.hFont);
    ox += 100;
    pData->hComboTtmode = CreateCombo(hwnd, hInst, IDC_CFG_TTMODE_COMBO,
        ox, cy, 170, 200, pData->base.hFont);
    SendMessageW(pData->hComboTtmode, CB_ADDSTRING, 0, (LPARAM)L"广播透传");
    SendMessageW(pData->hComboTtmode, CB_ADDSTRING, 0, (LPARAM)L"指定节点");
    SendMessageW(pData->hComboTtmode, CB_SETCURSEL, 0, 0);
    ox += 176;
    pData->hBtnTtmodeSet = CreateBtn(hwnd, hInst, IDC_CFG_TTMODE_SET,
        ox, cy, smallBtnW, smallBtnH, L"设置", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnTtmodeQuery = CreateBtn(hwnd, hInst, IDC_CFG_TTMODE_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);
    cy += lineH;

    /* Row 2: UPWID + 功率 */
    ox = cx;
    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 100, 24, L"上行携带ID:", pData->base.hFont);
    ox += 104;
    pData->hUpwidText = CreateStaticText(hwnd, hInst, IDC_CFG_UPWID_TEXT,
        ox, cy, 80, 22, L"-", pData->base.hFontMono);
    ox += 86;
    pData->hBtnUpwidQuery = CreateBtn(hwnd, hInst, IDC_CFG_UPWID_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnUpwidOn = CreateBtn(hwnd, hInst, IDC_CFG_UPWID_ON,
        ox, cy, smallBtnW, smallBtnH, L"开启", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnUpwidOff = CreateBtn(hwnd, hInst, IDC_CFG_UPWID_OFF,
        ox, cy, smallBtnW, smallBtnH, L"关闭", pData->base.hFont);
    ox += smallBtnW + 14;

    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 65, 24, L"功率:", pData->base.hFont);
    ox += 69;
    pData->hComboPwr = CreateCombo(hwnd, hInst, IDC_CFG_PWR_COMBO,
        ox, cy, 78, 200, pData->base.hFont);
    for (int p = 24; p <= 30; p++) {
        wchar_t pbuf[8];
        wsprintfW(pbuf, L"%d", p);
        SendMessageW(pData->hComboPwr, CB_ADDSTRING, 0, (LPARAM)pbuf);
    }
    SendMessageW(pData->hComboPwr, CB_SETCURSEL, 6, 0); /* 30 */
    ox += 84;
    pData->hBtnPwrSet = CreateBtn(hwnd, hInst, IDC_CFG_PWR_SET,
        ox, cy, smallBtnW, smallBtnH, L"设置", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnPwrQuery = CreateBtn(hwnd, hInst, IDC_CFG_PWR_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);
    cy += lineH;

    /* Row 3: CH + Freq + SPD */
    ox = cx;
    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 65, 24, L"通道:", pData->base.hFont);
    ox += 69;
    pData->hComboCh = CreateCombo(hwnd, hInst, IDC_CFG_CH_COMBO,
        ox, cy, 78, 200, pData->base.hFont);
    SendMessageW(pData->hComboCh, CB_ADDSTRING, 0, (LPARAM)L"CH1");
    SendMessageW(pData->hComboCh, CB_ADDSTRING, 0, (LPARAM)L"CH2");
    SendMessageW(pData->hComboCh, CB_SETCURSEL, 0, 0);
    ox += 84;
    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 65, 24, L"频率:", pData->base.hFont);
    ox += 69;
    pData->hComboFreq = CreateCombo(hwnd, hInst, IDC_CFG_CH_FREQ_COMBO,
        ox, cy, 80, 200, pData->base.hFont);
    for (int f = 4100; f <= 5100; f += 100) {
        wchar_t fbuf[8];
        wsprintfW(fbuf, L"%d", f);
        SendMessageW(pData->hComboFreq, CB_ADDSTRING, 0, (LPARAM)fbuf);
    }
    SendMessageW(pData->hComboFreq, CB_SETCURSEL, 6, 0); /* 4700 */
    ox += 86;
    pData->hBtnChSet = CreateBtn(hwnd, hInst, IDC_CFG_CH_SET,
        ox, cy, smallBtnW, smallBtnH, L"设置", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnChQuery = CreateBtn(hwnd, hInst, IDC_CFG_CH_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);
    ox += smallBtnW + 14;

    CreateLabel(hwnd, hInst, -1, ox, cy + 2, 65, 24, L"速度:", pData->base.hFont);
    ox += 69;
    pData->hComboSpd = CreateCombo(hwnd, hInst, IDC_CFG_SPD_COMBO,
        ox, cy, 75, 200, pData->base.hFont);
    for (int s = 4; s <= 11; s++) {
        wchar_t sbuf[8];
        wsprintfW(sbuf, L"%d", s);
        SendMessageW(pData->hComboSpd, CB_ADDSTRING, 0, (LPARAM)sbuf);
    }
    SendMessageW(pData->hComboSpd, CB_SETCURSEL, 3, 0); /* 7 */
    ox += 81;
    pData->hBtnSpdSet = CreateBtn(hwnd, hInst, IDC_CFG_SPD_SET,
        ox, cy, smallBtnW, smallBtnH, L"设置", pData->base.hFont);
    ox += smallBtnW + 6;
    pData->hBtnSpdQuery = CreateBtn(hwnd, hInst, IDC_CFG_SPD_QUERY,
        ox, cy, smallBtnW, smallBtnH, L"查询", pData->base.hFont);

    /* ========== Group 4: AT Command ========== */
    int grp4Y = grp3Y + grp3H + 6;
    int grp4W = pageW - 2 * margin;
    int grp4H = 56;
    pData->hGrpAt = CreateWindowExW(0, L"BUTTON", L"AT 命令",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, grp4Y, grp4W, grp4H, hwnd, NULL, hInst, NULL);

    cx = margin + 14;
    cy = grp4Y + 26;

    pData->hEditAtCmd = CreateEdit(hwnd, hInst, IDC_CFG_CMD_EDIT,
        cx, cy, 400, 22, L"AT+", pData->base.hFontMono, 0);
    pData->hBtnAtSend = CreateBtn(hwnd, hInst, IDC_CFG_SEND_BTN,
        cx + 408, cy, 80, smallBtnH, L"发送", pData->base.hFont);
    pData->hBtnQueryVer = CreateBtn(hwnd, hInst, IDC_CFG_QUERY_VER,
        cx + 496, cy, 100, smallBtnH, L"查询版本", pData->base.hFont);

    /* ========== Group 5: Log (bottom, stretch height) ========== */
    int grp5Y = grp4Y + grp4H + 6;
    int grp5W = pageW - 2 * margin;
    int grp5H = pageH - grp5Y - margin;
    if (grp5H < 80) grp5H = 80;
    pData->hGrpLog = CreateWindowExW(0, L"BUTTON", L"响应日志",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, grp5Y, grp5W, grp5H, hwnd, NULL, hInst, NULL);

    int logX = margin + 10;
    int logY = grp5Y + 24;
    int logW = grp5W - 20;
    int logH = grp5H - 58;
    if (logH < 30) logH = 30;
    if (logW < 50) logW = 50;
    pData->hLogEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        logX, logY, logW, logH,
        hwnd, (HMENU)IDC_CFG_LOG_EDIT, hInst, NULL);
    SendMessageW(pData->hLogEdit, WM_SETFONT, (WPARAM)pData->base.hFontMono, TRUE);
    SendMessageW(pData->hLogEdit, EM_LIMITTEXT, 0x7FFFFFFE, 0);

    pData->hBtnClear = CreateBtn(hwnd, hInst, IDC_CFG_CLEAR_BTN,
        margin + grp5W - 100, grp5Y + grp5H - 32, 80, smallBtnH,
        L"清除", pData->base.hFont);
}

static void lora_cfg_on_size(HWND hwnd, void *data, int cx, int cy)
{
    TAB_LORA_CFG *pData = (TAB_LORA_CFG *)data;

    int margin = 14;
    int grp0H = 62;    /* Transport */
    int grp1H = 94;
    int grp2H = 174;   /* Network + SOCKA */
    int grp3H = 130;
    int grp4H = 56;    /* AT command */

    /* Group 0: Transport, full width, fixed height */
    int grp0Y = margin;
    int grp0W = cx - 2 * margin;
    if (grp0W < 200) grp0W = 200;
    MoveWindow(pData->hGrpTransport, margin, grp0Y, grp0W, grp0H, TRUE);

    /* Group 1: full width, fixed height */
    int grp1Y = grp0Y + grp0H + 6;
    int grp1W = grp0W;
    MoveWindow(pData->hGrpDev, margin, grp1Y, grp1W, grp1H, TRUE);

    /* Group 2: full width, fixed height */
    int grp2Y = grp1Y + grp1H + 6;
    int grp2W = grp1W;
    MoveWindow(pData->hGrpNet, margin, grp2Y, grp2W, grp2H, TRUE);

    /* Group 3: full width, fixed height */
    int grp3Y = grp2Y + grp2H + 6;
    int grp3W = grp1W;
    MoveWindow(pData->hGrpProto, margin, grp3Y, grp3W, grp3H, TRUE);

    /* Group 4: AT command, full width, fixed height */
    int grp4Y = grp3Y + grp3H + 6;
    int grp4W = grp1W;
    MoveWindow(pData->hGrpAt, margin, grp4Y, grp4W, grp4H, TRUE);

    /* Group 5: Log, full width, stretch height */
    int grp5Y = grp4Y + grp4H + 6;
    int grp5W = grp1W;
    int grp5H = cy - grp5Y - margin;
    if (grp5H < 80) grp5H = 80;
    MoveWindow(pData->hGrpLog, margin, grp5Y, grp5W, grp5H, TRUE);

    /* Log edit fills group interior */
    int logX = margin + 10;
    int logY = grp5Y + 24;
    int logW = grp5W - 20;
    int logH = grp5H - 58;
    if (logW < 50) logW = 50;
    if (logH < 30) logH = 30;
    MoveWindow(pData->hLogEdit, logX, logY, logW, logH, TRUE);

    /* Clear button */
    MoveWindow(pData->hBtnClear,
               margin + grp5W - 100, grp5Y + grp5H - 32, 80, 26, TRUE);
}

static void lora_cfg_on_destroy(HWND hwnd, void *data)
{
    (void)hwnd;
    (void)data;
    /* No special cleanup — framework handles font deletion and free */
}

static LRESULT lora_cfg_on_message(HWND hwnd, void *data, UINT uMsg,
                                   WPARAM wParam, LPARAM lParam)
{
    TAB_LORA_CFG *pData = (TAB_LORA_CFG *)data;

    switch (uMsg) {

    /* ---- WM_LORA_DEVICE_FOUND ---- */
    case WM_LORA_DEVICE_FOUND: {
        LoraDeviceMsg *msg = (LoraDeviceMsg *)lParam;
        if (msg) {
            wchar_t wbuf[128];

            MultiByteToWideChar(CP_ACP, 0, msg->mac, -1, wbuf, 128);
            SetWindowTextW(pData->hMacText, wbuf);

            MultiByteToWideChar(CP_ACP, 0, msg->name, -1, wbuf, 128);
            SetWindowTextW(pData->hDevText, wbuf);

            MultiByteToWideChar(CP_ACP, 0, msg->sw, -1, wbuf, 128);
            SetWindowTextW(pData->hSwText, wbuf);

            char logLine[256];
            snprintf(logLine, sizeof(logLine),
                     "设备发现: MAC=%s, 设备=%s, SW=%s, IP=%s",
                     msg->mac, msg->name, msg->sw, msg->ip);
            AppendLog(pData, logLine);

            free(msg);
        }
        return TAB_MSG_HANDLED;
    }

    /* ---- WM_LORA_AT_RESPONSE ---- */
    case WM_LORA_AT_RESPONSE: {
        char *resp = (char *)lParam;
        if (resp) {
            AppendLog(pData, resp);
            ParseAtResponse(pData, resp);
            free(resp);
        }
        return TAB_MSG_HANDLED;
    }

    /* ---- WM_LORA_NET_PARAMS ---- */
    case WM_LORA_NET_PARAMS: {
        LoraNetParamsMsg *msg = (LoraNetParamsMsg *)lParam;
        if (msg) {
            wchar_t wbuf[128];

            MultiByteToWideChar(CP_ACP, 0, msg->ip, -1, wbuf, 128);
            SetWindowTextW(pData->hEditIp, wbuf);

            MultiByteToWideChar(CP_ACP, 0, msg->mask, -1, wbuf, 128);
            SetWindowTextW(pData->hEditMask, wbuf);

            MultiByteToWideChar(CP_ACP, 0, msg->gateway, -1, wbuf, 128);
            SetWindowTextW(pData->hEditGw, wbuf);

            char logLine[256];
            snprintf(logLine, sizeof(logLine),
                     "网络参数: IP=%s, 掩码=%s, 网关=%s",
                     msg->ip, msg->mask, msg->gateway);
            AppendLog(pData, logLine);

            free(msg);
        }
        return TAB_MSG_HANDLED;
    }

    /* ---- WM_LORA_LOG ---- */
    case WM_LORA_LOG: {
        char *text = (char *)lParam;
        if (text) {
            AppendLog(pData, text);
            free(text);
        }
        return TAB_MSG_HANDLED;
    }

    /* ---- Command handling ---- */
    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        /* ---- Group 0: Transport & serial ---- */
        case IDC_CFG_TRANSPORT_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE)
                UpdateTransportUI(pData);
            return TAB_MSG_HANDLED;

        case IDC_CFG_COMPORT_COMBO:
            /* User changed COM port — no action needed, just remember selection */
            return TAB_MSG_HANDLED;

        case IDC_CFG_BAUD_COMBO:
            /* User changed baud rate — no action needed, read when opening */
            return TAB_MSG_HANDLED;

        case IDC_CFG_SERIAL_OPEN_BTN: {
            if (!pData->sdk) return TAB_MSG_HANDLED;

            int serialOpen = lora_sdk_serial_is_open(pData->sdk);
            if (serialOpen) {
                /* Close */
                lora_sdk_serial_close(pData->sdk);
                AppendLog(pData, "串口已关闭");
                UpdateTransportUI(pData);
            } else {
                /* Open — read COM port and baud from combo */
                wchar_t wPort[32];
                GetWindowTextW(pData->hComboComPort, wPort, 32);
                char port[32];
                WideCharToMultiByte(CP_ACP, 0, wPort, -1, port, 32, NULL, NULL);

                int baudSel = (int)SendMessageW(pData->hComboBaud, CB_GETCURSEL, 0, 0);
                int baud = (int)SendMessageW(pData->hComboBaud, CB_GETITEMDATA, baudSel, 0);
                if (baud <= 0) baud = 115200;

                if (lora_sdk_serial_open(pData->sdk, port, baud) == 0) {
                    char log[128];
                    snprintf(log, sizeof(log), "串口 %s 已打开 (%d baud)", port, baud);
                    AppendLog(pData, log);
                }
                UpdateTransportUI(pData);
            }
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_SERIAL_REFRESH_BTN:
            EnumerateComPorts(pData->hComboComPort);
            AppendLog(pData, "COM 端口列表已刷新");
            return TAB_MSG_HANDLED;

        /* ---- Group 1: Device discovery ---- */
        case IDC_CFG_SEARCH_BTN:
            if (pData->sdk)
                lora_sdk_search_devices(pData->sdk);
            return TAB_MSG_HANDLED;

        case IDC_CFG_GETNET_BTN:
            if (pData->sdk)
                lora_sdk_get_net_params(pData->sdk);
            return TAB_MSG_HANDLED;

        case IDC_CFG_QUERY_GWID:
            SendAtCmd(pData, "AT+GWID?");
            return TAB_MSG_HANDLED;

        case IDC_CFG_QUERY_CSQ:
            SendAtCmd(pData, "AT+CSQ?");
            return TAB_MSG_HANDLED;

        /* ---- Group 2: Network settings ---- */
        case IDC_CFG_DHCP_QUERY:
            SendAtCmd(pData, "AT+DHCP?");
            return TAB_MSG_HANDLED;

        case IDC_CFG_DHCP_ON:
            SendAtCmd(pData, "AT+DHCP=ON");
            return TAB_MSG_HANDLED;

        case IDC_CFG_DHCP_OFF:
            SendAtCmd(pData, "AT+DHCP=OFF");
            return TAB_MSG_HANDLED;

        case IDC_CFG_OPTION_SET: {
            int sel = (int)SendMessageW(pData->hComboOption, CB_GETCURSEL, 0, 0);
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+OPTION=%d", sel);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_OPTION_QUERY:
            SendAtCmd(pData, "AT+OPTION?");
            return TAB_MSG_HANDLED;

        case IDC_CFG_IP_SET: {
            wchar_t wbuf[64];
            GetWindowTextW(pData->hEditIp, wbuf, 64);
            char abuf[64];
            WideCharToMultiByte(CP_ACP, 0, wbuf, -1, abuf, 64, NULL, NULL);
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "AT+GWIP=%s", abuf);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_IP_QUERY:
            SendAtCmd(pData, "AT+GWIP?");
            return TAB_MSG_HANDLED;

        case IDC_CFG_MASK_SET: {
            wchar_t wbuf[64];
            GetWindowTextW(pData->hEditMask, wbuf, 64);
            char abuf[64];
            WideCharToMultiByte(CP_ACP, 0, wbuf, -1, abuf, 64, NULL, NULL);
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "AT+MASK=%s", abuf);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_MASK_QUERY:
            SendAtCmd(pData, "AT+MASK?");
            return TAB_MSG_HANDLED;

        case IDC_CFG_GW_SET: {
            wchar_t wbuf[64];
            GetWindowTextW(pData->hEditGw, wbuf, 64);
            char abuf[64];
            WideCharToMultiByte(CP_ACP, 0, wbuf, -1, abuf, 64, NULL, NULL);
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "AT+GW=%s", abuf);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_GW_QUERY:
            SendAtCmd(pData, "AT+GW?");
            return TAB_MSG_HANDLED;

        /* ---- Group 3: LoRa protocol ---- */
        case IDC_CFG_NWMODE_SET: {
            int sel = (int)SendMessageW(pData->hComboNwmode, CB_GETCURSEL, 0, 0);
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+NWMODE=%d", sel);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_NWMODE_QUERY:
            SendAtCmd(pData, "AT+NWMODE?");
            return TAB_MSG_HANDLED;

        /* NWMODE combo change → update 工作模式 options */
        case IDC_CFG_NWMODE_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int isMesh = (int)SendMessageW(pData->hComboNwmode, CB_GETCURSEL, 0, 0);
                SendMessageW(pData->hComboTtmode, CB_RESETCONTENT, 0, 0);
                SendMessageW(pData->hComboTtmode, CB_ADDSTRING, 0, (LPARAM)L"广播透传");
                SendMessageW(pData->hComboTtmode, CB_ADDSTRING, 0, (LPARAM)L"指定节点");
                if (isMesh)
                    SendMessageW(pData->hComboTtmode, CB_ADDSTRING, 0, (LPARAM)L"主动上报");
                SendMessageW(pData->hComboTtmode, CB_SETCURSEL, 0, 0);
            }
            return TAB_MSG_HANDLED;

        case IDC_CFG_TTMODE_SET: {
            int isMesh = (int)SendMessageW(pData->hComboNwmode, CB_GETCURSEL, 0, 0);
            int sel = (int)SendMessageW(pData->hComboTtmode, CB_GETCURSEL, 0, 0);
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+%s=%d", isMesh ? "WMODE" : "TTMODE", sel);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_TTMODE_QUERY: {
            int isMesh = (int)SendMessageW(pData->hComboNwmode, CB_GETCURSEL, 0, 0);
            SendAtCmd(pData, isMesh ? "AT+WMODE?" : "AT+TTMODE?");
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_UPWID_QUERY:
            SendAtCmd(pData, "AT+UPWID?");
            return TAB_MSG_HANDLED;

        case IDC_CFG_UPWID_ON:
            SendAtCmd(pData, "AT+UPWID=ON");
            return TAB_MSG_HANDLED;

        case IDC_CFG_UPWID_OFF:
            SendAtCmd(pData, "AT+UPWID=OFF");
            return TAB_MSG_HANDLED;

        case IDC_CFG_CH_SET: {
            int ch_sel = (int)SendMessageW(pData->hComboCh, CB_GETCURSEL, 0, 0) + 1;
            int freq_sel = (int)SendMessageW(pData->hComboFreq, CB_GETCURSEL, 0, 0);
            int freq = 4100 + freq_sel * 100;
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+CH%d=%d", ch_sel, freq);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_CH_QUERY: {
            int ch_sel = (int)SendMessageW(pData->hComboCh, CB_GETCURSEL, 0, 0) + 1;
            char cmd[16];
            snprintf(cmd, sizeof(cmd), "AT+CH%d?", ch_sel);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_SPD_SET: {
            int sel = (int)SendMessageW(pData->hComboSpd, CB_GETCURSEL, 0, 0);
            int val = sel + 4;
            int ch = (int)SendMessageW(pData->hComboCh, CB_GETCURSEL, 0, 0) + 1;
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+SPD%d=%d", ch, val);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_SPD_QUERY: {
            int ch = (int)SendMessageW(pData->hComboCh, CB_GETCURSEL, 0, 0) + 1;
            char cmd[16];
            snprintf(cmd, sizeof(cmd), "AT+SPD%d?", ch);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_PWR_SET: {
            int sel = (int)SendMessageW(pData->hComboPwr, CB_GETCURSEL, 0, 0);
            int val = sel + 24;
            int ch = (int)SendMessageW(pData->hComboCh, CB_GETCURSEL, 0, 0) + 1;
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+PWR%d=%d", ch, val);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_PWR_QUERY: {
            int ch = (int)SendMessageW(pData->hComboCh, CB_GETCURSEL, 0, 0) + 1;
            char cmd[16];
            snprintf(cmd, sizeof(cmd), "AT+PWR%d?", ch);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        /* ---- Socket (SOCKA) ---- */
        case IDC_CFG_SOCKA_SET: {
            int modeSel = (int)SendMessageW(pData->hComboSockaMode, CB_GETCURSEL, 0, 0);
            const char *mode = modeSel == 0 ? "TCPC" : "TCPS";
            wchar_t wip[64], wrp[16], wlp[16];
            GetWindowTextW(pData->hEditSockaIp, wip, 64);
            GetWindowTextW(pData->hEditSockaRPort, wrp, 16);
            GetWindowTextW(pData->hEditSockaLPort, wlp, 16);
            char ipA[64], rpA[16], lpA[16];
            WideCharToMultiByte(CP_ACP, 0, wip, -1, ipA, 64, NULL, NULL);
            WideCharToMultiByte(CP_ACP, 0, wrp, -1, rpA, 16, NULL, NULL);
            WideCharToMultiByte(CP_ACP, 0, wlp, -1, lpA, 16, NULL, NULL);
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "AT+SOCKA=%s,%s,%s,%s", mode, ipA, rpA, lpA);
            SendAtCmd(pData, cmd);
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_SOCKA_QUERY:
            SendAtCmd(pData, "AT+SOCKA?");
            return TAB_MSG_HANDLED;

        /* ---- Group 4: AT command ---- */
        case IDC_CFG_SEND_BTN: {
            wchar_t wbuf[256];
            GetWindowTextW(pData->hEditAtCmd, wbuf, 256);
            if (wcslen(wbuf) > 0) {
                char abuf[256];
                WideCharToMultiByte(CP_ACP, 0, wbuf, -1, abuf, 256, NULL, NULL);
                SendAtCmd(pData, abuf);
            }
            return TAB_MSG_HANDLED;
        }

        case IDC_CFG_QUERY_VER:
            SendAtCmd(pData, "AT+VER?");
            return TAB_MSG_HANDLED;

        case IDC_CFG_REBOOT:
            if (pData->sdk)
                lora_sdk_reboot(pData->sdk);
            return TAB_MSG_HANDLED;

        /* ---- Group 5: Log ---- */
        case IDC_CFG_CLEAR_BTN:
            SetWindowTextW(pData->hLogEdit, L"");
            return TAB_MSG_HANDLED;

        default:
            break;
        }
        break;

    default:
        break;
    }

    return TAB_MSG_NOT_HANDLED;
}

/* ------------------------------------------------------------------ */
/*  Vtable + Public API                                               */
/* ------------------------------------------------------------------ */

static const TAB_IFACE g_lora_cfg_iface = {
    .data_size  = sizeof(TAB_LORA_CFG),
    .on_create  = lora_cfg_on_create,
    .on_size    = lora_cfg_on_size,
    .on_destroy = lora_cfg_on_destroy,
    .on_message = lora_cfg_on_message,
};

HWND TabLoraCfg_Create(HWND hParent, HINSTANCE hInst, lora_sdk_t *sdk)
{
    return TabBase_CreatePage(hParent, hInst, &g_lora_cfg_iface, sdk);
}
