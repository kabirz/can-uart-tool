#include "ui_helpers.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Control creation helpers                                          */
/* ------------------------------------------------------------------ */

HWND Ui_CreateLabel(HWND hParent, HINSTANCE hInst, int id,
                    int x, int y, int w, int h,
                    const wchar_t *text, HFONT hFont)
{
    HWND hw = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    if (hFont) SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

HWND Ui_CreateBtn(HWND hParent, HINSTANCE hInst, int id,
                  int x, int y, int w, int h,
                  const wchar_t *text, HFONT hFont)
{
    HWND hw = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, w, h, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    if (hFont) SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

HWND Ui_CreateCombo(HWND hParent, HINSTANCE hInst, int id,
                    int x, int y, int w, int h,
                    HFONT hFont)
{
    HWND hw = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, y, w, h, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    if (hFont) SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

HWND Ui_CreateEdit(HWND hParent, HINSTANCE hInst, int id,
                   int x, int y, int w, int h,
                   const wchar_t *text, HFONT hFont, DWORD extraStyle)
{
    (void)h;
    HWND hw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extraStyle,
        x, y, w, 26, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    if (hFont) SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

HWND Ui_CreateStaticText(HWND hParent, HINSTANCE hInst, int id,
                         int x, int y, int w, int h,
                         const wchar_t *text, HFONT hFont)
{
    (void)h;
    HWND hw = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_SUNKEN,
        x, y, w, 26, hParent, (HMENU)(INT_PTR)id, hInst, NULL);
    if (hFont) SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hw;
}

/* ------------------------------------------------------------------ */
/*  GroupBox theme helper                                             */
/* ------------------------------------------------------------------ */

void Ui_DisableGroupBoxTheme(HWND hwnd)
{
    typedef HRESULT (WINAPI *PFN_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
    HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");
    if (!hUx) return;

    PFN_SetWindowTheme pFn = (PFN_SetWindowTheme)GetProcAddress(hUx, "SetWindowTheme");
    if (!pFn) return;

    HWND child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        if ((GetWindowLongPtrW(child, GWL_STYLE) & 0xF) == BS_GROUPBOX)
            pFn(child, L"", L"");
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

/* ------------------------------------------------------------------ */
/*  Log append helper                                                 */
/* ------------------------------------------------------------------ */

void Ui_AppendLog(HWND hLog, const char *utf8Text)
{
    if (!hLog || !utf8Text) return;

    /* Convert UTF-8 to wide string */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Text, -1, NULL, 0);
    if (wlen <= 0) return;
    wchar_t *wstr = (wchar_t *)malloc(wlen * sizeof(wchar_t));
    if (!wstr) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8Text, -1, wstr, wlen);

    /* Build timestamp */
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timestamp[32];
    _snwprintf(timestamp, 32, L"[%02d:%02d:%02d] ",
               st.wHour, st.wMinute, st.wSecond);

    /* Truncate if too long */
    int curLen = GetWindowTextLengthW(hLog);
    if (curLen > 100000) {
        SendMessageW(hLog, EM_SETSEL, 0, curLen / 4);
        SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"");
        curLen = GetWindowTextLengthW(hLog);
    }

    /* Append timestamp + text + newline (skip trailing newline if text
     * already ends with one, e.g. raw AT responses carrying \r\n) */
    SendMessageW(hLog, EM_SETSEL, curLen, curLen);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)timestamp);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)wstr);
    int textChars = (int)wcslen(wstr);
    if (textChars == 0 ||
        (wstr[textChars - 1] != L'\n' && wstr[textChars - 1] != L'\r'))
        SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(hLog, EM_SCROLLCARET, 0, 0);

    free(wstr);
}

/* ------------------------------------------------------------------ */
/*  Byte-order helpers                                                */
/* ------------------------------------------------------------------ */

void Ui_PutBE16(uint16_t val, uint8_t *buf)
{
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
}

void Ui_PutBE32(uint32_t val, uint8_t *buf)
{
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val & 0xFF);
}

uint16_t Ui_GetBE16(const uint8_t *buf)
{
    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

uint32_t Ui_GetBE32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
}
