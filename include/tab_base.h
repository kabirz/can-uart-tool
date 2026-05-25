#ifndef TAB_BASE_H
#define TAB_BASE_H

#include <windows.h>

/* Forward declaration */
struct TAG_TAB_IFACE;

/* Each Tab's data struct MUST have this as its first member.
 * C99 guarantees struct pointer is interchangeable with first member pointer. */
typedef struct {
    HFONT                    hFont;
    HFONT                    hFontBold;
    HFONT                    hFontMono;
    const struct TAG_TAB_IFACE *iface;
    void                    *user_params;   /* valid only during on_create */
} TAB_BASE;

/* on_message return value semantics */
#define TAB_MSG_HANDLED      ((LRESULT)1)   /* handled, framework returns 0 */
#define TAB_MSG_NOT_HANDLED  ((LRESULT)0)   /* not handled, call DefWindowProcW */

typedef struct TAG_TAB_IFACE {
    size_t   data_size;    /* total size of Tab data struct (including TAB_BASE) */

    /* WM_CREATE: framework has created fonts and replaced cs->lpCreateParams with user_params.
     * user_params is only valid during this call (stack-allocated). */
    void     (*on_create)(HWND hwnd, void *data, CREATESTRUCTW *cs);

    /* WM_SIZE: framework has already filtered cx < 100 || cy < 100 */
    void     (*on_size)(HWND hwnd, void *data, int cx, int cy);

    /* WM_DESTROY: framework calls this BEFORE DeleteObject fonts + free data.
     * base->hFont etc. are still valid here. */
    void     (*on_destroy)(HWND hwnd, void *data);

    /* Non-lifecycle message handling.
     * Return TAB_MSG_HANDLED if processed, TAB_MSG_NOT_HANDLED for DefWindowProcW. */
    LRESULT  (*on_message)(HWND hwnd, void *data, UINT uMsg,
                           WPARAM wp, LPARAM lp);
} TAB_IFACE;

/* Register shared window class (once) + create Tab page window */
HWND TabBase_CreatePage(HWND hTabCtrl, HINSTANCE hInst,
                        const TAB_IFACE *iface, void *create_params);

#endif /* TAB_BASE_H */
