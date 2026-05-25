/**
 * Tab Base Framework - unified WndProc + vtable dispatch
 */
#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include "resource.h"
#include "tab_base.h"

/* Internal: parameter block passed via lpCreateParams */
typedef struct {
    const TAB_IFACE *iface;
    void            *user_params;
} TAB_CREATE_PARAMS;

/* Internal: disable visual themes on GroupBox children */
static void TabBase_ThemeGroupBoxes(HWND hwnd)
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

/* Unified WndProc */
static const wchar_t *TAB_BASE_CLASS = L"TabBaseClass";
static int g_tabBaseClassRegistered = 0;

static LRESULT CALLBACK TabBase_WndProc(HWND hwnd, UINT uMsg,
                                         WPARAM wParam, LPARAM lParam)
{
    TAB_BASE *base = (TAB_BASE *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (uMsg) {

    case WM_NCCREATE: {
        TAB_CREATE_PARAMS *params = (TAB_CREATE_PARAMS *)
            ((CREATESTRUCTW *)lParam)->lpCreateParams;
        base = (TAB_BASE *)calloc(1, params->iface->data_size);
        if (!base) return FALSE;
        base->iface       = params->iface;
        base->user_params = params->user_params;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)base);
        return TRUE;
    }

    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;

        /* Create standard fonts */
        base->hFont = CreateFontW(
            FONT_SIZE_UI, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            FONT_FACE_UI);
        base->hFontBold = CreateFontW(
            FONT_SIZE_UI, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            FONT_FACE_UI);
        base->hFontMono = CreateFontW(
            FONT_SIZE_MONO, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
            FONT_FACE_MONO);

        /* Replace lpCreateParams with user_params for transparent on_create */
        cs->lpCreateParams = base->user_params;

        if (base->iface->on_create)
            base->iface->on_create(hwnd, (void *)base, cs);

        /* Disable GroupBox visual themes for blue title support */
        TabBase_ThemeGroupBoxes(hwnd);

        return 0;
    }

    case WM_SIZE: {
        int cx = LOWORD(lParam);
        int cy = HIWORD(lParam);
        if (base && base->iface->on_size && cx >= 100 && cy >= 100)
            base->iface->on_size(hwnd, (void *)base, cx, cy);
        return 0;
    }

    case WM_DESTROY:
        if (base) {
            if (base->iface->on_destroy)
                base->iface->on_destroy(hwnd, (void *)base);
            if (base->hFont)     DeleteObject(base->hFont);
            if (base->hFontBold) DeleteObject(base->hFontBold);
            if (base->hFontMono) DeleteObject(base->hFontMono);
            free(base);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }

    /* GroupBox blue title - non-GroupBox falls through */
    if (uMsg == WM_CTLCOLORSTATIC) {
        HWND ctl = (HWND)lParam;
        if ((GetWindowLongPtrW(ctl, GWL_STYLE) & 0xF) == BS_GROUPBOX) {
            SetTextColor((HDC)wParam, RGB(0, 80, 180));
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
    }

    /* Tab custom message dispatch */
    if (base && base->iface->on_message) {
        LRESULT r = base->iface->on_message(hwnd, (void *)base, uMsg,
                                             wParam, lParam);
        if (r == TAB_MSG_HANDLED)
            return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* Public API */
HWND TabBase_CreatePage(HWND hTabCtrl, HINSTANCE hInst,
                        const TAB_IFACE *iface, void *create_params)
{
    if (!g_tabBaseClassRegistered) {
        WNDCLASSEXW wc = { 0 };
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = TabBase_WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = TAB_BASE_CLASS;
        RegisterClassExW(&wc);
        g_tabBaseClassRegistered = 1;
    }

    TAB_CREATE_PARAMS params;
    params.iface       = iface;
    params.user_params = create_params;

    RECT rc;
    GetClientRect(hTabCtrl, &rc);
    TabCtrl_AdjustRect(hTabCtrl, FALSE, &rc);

    return CreateWindowExW(0, TAB_BASE_CLASS, L"",
        WS_CHILD | WS_CLIPCHILDREN,
        rc.left, rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        hTabCtrl, NULL, hInst, &params);
}
