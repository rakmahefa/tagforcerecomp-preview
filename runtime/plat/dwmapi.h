/* dwmapi.h - Win32 compatibility shim (Linux): the dark title bar calls become no-ops (the
 * SDL window has no DWM; the runtime's own drawing is already dark). */
#pragma once
#include "windows.h"
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define DWMWA_CAPTION_COLOR 35
#define DWMWA_TEXT_COLOR 36
#define DWMWA_BORDER_COLOR 34
HRESULT DwmSetWindowAttribute(HWND, DWORD, const void *, DWORD);
HRESULT DwmGetCompositionTimingInfo(void *, void *);
HRESULT DwmFlush(void);
