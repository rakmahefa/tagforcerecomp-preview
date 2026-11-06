/* commdlg.h - Win32 compatibility shim (Linux): the ISO picker. GetOpenFileNameW uses a zenity
 * dialog when there is a display, and falls back to a terminal prompt otherwise (headless/CI). */
#pragma once
#include "windows.h"

typedef struct tagOFNW {
    DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance;
    LPCWSTR lpstrFilter, lpstrCustomFilter; DWORD nMaxCustFilter, nFilterIndex;
    LPWSTR lpstrFile; DWORD nMaxFile; LPWSTR lpstrFileTitle; DWORD nMaxFileTitle;
    LPCWSTR lpstrInitialDir, lpstrTitle; DWORD Flags;
    WORD nFileOffset, nFileExtension; LPCWSTR lpstrDefExt;
} OPENFILENAMEW, *LPOPENFILENAMEW;
#define OFN_FILEMUSTEXIST 0x1000
#define OFN_PATHMUSTEXIST 0x800
#define OFN_HIDEREADONLY 0x4
#define OFN_NOCHANGEDIR 0x8
BOOL GetOpenFileNameW(LPOPENFILENAMEW of);
