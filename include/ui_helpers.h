#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Control creation helpers                                          */
/*  Each creates a child control, sets the font, and returns the HWND */
/* ------------------------------------------------------------------ */

HWND Ui_CreateLabel(HWND hParent, HINSTANCE hInst, int id,
                    int x, int y, int w, int h,
                    const wchar_t *text, HFONT hFont);

HWND Ui_CreateBtn(HWND hParent, HINSTANCE hInst, int id,
                  int x, int y, int w, int h,
                  const wchar_t *text, HFONT hFont);

HWND Ui_CreateCombo(HWND hParent, HINSTANCE hInst, int id,
                    int x, int y, int w, int h,
                    HFONT hFont);

HWND Ui_CreateEdit(HWND hParent, HINSTANCE hInst, int id,
                   int x, int y, int w, int h,
                   const wchar_t *text, HFONT hFont, DWORD extraStyle);

HWND Ui_CreateStaticText(HWND hParent, HINSTANCE hInst, int id,
                         int x, int y, int w, int h,
                         const wchar_t *text, HFONT hFont);

/* ------------------------------------------------------------------ */
/*  GroupBox theme helper                                             */
/*  Disables visual themes on all groupbox children of hwnd, so that  */
/*  WM_CTLCOLORSTATIC can paint the title in a custom color.          */
/* ------------------------------------------------------------------ */
void Ui_DisableGroupBoxTheme(HWND hwnd);

/* ------------------------------------------------------------------ */
/*  Log append helper                                                 */
/*  Appends a timestamped UTF-8 line to an edit control.              */
/*  Automatically trims when text exceeds maxLen.                     */
/* ------------------------------------------------------------------ */
void Ui_AppendLog(HWND hLog, const char *utf8Text);

/* ------------------------------------------------------------------ */
/*  Byte-order helpers (big-endian wire format)                       */
/* ------------------------------------------------------------------ */
void Ui_PutBE16(uint16_t val, uint8_t *buf);
void Ui_PutBE32(uint32_t val, uint8_t *buf);
uint16_t Ui_GetBE16(const uint8_t *buf);
uint32_t Ui_GetBE32(const uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* UI_HELPERS_H */
