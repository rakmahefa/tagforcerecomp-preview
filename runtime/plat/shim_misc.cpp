/* shim_misc.cpp - the remaining corners of the Win32 surface (see windows.h):
 * the update check (winhttp: reported unavailable, one log line), the ISO picker
 * (commdlg: zenity when there is a display, a terminal prompt otherwise), xdg-open
 * (ShellExecute), msgbox fallback, DWM no-ops, waveOut stubs and the misc user32 bits.
 */
#include "windows.h"
#include "commdlg.h"
#include "mmsystem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <sys/stat.h>

extern std::string tf_w2u(const wchar_t *w, bool path);   /* shim_kern.cpp */
extern std::wstring tf_u2w(const char *s, bool path);
int tf_msgbox(HWND owner, const wchar_t *text, const wchar_t *title, UINT flags);   /* dark_ui.cpp */

/* ------------------------------------------------------------------ winhttp (update check) */
HINTERNET WinHttpOpen(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD) {
    static bool told = false;
    if (!told) { told = true; fprintf(stderr, "[update] the update check is not available in the Linux build yet\n"); }
    return nullptr;
}
BOOL WinHttpSetTimeouts(HINTERNET, int, int, int, int) { return FALSE; }
HINTERNET WinHttpConnect(HINTERNET, LPCWSTR, WORD, DWORD) { return nullptr; }
HINTERNET WinHttpOpenRequest(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD) { return nullptr; }
BOOL WinHttpSendRequest(HINTERNET, LPCWSTR, DWORD, const void *, DWORD, DWORD, DWORD_PTR) { return FALSE; }
BOOL WinHttpReceiveResponse(HINTERNET, void *) { return FALSE; }
BOOL WinHttpQueryHeaders(HINTERNET, DWORD, LPCWSTR, void *, DWORD *, DWORD *) { return FALSE; }
BOOL WinHttpReadData(HINTERNET, void *, DWORD, DWORD *) { return FALSE; }
BOOL WinHttpCloseHandle(HINTERNET) { return FALSE; }

/* ------------------------------------------------------------------ commdlg (the ISO picker) */
static bool have_display(void) { return getenv("DISPLAY") != nullptr || getenv("WAYLAND_DISPLAY") != nullptr; }
BOOL GetOpenFileNameW(LPOPENFILENAMEW of) {
    if (!of || !of->lpstrFile || of->nMaxFile < 4) return FALSE;
    std::string flt;
    if (of->lpstrFilter) {
        /*"Name\0*.iso\0\0" (wide) -> "*.iso" patterns*/
        const wchar_t *p = of->lpstrFilter;
        while (*p) {
            while (*p) p++;
            p++;
            if (*p) {
                for (const wchar_t *q = p; *q; q++) flt += (char)*q;
                break;
            }
        }
    }
    if (flt.empty()) flt = "*.iso";
    char out[4096] = "";
    if (have_display()) {
        std::string cmd = "zenity --file-selection --title='Select your game ISO' --file-filter='" + flt + "' 2>/dev/null";
        FILE *f = popen(cmd.c_str(), "r");
        if (f) {
            if (fgets(out, sizeof out, f)) {
                size_t n = strlen(out);
                while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
            }
            pclose(f);
        }
    }
    if (!out[0]) {   /*headless: ask on the terminal (automated runs pipe an answer in)*/
        fprintf(stderr, "[iso] pick the game ISO (path, empty quits): ");
        fflush(stderr);
        if (!fgets(out, sizeof out, stdin)) return FALSE;
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
        if (!n) return FALSE;
    }
    std::wstring w = tf_u2w(out, true);
    if (w.size() + 1 > of->nMaxFile) return FALSE;
    wcscpy(of->lpstrFile, w.c_str());
    return TRUE;
}

/* ------------------------------------------------------------------ dwmapi (dark title bar: no-op) */
HRESULT DwmSetWindowAttribute(HWND, DWORD, const void *, DWORD) { return S_OK; }
HRESULT DwmGetCompositionTimingInfo(void *, void *) { return E_FAIL; }
HRESULT DwmFlush(void) { return S_OK; }

/* ------------------------------------------------------------------ shell (xdg-open) */
BOOL ShellExecuteW2(HWND, LPCWSTR, LPCWSTR file, LPCWSTR, LPCWSTR dir, int) {
    if (!file) return FALSE;
    std::string f = tf_w2u(file, true);
    std::string d = dir ? tf_w2u(dir, true) : "";
    std::string cmd = "xdg-open \"" + f + "\" >/dev/null 2>&1 &";
    int rc = system(cmd.c_str());
    (void)d;
    return rc == 0 || true;
}
HINSTANCE ShellExecuteA(HWND, LPCSTR, LPCSTR file, LPCSTR, LPCSTR, int) {
    if (!file) return nullptr;
    std::string cmd = "xdg-open \"" + std::string(file) + "\" >/dev/null 2>&1 &";
    system(cmd.c_str());
    return (HINSTANCE)42;
}
void OutputDebugStringA(LPCSTR s) { fprintf(stderr, "%s", s ? s : ""); }
void OutputDebugStringW(LPCWSTR s) { if (s) fprintf(stderr, "%ls", s); }

/* ------------------------------------------------------------------ msgbox fallback (the dark box handles the real one) */
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) { return tf_msgbox(owner, text, title, flags); }
int MessageBoxA(HWND owner, LPCSTR text, LPCSTR title, UINT flags) {
    return tf_msgbox(owner, tf_u2w(text ? text : "", false).c_str(), tf_u2w(title ? title : "", false).c_str(), flags);
}

/* ------------------------------------------------------------------ waveOut stubs (the Linux audio backend is SDL) */
UINT waveOutGetNumDevs(void) { return 0; }
UINT waveOutGetDevCapsA(UINT, void *, UINT) { return MMSYSERR_BADDEVICEID; }
UINT waveOutOpen(HWAVEOUT *, UINT, const WAVEFORMATEX *, DWORD_PTR, DWORD_PTR, DWORD) { return MMSYSERR_BADDEVICEID; }
UINT waveOutPrepareHeader(HWAVEOUT, WAVEHDR *, UINT) { return MMSYSERR_BADDEVICEID; }
UINT waveOutWrite(HWAVEOUT, WAVEHDR *, UINT) { return MMSYSERR_BADDEVICEID; }
UINT waveOutUnprepareHeader(HWAVEOUT, WAVEHDR *, UINT) { return MMSYSERR_BADDEVICEID; }
UINT waveOutReset(HWAVEOUT) { return MMSYSERR_BADDEVICEID; }
UINT waveOutClose(HWAVEOUT) { return MMSYSERR_BADDEVICEID; }

/* ------------------------------------------------------------------ user32 odds and ends */
BOOL SetThreadDpiAwarenessContext_dummy(void);
UINT GetDpiForWindow_dummy(HWND);
DWORD GetCurrentDirectoryA(DWORD n, LPSTR buf) {
    char c[4096];
    if (!getcwd(c, sizeof c)) return 0;
    if (strlen(c) + 1 > n) return 0;
    strcpy(buf, c);
    return (DWORD)strlen(c);
}

/*embedded resources: the ELF carries none (the Windows exe has the icon/PNG resources)*/
extern "C" {
HRSRC FindResourceW(HINSTANCE, LPCWSTR, LPCWSTR) { return nullptr; }
DWORD SizeofResource(HINSTANCE, HRSRC) { return 0; }
HANDLE LoadResource(HINSTANCE, HRSRC) { return nullptr; }
LPVOID LockResource(HANDLE) { return nullptr; }
}
