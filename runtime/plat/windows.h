/* windows.h - Win32 compatibility layer for the Linux build of the Tag Force Recomp runtime.
 *
 * The runtime (runtime/) is written against Win32: windowing (Win32 + GDI), XInput/DirectInput,
 * WASAPI/waveOut, TinyCC via libtcc.dll, files with wide paths. This header, plus the shim
 * implementation files next to it (shim_*.cpp), provide that API surface on Linux on top of
 * POSIX + SDL2 (+ X11 through SDL) so the game code itself stays single-source.
 *
 * Differences that matter:
 *  - wchar_t is 4 bytes (UTF-32) on Linux; every W function converts to UTF-8 internally.
 *  - The "window manager" is SDL2: one SDL window per top-level HWND, an ARGB32 backing store
 *    per window, a Win32-style message queue per thread, and a software rasterizer for GDI.
 *  - gdiplus.h, dinput.h, commdlg.h, dwmapi.h, mmsystem.h, shellapi.h, winhttp.h, direct.h,
 *    objidl.h, winioctl.h and io.h next to this file include it back.
 */
#ifndef TF_WINDOWS_SHIM_H
#define TF_WINDOWS_SHIM_H

#ifndef __linux__
#error "This windows.h is the Linux compatibility shim; build the runtime with tools/build.sh on Windows."
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*draw item (ownerdraw buttons)*/
#define ODT_BUTTON 4
#define ODT_STATIC 5
#define ODA_DRAWENTIRE 1
#define ODA_SELECT 2
#define ODA_FOCUS 4
#define ODS_SELECTED 1
#define ODS_GRAYED 2
#define ODS_DISABLED 4
#define ODS_CHECKED 8
#define ODS_FOCUS 16
/*button notifications*/
#define BN_CLICKED 0
#define BN_DOUBLECLICKED 5
/*icons*/
#define ICON_SMALL 0
#define ICON_BIG 1
#define DI_NORMAL 3
/*stretch mode*/
#define BLACKONWHITE 1
#define WHITEONBLACK 2
#define COLORONCOLOR 3
#define HALFTONE 4
/*device caps*/
#define LOGPIXELSX 88
#define LOGPIXELSY 90
#define HORZRES 8
#define VERTRES 10
#define BITSPIXEL 12
#define PLANES 14
/*message beep*/
#define MB_OK_BEEP 0

/* ------------------------------------------------------------------ base types */
typedef int BOOL; typedef unsigned char BYTE; typedef unsigned short WORD;
typedef unsigned long DWORD; typedef unsigned int UINT; typedef long LONG; typedef unsigned long ULONG;
typedef short SHORT; typedef unsigned short USHORT; typedef unsigned char UCHAR;
typedef int INT; typedef unsigned int UINT_PTR; typedef long LONG_PTR; typedef unsigned long ULONG_PTR;
typedef void VOID; typedef void *LPVOID; typedef const void *LPCVOID;
typedef char CHAR, *LPSTR; typedef const char *LPCSTR; typedef wchar_t WCHAR, *LPWSTR; typedef const wchar_t *LPCWSTR;
typedef BYTE *LPBYTE, *PBYTE; typedef DWORD *LPDWORD, *PDWORD; typedef WORD *LPWORD, *PWORD;
typedef BOOL *LPBOOL, *PBOOL; typedef LONG *LPLONG; typedef ULONG *PULONG; typedef UINT *PUINT;
typedef long INT_PTR;
typedef size_t SIZE_T; typedef ptrdiff_t SSIZE_T;
typedef DWORD COLORREF;
typedef LONG HRESULT; typedef DWORD DWORD_PTR;
typedef unsigned long long ULONGLONG, DWORD64;
typedef long long LONGLONG;
#define WINAPI
#define CALLBACK
#define APIENTRY
#define PASCAL
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INFINITE 0xFFFFFFFFu

/*HANDLE family: opaque pointers (never dereferenced by runtime code)*/
typedef struct tf_handle_s *HANDLE;
typedef struct tf_hwnd_s *HWND;
typedef struct tf_hdc_s *HDC;
/*GDI objects share one base (like the real Win32 handles: HGDIOBJ of every kind)*/
typedef struct tf_hgdi_s *HGDIOBJ, *HBRUSH, *HPEN, *HFONT, *HBITMAP, *HPALETTE;
typedef struct tf_hmenu_s *HMENU;
typedef struct tf_hicon_s *HICON, *HCURSOR;
typedef struct tf_hinst_s *HINSTANCE, *HMODULE;
typedef struct tf_hglobal_s *HGLOBAL;
typedef struct tf_hmonitor_s *HMONITOR;
typedef HANDLE HKEY, HRGN, HACCEL, HHOOK, HIMAGELIST, HINTERNET, HWAVEOUT, HWAVEIN, HMIDI, HWAITABLETIMER;
typedef HANDLE *PHANDLE;
typedef void *FARPROC, *NEARPROC, *PROC;
typedef uintptr_t WPARAM; typedef intptr_t LPARAM; typedef intptr_t LRESULT;
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *TIMERPROC)(HWND, UINT, WPARAM, LPARAM);
typedef DWORD (CALLBACK *LPTHREAD_START_ROUTINE)(LPVOID);
typedef WORD ATOM;

typedef union _LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;
typedef union _ULARARGE_INTEGER { struct { DWORD LowPart; DWORD HighPart; } u; ULONGLONG QuadPart; } ULARGE_INTEGER, *PULARARGE_INTEGER;

/* ------------------------------------------------------------------ structs */
typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT;
typedef struct tagSIZE { LONG cx, cy; } SIZE, *LPSIZE;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef const RECT *LPCRECT;
typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG, *LPMSG;
typedef struct tagPAINTSTRUCT { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore; BOOL fIncUpdate; BYTE rgbReserved[32]; } PAINTSTRUCT;
typedef struct _SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME, *LPSYSTEMTIME;
typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *LPFILETIME;

typedef struct tagWNDCLASSA {
    UINT style; WNDPROC lpfnWndProc; int cbClsExtra; int cbWndExtra; HINSTANCE hInstance; HICON hIcon;
    HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName; LPCSTR lpszClassName;
} WNDCLASSA, *PWNDCLASSA;
typedef struct tagWNDCLASSW {
    UINT style; WNDPROC lpfnWndProc; int cbClsExtra; int cbWndExtra; HINSTANCE hInstance; HICON hIcon;
    HCURSOR hCursor; HBRUSH hbrBackground; LPCWSTR lpszMenuName; LPCWSTR lpszClassName;
} WNDCLASSW, *PWNDCLASSW;

typedef struct tagMONITORINFO { DWORD cbSize; RECT rcMonitor; RECT rcWork; DWORD dwFlags; } MONITORINFO, *LPMONITORINFO;
#define MONITORINFOEXW_COMPAT 1 /* MONITORINFOEXW: MONITORINFO + device name, passed to GetMonitorInfoW by cbSize (see below) */
typedef struct tagWINDOWPLACEMENT { UINT length, flags; UINT showCmd; POINT ptMinPosition, ptMaxPosition; RECT rcNormalPosition; } WINDOWPLACEMENT;
typedef struct _TRACKMOUSEEVENT { UINT cbSize, dwFlags; HWND hwndTrack; DWORD dwHoverTime; } TRACKMOUSEEVENT, *LPTRACKMOUSEEVENT;

typedef struct tagTEXTMETRICW {
    LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth, tmWeight;
    LONG tmOverhang; LONG tmDigitizedAspectX, tmDigitizedAspectY; WCHAR tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily; BYTE tmCharSet;
} TEXTMETRICW, *LPTEXTMETRICW;
typedef struct _GLYPHMETRICS { UINT gmBlackBoxX, gmBlackBoxY; POINT gmptGlyphOrigin; short gmCellIncX, gmCellIncY; } GLYPHMETRICS, *LPGLYPHMETRICS;
/*the runtime builds MAT2 = {{0,1},{0,0},{0,0},{0,1}} (identity): fixed-point 2x2 as WORD pairs*/
typedef struct _MAT2 { WORD eM11[2], eM12[2], eM21[2], eM22[2]; } MAT2, *LPMAT2;
typedef struct tagLOGFONTW {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight; BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet;
    BYTE lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily; WCHAR lfFaceName[32];
} LOGFONTW, *LPLOGFONTW;
typedef struct tagBITMAPINFOHEADER {
    DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
typedef struct tagDIBSECTION { BITMAPINFOHEADER dsBmih; DWORD dsBitfields[3]; HANDLE dshSection; DWORD dsOffset; } DIBSECTION;

typedef struct tagDRAWITEMSTRUCT {
    UINT CtlType, CtlID; UINT itemID; UINT itemAction, itemState; HWND hwndItem; HDC hDC; RECT rcItem;
    ULONG_PTR itemData;
} DRAWITEMSTRUCT, *PDRAWITEMSTRUCT, *LPDRAWITEMSTRUCT;
typedef struct tagMEASUREITEMSTRUCT { UINT CtlType, CtlID, itemID, itemWidth, itemHeight; ULONG_PTR itemData; } MEASUREITEMSTRUCT;
typedef struct tagCREATESTRUCTW { LPVOID lpCreateParams; HINSTANCE hInstance; HMENU hMenu; HWND hwndParent; int cy, cx, y, x; LONG style; LPCWSTR lpszName, lpszClass; DWORD dwExStyle; } CREATESTRUCTW, *LPCREATESTRUCTW;

/*XInput (shim: SDL2 game controllers mapped onto the XInput layout)*/
typedef struct _XINPUT_GAMEPAD {
    WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;
typedef struct _XINPUT_STATE { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
#define XINPUT_GAMEPAD_DPAD_UP          0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN        0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT        0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT       0x0008
#define XINPUT_GAMEPAD_START            0x0010
#define XINPUT_GAMEPAD_BACK             0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB       0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB      0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER    0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER   0x0200
#define XINPUT_GAMEPAD_A                0x1000
#define XINPUT_GAMEPAD_B                0x2000
#define XINPUT_GAMEPAD_X                0x4000
#define XINPUT_GAMEPAD_Y                0x8000
#define ERROR_SUCCESS 0
#define ERROR_DEVICE_NOT_CONNECTED 1167
typedef struct _XINPUT_CAPABILITIES { BYTE Type, SubType, Flags; XINPUT_GAMEPAD Gamepad; WORD wReserved; } XINPUT_CAPABILITIES;

/* ------------------------------------------------------------------ constants */
/*window styles*/
#define WS_OVERLAPPED    0x00000000L
#define WS_POPUP         0x80000000L
#define WS_CHILD         0x40000000L
#define WS_MINIMIZE      0x20000000L
#define WS_VISIBLE       0x10000000L
#define WS_DISABLED      0x08000000L
#define WS_CLIPSIBLINGS  0x04000000L
#define WS_CLIPCHILDREN  0x02000000L
#define WS_MAXIMIZE      0x01000000L
#define WS_CAPTION       0x00C00000L
#define WS_BORDER        0x00800000L
#define WS_DLGFRAME      0x00400000L
#define WS_VSCROLL       0x00200000L
#define WS_HSCROLL       0x00100000L
#define WS_SYSMENU       0x00080000L
#define WS_THICKFRAME    0x00040000L
#define WS_MINIMIZEBOX   0x00020000L
#define WS_MAXIMIZEBOX   0x00010000L
#define WS_TABSTOP       0x00010000L
#define WS_GROUP         0x00020000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_DLGMODALFRAME  0x00000001L
#define WS_EX_NOPARENTNOTIFY 0x00000004L
#define WS_EX_TOPMOST        0x00000008L
#define WS_EX_TOOLWINDOW     0x00000080L
#define WS_EX_CLIENTEDGE     0x00000200L
#define WS_EX_NOACTIVATE     0x08000000L
/*button/static/edit styles*/
#define BS_PUSHBUTTON   0x00000000L
#define BS_OWNERDRAW    0x0000000BL
#define BS_DEFPUSHBUTTON 0x00000001L
#define SS_LEFT         0x00000000L
#define SS_CENTER       0x00000001L
#define SS_RIGHT        0x00000002L
#define SS_ENDELLIPSIS  0x00004000L
#define ES_LEFT         0x00000000L
#define ES_MULTILINE    0x00000004L
#define ES_AUTOVSCROLL  0x0040L
#define ES_AUTOHSCROLL  0x0080L
#define ES_NUMBER       0x2000L
#define ES_READONLY     0x0800L
#define ES_WANTRETURN   0x1000L
/*show window*/
#define SW_HIDE             0
#define SW_SHOWNORMAL       1
#define SW_SHOWMAXIMIZED    3
#define SW_SHOW             5
#define SW_MINIMIZE         6
#define SW_RESTORE          9
/*set window pos*/
#define SWP_NOSIZE          0x0001
#define SWP_NOMOVE          0x0002
#define SWP_NOZORDER        0x0004
#define SWP_NOOWNERZORDER   0x0200
#define SWP_FRAMECHANGED    0x0020
#define SWP_SHOWWINDOW      0x0040
#define SWP_HIDEWINDOW      0x0080
#define SWP_NOACTIVATE      0x0010
/*get/set window long*/
#define GWL_STYLE       (-16)
#define GWL_EXSTYLE     (-20)
#define GWLP_WNDPROC    (-4)
#define GWLP_USERDATA   (-21)
/*hit test*/
#define HTCLIENT 1
#define HTCAPTION 2
/*virtual keys (the set the runtime uses; 'A'..'Z','0'..'9' are their ASCII codes, as on Windows)*/
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F24 0x87
#define VK_NUMLOCK 0x90
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_OEM_1 0xBA   /* ;: */
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF   /* /? */
#define VK_OEM_3 0xC0   /* `~ */
#define VK_OEM_4 0xDB   /* [{ */
#define VK_OEM_5 0xDC   /* \| */
#define VK_OEM_6 0xDD   /* ]} */
#define VK_OEM_7 0xDE   /* '" */
/*messages*/
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETREDRAW 0x000B
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_ENTERSIZEMOVE 0x0231
#define WM_EXITSIZEMOVE 0x0232
#define WM_CAPTURECHANGED 0x0215
#define WM_NCCREATE 0x0081
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCACTIVATE 0x0086
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_DEADCHAR 0x0103
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_MOUSEFIRST 0x0200
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020A
#define WM_MOUSEHWHEEL 0x020E
#define WM_MOUSELEAVE 0x02A3
#define WM_CUT 0x0300
#define WM_COPY 0x0301
#define WM_PASTE 0x0302
#define WM_CLEAR 0x0303
#define WM_UNDO 0x0304
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_PRINTCLIENT 0x0318
#define WM_ENTERMENULOOP 0x0211
#define WM_EXITMENULOOP 0x0212
#define WM_NEXTDLGCTL 0x0028
#define WM_TIMER 0x0113
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_NOTIFY 0x004E
#define WM_CONTEXTMENU 0x007B
#define WM_STYLECHANGED 0x007D
#define WM_SETICON 0x0080
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_CTLCOLOREDIT 0x0133
#define WM_CTLCOLORSTATIC 0x0138
#define WM_CTLCOLORBTN 0x0135
#define WM_CTLCOLORLISTBOX 0x0134
#define WM_DRAWITEM 0x002B
#define WM_MEASUREITEM 0x002C
#define WM_PARENTNOTIFY 0x0210
#define WM_APP 0x8000
#define WM_USER 0x0400
/*edit messages*/
#define EM_SETSEL 0x00B1
#define EM_REPLACESEL 0x00C2
#define EM_LIMITTEXT 0x00C5
#define EM_GETLIMITTEXT 0x00D4
/*window sizing / metrics*/
#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define MONITOR_DEFAULTTOPRIMARY 1
#define MONITOR_DEFAULTTONEAREST 2
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
/*activation*/
#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2
/*draw text*/
#define DT_TOP 0x000
#define DT_LEFT 0x000
#define DT_CENTER 0x001
#define DT_RIGHT 0x002
#define DT_VCENTER 0x004
#define DT_BOTTOM 0x008
#define DT_WORDBREAK 0x0010
#define DT_SINGLELINE 0x0020
#define DT_EXPANDTABS 0x0040
#define DT_NOPREFIX 0x0080
#define DT_END_ELLIPSIS 0x8000
#define DT_PATH_ELLIPSIS 0x4000
#define DT_CALCRECT 0x0400
#define DT_NOCLIP 0x0100
/*message box*/
#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_ABORTRETRYIGNORE 0x00000002L
#define MB_YESNOCANCEL 0x00000003L
#define MB_YESNO 0x00000004L
#define MB_RETRYCANCEL 0x00000005L
#define MB_ICONHAND 0x00000010L
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONASTERISK 0x00000040L
#define MB_ICONWARNING MB_ICONEXCLAMATION
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONINFORMATION MB_ICONASTERISK
#define MB_DEFBUTTON2 0x00000100L
#define MB_DEFBUTTON3 0x00000200L
#define MB_SETFOREGROUND 0x00010000L
#define MB_TYPEMASK 0x0000000FL
#define MB_ICONMASK 0x000000F0L
#define MB_DEFMASK 0x00000F00L
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#define IDCLOSE 8
#define IDHELP 9
/*virtual memory*/
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READWRITE 0x40
/*file creation/disposition*/
#define GENERIC_READ 0x80000000L
#define GENERIC_WRITE 0x40000000L
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
/*wait*/
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_ABANDONED 0x80
#define WAIT_FAILED ((DWORD)-1)
#define MAXIMUM_WAIT_OBJECTS 64
/*thread priority*/
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_MODE_BACKGROUND_BEGIN 0x10000000
#define THREAD_MODE_BACKGROUND_END 0x20000000
/*GDI*/
#define SRCCOPY 0x00CC0020
#define DIB_RGB_COLORS 0
#define BI_RGB 0
#define WHITE_BRUSH 0
#define LTGRAY_BRUSH 1
#define GRAY_BRUSH 2
#define DKGRAY_BRUSH 3
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define WHITE_PEN 6
#define BLACK_PEN 7
#define NULL_PEN 8
#define OEM_FIXED_FONT 10
#define ANSI_FIXED_FONT 11
#define ANSI_VAR_FONT 12
#define SYSTEM_FONT 13
#define DEFAULT_GUI_FONT 17
#define PS_SOLID 0
#define PS_NULL 5
#define PS_DOT 2
#define PS_DASH 1
#define FW_DONTCARE 0
#define FW_THIN 100
#define FW_NORMAL 400
#define FW_MEDIUM 500
#define FW_SEMIBOLD 600
#define FW_BOLD 700
#define FW_HEAVY 900
#define FF_DONTCARE 0
#define DEFAULT_CHARSET 1
#define ANSI_CHARSET 0
#define SHIFTJIS_CHARSET 128
#define DEFAULT_PITCH 0
#define FIXED_PITCH 1
#define VARIABLE_PITCH 2
#define OUT_DEFAULT_PRECIS 0
#define OUT_TT_PRECIS 4
#define OUT_DEVICE_PRECIS 3
#define CLIP_DEFAULT_PRECIS 0
#define ANTIALIASED_QUALITY 4
#define CLEARTYPE_QUALITY 5
#define DEFAULT_QUALITY 0
#define GDI_ERROR 0xFFFFFFFFL
#define TRANSPARENT 1
#define OPAQUE 2
#define R2_COPYPEN 13
#define RDW_INVALIDATE 0x0001
#define RDW_ERASE 0x0004
#define RDW_UPDATENOW 0x0100
#define RDW_ERASENOW 0x0200
#define GGO_METRICS 0
#define GGO_BITMAP 1
#define GGO_NATIVE 2
#define GGO_GRAY8_BITMAP 5
#define GGO_GLYPH_INDEX 0x0010
#define MAPVK_VK_TO_VSC 0
#define MAPVK_VSC_TO_VK 1
#define MAPVK_VK_TO_CHAR 2
/*cursor / icon*/
#define IDC_ARROW MAKEINTRESOURCEA(32512)
#define IDC_IBEAM MAKEINTRESOURCEA(32513)
#define IDC_CROSS MAKEINTRESOURCEA(32515)
#define IDC_HAND MAKEINTRESOURCEA(32649)
#define IDI_APPLICATION MAKEINTRESOURCEA(100)
#define IMAGE_ICON 1
#define IMAGE_BITMAP 0
#define LR_DEFAULTCOLOR 0x0000
#define LR_LOADFROMFILE 0x0010
#define LR_DEFAULTSIZE 0x0040
#define LR_SHARED 0x8000
/*SystemParametersInfo*/
#define SPI_GETWORKAREA 48
/*exceptions (shim: signals)*/
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005
#define EXCEPTION_DATATYPE_MISALIGNMENT 0x80000002
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094
typedef struct _EXCEPTION_RECORD { UINT ExceptionCode; UINT ExceptionFlags; struct _EXCEPTION_RECORD *ExceptionRecord; LPVOID ExceptionAddress; UINT NumberParameters; ULONG_PTR ExceptionInformation[2]; } EXCEPTION_RECORD;
typedef struct _CONTEXT { DWORD64 Rip; DWORD ContextFlags; } CONTEXT;   /* the fields the runtime reads (sampling profiler) */
typedef struct _EXCEPTION_POINTERS { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;
typedef LONG (CALLBACK *PTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);
typedef LONG (CALLBACK *PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);
/*process creation*/
#define CREATE_NO_WINDOW 0x08000000
#define STARTF_USESTDHANDLES 0x00000100
typedef struct _STARTUPINFOW {
    DWORD cb; LPWSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2; LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW, *LPSTARTUPINFOW;
typedef struct _PROCESS_INFORMATION { HANDLE hProcess; HANDLE hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION, *LPPROCESS_INFORMATION;
#define STILL_ACTIVE 259
/*errors*/
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define ERROR_NO_MORE_FILES 18
/*keyboard event flags*/
#define KEYEVENTF_KEYUP 2

#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define MAKEWPARAM(l, h) ((WPARAM)MAKELONG(l, h))
#define MAKEWPARAM_DECL_GUARD
#define MAKELPARAM(l, h) ((LPARAM)MAKELONG(l, h))
#define MAKELRESULT(l, h) ((LRESULT)MAKELONG(l, h))
#define LOWORD(d) ((WORD)(((DWORD_PTR)(d)) & 0xFFFF))
#define HIWORD(d) ((WORD)((((DWORD_PTR)(d)) >> 16) & 0xFFFF))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xFF))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xFF))
#define GET_WHEEL_DELTA_WPARAM(w) ((SHORT)HIWORD(w))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((BYTE)(g) << 8)) | ((BYTE)(b) << 16)))
#define GetRValue(c) ((BYTE)((c) & 0xFF))
#define GetGValue(c) ((BYTE)(((c) >> 8) & 0xFF))
#define GetBValue(c) ((BYTE)(((c) >> 16) & 0xFF))
#define MAKEINTRESOURCEA(i) ((LPCSTR)(uintptr_t)(i))
#define MAKEINTRESOURCEW(i) ((LPCWSTR)(uintptr_t)(i))
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define S_OK ((HRESULT)0L)
#define S_FALSE ((HRESULT)1L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_INVALIDARG ((HRESULT)0x80070057L)

/*BITMAPINFOHEADER: defined above (before DIBSECTION)*/
typedef struct tagBITMAPINFO { BITMAPINFOHEADER bmiHeader; DWORD bmiColors[3]; } BITMAPINFO;

/*min/max: like MinGW's windows.h, no macros in C++ (they break libstdc++); the runtime only
 * uses unqualified min/max in gpu_d3d11.cpp, which is not built on Linux*/
#ifndef __cplusplus
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

/* ------------------------------------------------------------------ kernel: memory, time, threads, processes */
DWORD GetLastError(void);
void SetLastError(DWORD);
void Sleep(DWORD ms);
int SleepEx(DWORD ms, BOOL alertable);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
BOOL QueryPerformanceCounter(LARGE_INTEGER *v);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *v);
DWORD GetCurrentProcessId(void);
DWORD GetCurrentThreadId(void);
BOOL SetThreadPriority(HANDLE, int);
int GetThreadPriority(HANDLE);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
void ExitProcess(UINT code) __attribute__((noreturn));

LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, DWORD *oldprotect);

HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
BOOL CloseHandle(HANDLE);
DWORD WaitForSingleObject(HANDLE, DWORD ms);
DWORD WaitForSingleObjectEx(HANDLE, DWORD ms, BOOL alertable);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
#define CreateEvent CreateEventW
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCWSTR name);
BOOL SetEvent(HANDLE);
BOOL ResetEvent(HANDLE);
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES, BOOL initial, LPCWSTR name);
BOOL ReleaseMutex(HANDLE);
BOOL CreatePipe(PHANDLE rd, PHANDLE wr, LPSECURITY_ATTRIBUTES, DWORD);
BOOL SetHandleInformation(HANDLE, DWORD mask, DWORD flags);
#define HANDLE_FLAG_INHERIT 0x1
BOOL GetExitCodeProcess(HANDLE, DWORD *code);
BOOL GetExitCodeThread(HANDLE, DWORD *code);
BOOL CreateProcessW(LPWSTR cmd, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL inherit, DWORD flags, LPWSTR env, LPCWSTR dir,
                    LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi);
BOOL TerminateProcess(HANDLE, UINT);

/*environment*/
BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value);
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size);
wchar_t *_wgetenv(const wchar_t *name);
int _wputenv(const wchar_t *s);
wchar_t *_wgetcwd(wchar_t *buf, int size);
int _wchdir(const wchar_t *dir);

/*command line / stdio plumbing*/
LPCWSTR GetCommandLineW(void);
LPWSTR *CommandLineToArgvW(LPCWSTR cmd, int *argc);
HANDLE GetStdHandle(DWORD which);
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
BOOL SetStdHandle(DWORD which, HANDLE h);
BOOL AttachConsole(DWORD);
#define ATTACH_PARENT_PROCESS ((DWORD)-1)
#define HWND_TOP ((HWND)0)
#define WHEEL_DELTA 120
#define CW_USEDEFAULT ((int)0x80000000)
#define COINIT_MULTITHREADED 0
void LocalFree(void *);
HGLOBAL LocalAlloc(UINT, SIZE_T);
void GlobalFree(void *);
HGLOBAL GlobalAlloc(UINT, SIZE_T);
#define GMEM_MOVEABLE 2
#define GMEM_FIXED 0
LPVOID GlobalLock(HGLOBAL);
BOOL GlobalUnlock(HGLOBAL);
SIZE_T GlobalSize(HGLOBAL);

/*waitable timers (pacing.cpp: 1 ms-resolution sleep)*/
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES, BOOL manual, LPCWSTR name);
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
BOOL SetWaitableTimer(HANDLE, const LARGE_INTEGER *due, LONG period, void *apc, LPVOID apcarg, BOOL resume);
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD flags, DWORD access);
#define TIMER_ALL_ACCESS 0x1F0003
void YieldProcessor(void);
/*futex-style waits (raster_pool worker pool)*/
BOOL WaitOnAddress(void volatile *addr, void *compare, SIZE_T size, DWORD ms);
void WakeByAddressAll(void *addr);
void WakeByAddressSingle(void *addr);
BOOL CancelWaitableTimer(HANDLE);

/* ------------------------------------------------------------------ strings */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int slen, LPWSTR dst, int dlen);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int slen, LPSTR dst, int dlen, LPCSTR def, LPBOOL used);
#define CP_UTF8 65001
#define CP_ACP 0
FILE *_wfopen(const wchar_t *path, const wchar_t *mode);
int _wremove(const wchar_t *path);
wchar_t *_wcsdup(const wchar_t *s);
int _wtoi(const wchar_t *s);
int _stricmp(const char *a, const char *b);
int _wcsicmp(const wchar_t *a, const wchar_t *b);
int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n);
int _strnicmp(const char *a, const char *b, size_t n);
LONG InterlockedExchange(LONG volatile *dst, LONG value);
typedef struct _RTL_CRITICAL_SECTION { void *unused[4]; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
void InitializeCriticalSection(CRITICAL_SECTION *);
void DeleteCriticalSection(CRITICAL_SECTION *);
void EnterCriticalSection(CRITICAL_SECTION *);
void LeaveCriticalSection(CRITICAL_SECTION *);
BOOL TryEnterCriticalSection(CRITICAL_SECTION *);
typedef long long LONG64, *PLONG64;
LONG64 InterlockedAdd64(LONG64 volatile *dst, LONG64 v);
LONG64 InterlockedIncrement64(LONG64 volatile *dst);
LONG64 InterlockedDecrement64(LONG64 volatile *dst);
LONG64 InterlockedCompareExchange64(LONG64 volatile *dst, LONG64 v, LONG64 expected);

/* ------------------------------------------------------------------ modules (loader) */
HMODULE LoadLibraryA(LPCSTR name);
HMODULE LoadLibraryW(LPCWSTR name);
HMODULE LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags);
HMODULE LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags);
#define LOAD_WITH_ALTERED_SEARCH_PATH 8
FARPROC GetProcAddress(HMODULE, LPCSTR name);
BOOL FreeLibrary(HMODULE);
HMODULE GetModuleHandleA(LPCSTR name);
HMODULE GetModuleHandleW(LPCWSTR name);
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *mod);
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
DWORD GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DWORD GetModuleFileNameW(HMODULE, LPWSTR, DWORD);

/* ------------------------------------------------------------------ files */
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD flags, HANDLE template_file);
BOOL ReadFile(HANDLE, LPVOID buf, DWORD n, LPDWORD got, LPVOID overlap);
BOOL WriteFile(HANDLE, LPCVOID buf, DWORD n, LPDWORD got, LPVOID overlap);
DWORD SetFilePointer(HANDLE, LONG dist, LONG *disthigh, DWORD method);
BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER dist, LONGLONG *newpos, DWORD method);
int _fseeki64(FILE *f, long long off, int from);
int _commit(int fd);
long long _ftelli64(FILE *f);
typedef unsigned long long DWORDLONG;
typedef struct _MEMORYSTATUSEX { DWORD dwLength, dwMemoryLoad; DWORDLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual; } MEMORYSTATUSEX, *LPMEMORYSTATUSEX;
BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL GetFileSizeEx(HANDLE, LONGLONG *size);
DWORD GetFileSize(HANDLE, LPDWORD high);
BOOL FlushFileBuffers(HANDLE);
BOOL DeleteFileW(LPCWSTR);
BOOL DeleteFileA(LPCSTR);
BOOL MoveFileW(LPCWSTR from, LPCWSTR to);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 8
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists);
BOOL CreateDirectoryW(LPCWSTR, LPSECURITY_ATTRIBUTES);
BOOL CreateDirectoryA(LPCSTR, LPSECURITY_ATTRIBUTES);
BOOL RemoveDirectoryW(LPCWSTR);
DWORD GetFileAttributesW(LPCWSTR);
BOOL GetFileAttributesExW(LPCWSTR, DWORD, LPVOID);
#define GetFileExInfoStandard 0
typedef struct _WIN32_FILE_ATTRIBUTE_DATA { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
DWORD GetFullPathNameW(LPCWSTR path, DWORD n, LPWSTR buf, LPWSTR *part);
typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH * 2]; WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;
HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW fd);
typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH * 2]; CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd);
BOOL FindNextFileA(HANDLE, LPWIN32_FIND_DATAA fd);
DWORD GetFileAttributesExA(LPCSTR, DWORD, LPVOID);
BOOL RemoveDirectoryA(LPCSTR);
BOOL FindNextFileW(HANDLE, LPWIN32_FIND_DATAW fd);
BOOL FindClose(HANDLE);
DWORD GetCurrentDirectoryW(DWORD n, LPWSTR buf);
BOOL SetCurrentDirectoryW(LPCWSTR);
BOOL GetDiskFreeSpaceExW(LPCWSTR, LPVOID, LPVOID, LPVOID);

/*INI files (Windows semantics: [section]\r\nkey=value, case-insensitive)*/
UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, INT def, LPCWSTR file);
DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size, LPCWSTR file);
BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file);

/*time of day*/
void GetSystemTime(SYSTEMTIME *);
void GetLocalTime(SYSTEMTIME *);
BOOL SystemTimeToFileTime(const SYSTEMTIME *, FILETIME *);
void GetTimeZoneInformation(LPVOID);
DWORD timeGetTime(void);
UINT timeBeginPeriod(UINT);
UINT timeEndPeriod(UINT);

/*system info (log header)*/
typedef struct _SYSTEM_INFO { DWORD dwNumberOfProcessors; } SYSTEM_INFO, *LPSYSTEM_INFO;
void GetNativeSystemInfo(SYSTEM_INFO *);
void GetSystemInfo(SYSTEM_INFO *);
void GetSystemTimeAsFileTime(FILETIME *);
typedef struct _FILETIME_TPL { DWORD dwLowDateTime, dwHighDateTime; } FILETIME_MONITOR;
BOOL GetProcessTimes(HANDLE, FILETIME *, FILETIME *, FILETIME *, FILETIME *);
#define THREAD_QUERY_LIMITED_INFORMATION 0x1000
HANDLE OpenThread(DWORD, BOOL, DWORD tid);
BOOL GetThreadTimes(HANDLE, FILETIME *, FILETIME *, FILETIME *, FILETIME *);
DWORD SuspendThread(HANDLE);
int IntersectClipRect(HDC, int, int, int, int);
BOOL SetBrushOrgEx(HDC, int, int, LPPOINT);
HWND GetDlgItem(HWND, int);
HICON LoadIconW(HINSTANCE, LPCWSTR);
#define DM_SETDEFID (WM_USER + 1)  /* dummy: IsDialogMessage sends the default button directly */
typedef struct _DISPLAY_DEVICEW { DWORD cb; WCHAR DeviceName[32], DeviceString[128]; DWORD StateFlags; } DISPLAY_DEVICEW, *LPDISPLAY_DEVICEW;
BOOL EnumDisplayDevicesW(LPCWSTR, DWORD, DISPLAY_DEVICEW *, DWORD);
int GetClassNameW(HWND, LPWSTR, int);
DWORD CharUpperBuffW(LPWSTR s, DWORD n);
typedef BOOL (CALLBACK *WNDENUMPROC)(HWND, LPARAM);
BOOL EnumWindows(WNDENUMPROC, LPARAM);
DWORD GetProcessId(HANDLE);
typedef HANDLE HRSRC, HGLOBAL_RES;
HRSRC FindResourceW(HMODULE, LPCWSTR name, LPCWSTR type);
DWORD SizeofResource(HMODULE, HRSRC);
HANDLE LoadResource(HMODULE, HRSRC);
LPVOID LockResource(HANDLE);
DWORD ResumeThread(HANDLE);
BOOL GetThreadContext(HANDLE, CONTEXT *);
typedef struct _OSVERSIONINFOW { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber; WCHAR szCSDVersion[128]; } OSVERSIONINFOW;
typedef struct _SYSTEM_POWER_STATUS { BYTE ACLineStatus, BatteryFlag; BYTE BatteryLifePercent; } SYSTEM_POWER_STATUS;
BOOL GetSystemPowerStatus(SYSTEM_POWER_STATUS *);
#define NTAPI
typedef unsigned long ULONG;
/*registry (read-only lookup: the Linux shim answers the CPU name from /proc/cpuinfo)*/
typedef HANDLE HKEY;
#define HKEY_LOCAL_MACHINE ((HKEY)1)
#define RRF_RT_REG_SZ 2
typedef LONG LSTATUS;
LSTATUS RegGetValueA(HKEY, LPCSTR subkey, LPCSTR value, DWORD flags, void *, void *data, DWORD *size);
HRESULT CoInitializeEx(LPVOID, DWORD);
void CoUninitialize(void);

/*sampling profiler + display settings (display.cpp)*/
#define CONTEXT_CONTROL 1
#define DUPLICATE_SAME_ACCESS 2
BOOL DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);
typedef struct _DEVMODEW { DWORD dmSize, dmPelsWidth, dmPelsHeight, dmDisplayFrequency; } DEVMODEW, *LPDEVMODEW;
struct tagMONITORINFOEXW : public tagMONITORINFO { WCHAR szDevice[32]; };
typedef struct tagMONITORINFOEXW MONITORINFOEXW, *LPMONITORINFOEXW;
BOOL EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW *dm);
BOOL EnumDisplaySettingsA(LPCSTR device, DWORD mode, DEVMODEW *dm);

/* ------------------------------------------------------------------ user: window, message, input */
ATOM RegisterClassA(const WNDCLASSA *);
ATOM RegisterClassW(const WNDCLASSW *);
BOOL UnregisterClassA(LPCSTR, HINSTANCE);
HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent, HMENU, HINSTANCE, LPVOID);
HWND CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w, int h, HWND parent, HMENU, HINSTANCE, LPVOID);
#define CreateWindowA(cls, title, style, x, y, w, h, parent, menu, inst, param) CreateWindowExA(0, cls, title, style, x, y, w, h, parent, menu, inst, param)
#define CreateWindowW(cls, title, style, x, y, w, h, parent, menu, inst, param) CreateWindowExW(0, cls, title, style, x, y, w, h, parent, menu, inst, param)
LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
LRESULT CallWindowProcW(WNDPROC, HWND, UINT, WPARAM, LPARAM);
BOOL DestroyWindow(HWND);
BOOL ShowWindow(HWND, int);
BOOL IsWindowVisible(HWND);
BOOL IsZoomed(HWND);
BOOL IsIconic(HWND);
BOOL UpdateWindow(HWND);
BOOL InvalidateRect(HWND, const RECT *, BOOL);
BOOL ValidateRect(HWND, const RECT *);
BOOL GetClientRect(HWND, RECT *);
BOOL GetWindowRect(HWND, RECT *);
BOOL AdjustWindowRect(RECT *, DWORD style, BOOL menu);
BOOL AdjustWindowRectEx(RECT *, DWORD style, BOOL menu, DWORD ex);
BOOL SetWindowPos(HWND, HWND after, int x, int y, int w, int h, UINT flags);
BOOL MoveWindow(HWND, int x, int y, int w, int h, BOOL repaint);
LONG GetWindowLongA(HWND, int);
LONG SetWindowLongA(HWND, int, LONG);
LONG_PTR GetWindowLongPtrW(HWND, int);
LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR);
#define GetWindowLongPtrA GetWindowLongPtrW
#define SetWindowLongPtrA SetWindowLongPtrW
LONG GetWindowLongW(HWND, int);
LONG SetWindowLongW(HWND, int, LONG);
int GetWindowTextW(HWND, LPWSTR, int);
int GetWindowTextA(HWND, LPSTR, int);
BOOL SetWindowTextW(HWND, LPCWSTR);
BOOL SetWindowTextA(HWND, LPCSTR);
int GetWindowTextLengthW(HWND);
HWND SetFocus(HWND);
HWND GetFocus(void);
BOOL SetForegroundWindow(HWND);
HWND GetForegroundWindow(void);
BOOL BringWindowToTop(HWND);
BOOL EnableWindow(HWND, BOOL);
DWORD GetWindowThreadProcessId(HWND, LPDWORD pid);
BOOL SetActiveWindow(HWND);
BOOL ScreenToClient(HWND, LPPOINT);
BOOL ClientToScreen(HWND, LPPOINT);
BOOL GetCursorPos(LPPOINT);
BOOL SetCursorPos(int, int);
HCURSOR SetCursor(HCURSOR);
HCURSOR LoadCursorA(HINSTANCE, LPCSTR);
HCURSOR LoadCursorW(HINSTANCE, LPCWSTR);
HICON LoadImageA(HINSTANCE, LPCSTR, UINT type, int cx, int cy, UINT flags);
HICON LoadImageW(HINSTANCE, LPCWSTR, UINT type, int cx, int cy, UINT flags);
BOOL DestroyIcon(HICON);
BOOL DrawIcon(HDC, int, int, HICON);
HMONITOR MonitorFromWindow(HWND, DWORD);
HMONITOR MonitorFromPoint(POINT, DWORD);
BOOL GetMonitorInfoA(HMONITOR, MONITORINFO *);
BOOL GetMonitorInfoW(HMONITOR, MONITORINFO *);
BOOL GetWindowPlacement(HWND, WINDOWPLACEMENT *);
BOOL SetWindowPlacement(HWND, const WINDOWPLACEMENT *);
int GetSystemMetrics(int);
BOOL SystemParametersInfoW(UINT, UINT, LPVOID, UINT);
BOOL FlashWindow(HWND, BOOL);
DWORD GetDoubleClickTime(void);
SHORT GetAsyncKeyState(int vk);
SHORT GetKeyState(int vk);
BOOL GetKeyboardState(BYTE *);
BOOL TrackMouseEvent(LPTRACKMOUSEEVENT);
#define TME_LEAVE 2
HWND SetCapture(HWND);
BOOL ReleaseCapture(void);
HWND GetCapture(void);
UINT SendDlgItemMessageW(HWND, int, UINT, WPARAM, LPARAM);
BOOL IsDialogMessageW(HWND, MSG *);
BOOL IsWindow(HWND);
BOOL IsWindowEnabled(HWND);
BOOL IsChild(HWND parent, HWND child);
HWND WindowFromPoint(POINT);
HWND ChildWindowFromPoint(HWND, POINT);
BOOL PtInRect(const RECT *, POINT);
BOOL OffsetRect(RECT *, int, int);
BOOL InflateRect(RECT *, int, int);
BOOL SetRect(RECT *, int, int, int, int);
BOOL SetRectEmpty(RECT *);
BOOL CopyRect(RECT *, const RECT *);
BOOL IntersectRect(RECT *, const RECT *, const RECT *);
BOOL UnionRect(RECT *, const RECT *, const RECT *);
BOOL DrawIconEx(HDC, int, int, HICON, int, int, UINT, HBRUSH, UINT);
BOOL MessageBeep(UINT);
int MulDiv(int a, int b, int c);
int SetStretchBltMode(HDC, int);
int GetDeviceCaps(HDC, int);
HBITMAP CreateCompatibleBitmap(HDC, int, int);
HWND GetActiveWindow(void);
BOOL PrintWindow(HWND, HDC, UINT);
#define PW_CLIENTONLY 1
LRESULT SendMessageA(HWND, UINT, WPARAM, LPARAM);
LRESULT SendMessageW(HWND, UINT, WPARAM, LPARAM);
BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM);
BOOL PostMessageA(HWND, UINT, WPARAM, LPARAM);
BOOL PostThreadMessageW(DWORD tid, UINT, WPARAM, LPARAM);
void PostQuitMessage(int);
int GetMessageW(MSG *, HWND, UINT, UINT);
#define GetMessage GetMessageW
BOOL PeekMessageA(MSG *, HWND, UINT, UINT, UINT remove);
BOOL PeekMessageW(MSG *, HWND, UINT, UINT, UINT remove);
#define PM_NOREMOVE 0
#define PM_REMOVE 1
#define PM_NOYIELD 2
LRESULT DispatchMessageW(const MSG *);
LRESULT DispatchMessageA(const MSG *);
BOOL TranslateMessage(const MSG *);
HWND GetParent(HWND);
HWND SetParent(HWND, HWND);
UINT_PTR SetTimer(HWND, UINT_PTR id, UINT ms, TIMERPROC);
BOOL KillTimer(HWND, UINT_PTR);
BOOL ClientToScreenDummyRemoved(void);

/* ------------------------------------------------------------------ GDI */
HDC GetDC(HWND);
int ReleaseDC(HWND, HDC);
HDC BeginPaint(HWND, PAINTSTRUCT *);
BOOL EndPaint(HWND, const PAINTSTRUCT *);
HDC CreateCompatibleDC(HDC);
BOOL DeleteDC(HDC);
HGDIOBJ SelectObject(HDC, HGDIOBJ);
BOOL DeleteObject(HGDIOBJ);
HBRUSH CreateSolidBrush(COLORREF);
HPEN CreatePen(int style, int width, COLORREF);
HFONT CreateFontW(int height, int width, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike, DWORD charset,
                  DWORD outprec, DWORD clipprec, DWORD quality, DWORD pitchfamily, LPCWSTR face);
HGDIOBJ GetStockObject(int);
int SetTextColor(HDC, COLORREF);
COLORREF GetTextColor(HDC);
int SetBkColor(HDC, COLORREF);
COLORREF GetBkColor(HDC);
int SetBkMode(HDC, int);
int SetROP2(HDC, int);
int FillRect(HDC, const RECT *, HBRUSH);
int FrameRect(HDC, const RECT *, HBRUSH);
BOOL Rectangle(HDC, int x1, int y1, int x2, int y2);
BOOL RoundRect(HDC, int x1, int y1, int x2, int y2, int ew, int eh);
BOOL MoveToEx(HDC, int, int, LPPOINT);
BOOL LineTo(HDC, int, int);
BOOL Polyline(HDC, const POINT *, int);
BOOL Polygon(HDC, const POINT *, int);
BOOL Ellipse(HDC, int, int, int, int);
COLORREF SetPixel(HDC, int, int, COLORREF);
COLORREF GetPixel(HDC, int, int);
BOOL BitBlt(HDC, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);
int StretchDIBits(HDC, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, const void *bits, const BITMAPINFO *, UINT usage, DWORD rop);
int SetDIBitsToDevice(HDC, int dx, int dy, int dw, int dh, int sx, int sy, UINT sline, UINT nlines, const void *bits, const BITMAPINFO *, UINT usage);
BOOL GetTextMetricsW(HDC, LPTEXTMETRICW);
BOOL GetTextExtentPoint32W(HDC, LPCWSTR, int, LPSIZE);
DWORD GetGlyphOutlineW(HDC, UINT code, UINT format, LPGLYPHMETRICS, DWORD size, LPVOID buf, const MAT2 *);
BOOL TextOutW(HDC, int x, int y, LPCWSTR s, int n);
int DrawTextW(HDC, LPCWSTR, int, LPRECT, UINT flags);
int DrawTextA(HDC, LPCSTR, int, LPRECT, UINT flags);
BOOL ExtTextOutW(HDC, int x, int y, UINT opts, const RECT *, LPCWSTR s, UINT n, const int *dx);
BOOL PatBlt(HDC, int, int, int, int, DWORD rop);
int SaveDC(HDC);
BOOL RestoreDC(HDC, int);

/* ------------------------------------------------------------------ shell / misc */
BOOL ShellExecuteW2(HWND, LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, int show);
#define ShellExecuteW ShellExecuteW2
HINSTANCE ShellExecuteA(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, int);
void OutputDebugStringA(LPCSTR);
void OutputDebugStringW(LPCWSTR);

/*msg box (dark_ui.cpp draws its own; the plain API exists for fallback paths and the shim maps
 * it onto the same dark box when a window exists, or onto stderr+stdIN when headless)*/
int MessageBoxW(HWND, LPCWSTR, LPCWSTR, UINT);
int MessageBoxA(HWND, LPCSTR, LPCSTR, UINT);
#define MessageBox MessageBoxA

/*exception filters*/
PTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(PTOP_LEVEL_EXCEPTION_FILTER);
PVECTORED_EXCEPTION_HANDLER AddVectoredExceptionHandler(UINT first, PVECTORED_EXCEPTION_HANDLER);
ULONG RemoveVectoredExceptionHandler(PVECTORED_EXCEPTION_HANDLER);

/* ------------------------------------------------------------------ XInput (SDL2-backed) */
#ifdef __linux__
/*the X11 handles behind a shim HWND (gpu_vulkan creates its Xlib surface from them)*/
int tf_shim_x11_window(HWND h, void **display, unsigned long *xid);
#endif
DWORD XInputGetState(DWORD idx, XINPUT_STATE *state);
DWORD XInputGetCapabilities(DWORD idx, DWORD flags, XINPUT_CAPABILITIES *caps);
void XInputEnable(BOOL);
DWORD XInputSetState(DWORD, void *);

/*keyboard layout helpers*/
UINT MapVirtualKeyW(UINT, UINT);
int GetKeyNameTextW(LONG lParam, LPWSTR out, int size);
SHORT VkKeyScanW(WCHAR ch);

#ifdef __cplusplus
}
#endif

#endif /*TF_WINDOWS_SHIM_H*/
