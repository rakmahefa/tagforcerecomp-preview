/* shim_user.cpp - the window/message half of the Win32 compatibility layer (see windows.h).
 *
 * One SDL2 window per top-level HWND; an ARGB8888 backing store per window that the GDI half
 * (shim_gdi.cpp) draws into and that EndPaint presents through an SDL renderer. A per-thread
 * message queue mimics Win32: SDL events are translated during GetMessage/PeekMessage, posted
 * messages wait in the queue, and SendMessage from another thread round-trips through the pump
 * (woken with a custom SDL event). WM_PAINT is synthesized like Windows does (lowest priority,
 * only when the queue is empty), for the dirtiest visible window. The built-in control classes
 * BUTTON (owner-draw), STATIC and EDIT cover the runtime's dark message box, the text input
 * box and the launcher.
 */
#include "windows.h"
#include <SDL.h>
#include <pthread.h>
#include <unistd.h>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <algorithm>

typedef std::string S; typedef std::wstring W;

extern S tf_w2u(const wchar_t *w, bool path);            /* shim_kern.cpp */
extern W tf_u2w(const char *s, bool path);

/*macros used below, defined before use*/
#define MK_LBUTTON_dummy(k) ((k == 1) ? 0x1 : (k == 2) ? 0x2 : 0x10)
#define GET_X_LPARAM_dummy(l) ((int)(short)LOWORD(l))
#define GET_Y_LPARAM_dummy(l) ((int)(short)HIWORD(l))
#define VK_MULTIPLY_6A 0x6A
#define VK_DIVIDE_6F 0x6F

/* ------------------------------------------------------------------ internal types */
struct WndClass { WNDPROC proc = nullptr; HBRUSH brush = nullptr; HCURSOR cursor = nullptr; };
struct Canvas;                                          /* shim_gdi.cpp: pixels + clip + GDI state */

struct Pres {                                           /* window presentation (owned here) */
    SDL_Renderer *ren = nullptr;
    SDL_Texture *tex = nullptr;
    Canvas *canvas = nullptr;                           /* the window's backing store (0xAARRGGBB) */
    int w = 0, h = 0;
    bool touched = false;                               /* a GDI op drew since the last present */
    bool vulkan_owns = false;                           /* the Vulkan backend owns the window's native surface: the SDL present path stays off (Wayland: one producer per wl_surface) */
};

struct Wnd {
    HWND self = nullptr;
    SDL_Window *win = nullptr;
    Pres *pres = nullptr;
    WNDPROC proc = nullptr;
    HINSTANCE inst = nullptr;
    DWORD style = 0, ex = 0;
    W text, cls;
    HWND parent = nullptr;
    std::vector<HWND> children;
    bool visible = false, enabled = true, destroyed = false;
    LONG_PTR userdata = 0;
    int id = 0;                                          /* control id (the HMENU of a child) */
    int x = 0, y = 0, w = 100, h = 100;                  /* client == whole window */
    bool needs_paint = true;
    bool track_leave = false;
    bool fullscreen = false;
    bool maximized = false, minimized = false;
    bool no_activate = false;                            /* WS_EX_NOACTIVATE (automated tests) */
    int ctrl_kind = 0;                                   /* 0 plain, 1 BUTTON, 2 STATIC, 3 EDIT */
    bool btn_down = false, btn_keydown = false;          /* BUTTON state */
    W edit_text; size_t edit_caret = 0; int edit_limit = 0;   /* EDIT state */
    HFONT ctrl_font = nullptr;
};

/* ------------------------------------------------------------------ globals */
static std::mutex g_mu;
static std::map<S, WndClass> g_classes;
static std::map<HWND, Wnd *> g_wnds;
static std::vector<Wnd *> g_zorder;                      /* top-level windows */
static HWND g_focus = nullptr, g_capture_wnd = nullptr, g_active = nullptr, g_foreground = nullptr;
static std::map<DWORD, std::deque<MSG>> g_queues;        /* posted messages, per thread */
static std::map<HWND, DWORD> g_wnd_thread;
static bool g_sdl_up = false;
static Uint32 g_wake_event = 0;
static bool g_quit_posted = false;
static int g_quit_code = 0;
static DWORD g_main_thread = 0;

struct SendCall { HWND hwnd; UINT msg; WPARAM w; LPARAM l; LRESULT result; bool done; };
static std::deque<SendCall *> g_sends; static std::mutex g_send_mu; static std::condition_variable g_send_cv;

struct TimerEnt { HWND hwnd; UINT_PTR id; UINT ms; SDL_TimerID tid; };
static std::vector<TimerEnt *> g_timers;

/* ------------------------------------------------------------------ SDL bootstrap */
static void sdl_init(void) {
    if (g_sdl_up) return;
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "[shim] SDL_Init failed: %s (continuing without video)\n", SDL_GetError());
        SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER);
    }
    SDL_StartTextInput();                                /* WM_CHAR comes from SDL_TEXTINPUT */
    g_wake_event = SDL_RegisterEvents(1);
    g_main_thread = (DWORD)pthread_self();
    g_sdl_up = true;
}
static void wake_pump(void) { if (!g_sdl_up) return; SDL_Event e; SDL_zero(e); e.type = g_wake_event; SDL_PushEvent(&e); }

/* ------------------------------------------------------------------ helpers */
static DWORD my_tid(void) { return (DWORD)pthread_self(); }
static Wnd *find_wnd(HWND h) { if (!h) return nullptr; auto it = g_wnds.find(h); return it == g_wnds.end() ? nullptr : it->second; }
static Wnd *sdl_to_wnd(Uint32 id) {
    for (auto &kv : g_wnds) { Wnd *w = kv.second; if (w->win && SDL_GetWindowID(w->win) == id) return w; }
    return nullptr;
}
static bool is_child_of(HWND c, HWND p) { if (!c) return false; for (HWND a = c; a;) { if (a == p) return true; Wnd *w = find_wnd(a); a = w ? w->parent : nullptr; } return false; }
static Wnd *top_level_wnd(Wnd *w) { while (w && w->parent) w = find_wnd(w->parent); return w; }

/*GDI glue implemented at the bottom of this file (needs Wnd)*/
static void pres_free(Wnd *w);
static void pres_alloc(Wnd *w);
static void wnd_present(Wnd *w);
extern void tf_gdi_default_button2(Wnd *b);
extern void tf_gdi_default_static(Wnd *s);
extern void tf_gdi_default_edit(Wnd *e);

static void wnd_dirty(Wnd *w) { if (w) w->needs_paint = true; }

/*child-to-top coordinate chain: the target's (lx,ly) in its top-level window's space*/
static void point_to_top(Wnd *target, int lx, int ly, LONG *ox, LONG *oy) {
    LONG ax = 0, ay = 0;
    std::vector<Wnd *> chain;
    for (Wnd *a = target; a; a = find_wnd(a->parent)) chain.push_back(a);
    for (size_t i = chain.size(); i-- > 0;) { ax += chain[i]->x; ay += chain[i]->y; }
    *ox = ax + lx; *oy = ay + ly;
}

/* ------------------------------------------------------------------ message delivery */
static void deliver(Wnd *w, UINT msg, WPARAM wp, LPARAM lp) {
    if (!w || !w->proc) return;
    w->proc(w->self, msg, wp, lp);
}
static void post_msg(Wnd *w, UINT msg, WPARAM wp, LPARAM lp) {
    if (!w) return;
    DWORD tid = 0;
    { auto it = g_wnd_thread.find(w->self); if (it != g_wnd_thread.end()) tid = it->second; }
    MSG m{}; m.hwnd = w->self; m.message = msg; m.wParam = wp; m.lParam = lp;
    { std::lock_guard<std::mutex> lk(g_mu); g_queues[tid].push_back(m); }
    wake_pump();
}
LRESULT SendMessageW(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Wnd *wnd = find_wnd(h);
    if (!wnd) return 0;
    DWORD tid = 0;
    { auto it = g_wnd_thread.find(h); if (it != g_wnd_thread.end()) tid = it->second; }
    if (tid == my_tid() || !tid) return wnd->proc(h, msg, w, l);
    SendCall c{h, msg, w, l, 0, false};                  /* cross-thread: round-trip through the owner's pump */
    { std::lock_guard<std::mutex> lk(g_send_mu); g_sends.push_back(&c); }
    wake_pump();
    std::unique_lock<std::mutex> lk(g_send_mu);
    while (!c.done) g_send_cv.wait(lk);
    return c.result;
}
LRESULT SendMessageA(HWND h, UINT msg, WPARAM w, LPARAM l) { return SendMessageW(h, msg, w, l); }
BOOL PostMessageW(HWND h, UINT msg, WPARAM w, LPARAM l) { post_msg(find_wnd(h), msg, w, l); return TRUE; }
BOOL PostMessageA(HWND h, UINT msg, WPARAM w, LPARAM l) { return PostMessageW(h, msg, w, l); }
BOOL PostThreadMessageW(DWORD tid, UINT msg, WPARAM w, LPARAM l) {
    MSG m{}; m.hwnd = nullptr; m.message = msg; m.wParam = w; m.lParam = l;
    { std::lock_guard<std::mutex> lk(g_mu); g_queues[tid].push_back(m); }
    wake_pump();
    return TRUE;
}
void PostQuitMessage(int code) { std::lock_guard<std::mutex> lk(g_mu); g_quit_posted = true; g_quit_code = code; wake_pump(); }

/* ------------------------------------------------------------------ keyboard state and names */
static SDL_Scancode vk_to_scancode(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_0 + (vk - '0'));
    switch (vk) {
    case VK_BACK: return SDL_SCANCODE_BACKSPACE; case VK_TAB: return SDL_SCANCODE_TAB; case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_SHIFT: return SDL_SCANCODE_LSHIFT; case VK_CONTROL: return SDL_SCANCODE_LCTRL; case VK_MENU: return SDL_SCANCODE_LALT;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE; case VK_SPACE: return SDL_SCANCODE_SPACE;
    case VK_PRIOR: return SDL_SCANCODE_PAGEUP; case VK_NEXT: return SDL_SCANCODE_PAGEDOWN; case VK_END: return SDL_SCANCODE_END; case VK_HOME: return SDL_SCANCODE_HOME;
    case VK_LEFT: return SDL_SCANCODE_LEFT; case VK_UP: return SDL_SCANCODE_UP; case VK_RIGHT: return SDL_SCANCODE_RIGHT; case VK_DOWN: return SDL_SCANCODE_DOWN;
    case VK_INSERT: return SDL_SCANCODE_INSERT; case VK_DELETE: return SDL_SCANCODE_DELETE;
    case VK_F1: return SDL_SCANCODE_F1; case VK_F2: return SDL_SCANCODE_F2; case VK_F3: return SDL_SCANCODE_F3; case VK_F4: return SDL_SCANCODE_F4;
    case VK_F5: return SDL_SCANCODE_F5; case VK_F6: return SDL_SCANCODE_F6; case VK_F7: return SDL_SCANCODE_F7; case VK_F8: return SDL_SCANCODE_F8;
    case VK_F9: return SDL_SCANCODE_F9; case VK_F10: return SDL_SCANCODE_F10; case VK_F11: return SDL_SCANCODE_F11; case VK_F12: return SDL_SCANCODE_F12;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}
SHORT GetAsyncKeyState(int vk) {
    if (!g_sdl_up) return 0;
    SDL_Scancode sc = vk_to_scancode(vk);
    if (sc == SDL_SCANCODE_UNKNOWN) return 0;
    const Uint8 *st = SDL_GetKeyboardState(nullptr);
    return st[sc] ? (SHORT)0x8001 : 0;
}
SHORT GetKeyState(int vk) { return GetAsyncKeyState(vk); }
BOOL GetKeyboardState(BYTE *out) { memset(out, 0, 256); return TRUE; }
UINT MapVirtualKeyW(UINT vk, UINT) { return vk; }
int GetKeyNameTextW(LONG lParam, LPWSTR out, int size) {
    int vk = (int)((lParam >> 16) & 0x1FF);
    const char *n = nullptr;
    static thread_local char b[16];
    if (vk >= 'A' && vk <= 'Z') { b[0] = (char)vk; b[1] = 0; n = b; }
    else switch (vk) {
        case VK_SPACE: n = "Space"; break; case VK_RETURN: n = "Enter"; break; case VK_TAB: n = "Tab"; break;
        case VK_SHIFT: n = "Shift"; break; case VK_CONTROL: n = "Ctrl"; break; case VK_MENU: n = "Alt"; break;
        case VK_BACK: n = "Backspace"; break; case VK_ESCAPE: n = "Esc"; break; case VK_DELETE: n = "Del"; break;
        case VK_INSERT: n = "Ins"; break; case VK_PRIOR: n = "Page Up"; break; case VK_NEXT: n = "Page Down"; break;
        case VK_END: n = "End"; break; case VK_HOME: n = "Home"; break; case VK_UP: n = "Up"; break; case VK_DOWN: n = "Down"; break;
        case VK_LEFT: n = "Left"; break; case VK_RIGHT: n = "Right"; break;
        default: if (vk >= VK_F1 && vk <= VK_F12) { snprintf(b, sizeof b, "F%d", vk - VK_F1 + 1); n = b; }
    }
    if (!n) { if (size) out[0] = 0; return 0; }
    W w = tf_u2w(n, false);
    int c = std::min(size - 1, (int)w.size());
    if (c < 0) c = 0;
    memcpy(out, w.data(), (size_t)c * sizeof(wchar_t)); out[c] = 0;
    return c;
}
SHORT VkKeyScanW(WCHAR c) { if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) return (SHORT)c; return -1; }

/* ------------------------------------------------------------------ focus / active / capture */
HWND SetFocus(HWND h) {
    HWND old = g_focus;
    if (old && find_wnd(old)) deliver(find_wnd(old), WM_KILLFOCUS, (WPARAM)h, 0);
    g_focus = h;
    if (h) deliver(find_wnd(h), WM_SETFOCUS, (WPARAM)old, 0);
    return old;
}
HWND GetFocus(void) { return g_focus; }
BOOL SetForegroundWindow(HWND h) { g_foreground = h; g_active = h; return TRUE; }
HWND GetForegroundWindow(void) { return g_foreground; }
HWND GetActiveWindow(void) { return g_active; }
BOOL BringWindowToTop(HWND) { return TRUE; }
BOOL EnableWindow(HWND h, BOOL on) { Wnd *w = find_wnd(h); if (!w) return FALSE; BOOL o = w->enabled; w->enabled = on != FALSE; return o; }
BOOL SetActiveWindow(HWND h) { g_active = h; return TRUE; }
HWND SetCapture(HWND h) { HWND o = g_capture_wnd; g_capture_wnd = h; return o; }
BOOL ReleaseCapture(void) { g_capture_wnd = nullptr; return TRUE; }
HWND GetCapture(void) { return g_capture_wnd; }

/* ------------------------------------------------------------------ geometry */
BOOL GetClientRect(HWND h, RECT *r) {
    Wnd *w = find_wnd(h);
    if (!w) { r->left = r->top = r->right = r->bottom = 0; return FALSE; }
    r->left = r->top = 0; r->right = w->w; r->bottom = w->h;
    return TRUE;
}
BOOL GetWindowRect(HWND h, RECT *r) {
    Wnd *w = find_wnd(h);
    if (!w) return FALSE;
    if (w->win && !w->parent) { int x = 0, y = 0; SDL_GetWindowPosition(w->win, &x, &y); r->left = x; r->top = y; }
    else { point_to_top(w, 0, 0, &r->left, &r->top); Wnd *t = top_level_wnd(w); if (t && t->win) { int ax = 0, ay = 0; SDL_GetWindowPosition(t->win, &ax, &ay); r->left += ax; r->top += ay; } }
    r->right = r->left + w->w; r->bottom = r->top + w->h;
    return TRUE;
}
BOOL AdjustWindowRect(RECT *, DWORD, BOOL) { return TRUE; }    /* client == whole window */
BOOL AdjustWindowRectEx(RECT *, DWORD, BOOL, DWORD) { return TRUE; }
static void apply_fullscreen(Wnd *w, bool on) {
    if (!w->win || w->fullscreen == on) return;
    w->fullscreen = on;
    SDL_SetWindowFullscreen(w->win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
BOOL SetWindowPos(HWND h, HWND, int x, int y, int cw, int ch, UINT flags) {
    Wnd *w = find_wnd(h);
    if (!w) return FALSE;
    bool frame = (flags & SWP_FRAMECHANGED) != 0;
    if (w->win && !w->parent) {
        if (!(flags & SWP_NOMOVE) || !(flags & SWP_NOSIZE) || frame) {
            int nx = w->x, ny = w->y, nw = w->w, nh = w->h;
            if (!(flags & SWP_NOMOVE)) { nx = x; ny = y; }
            if (!(flags & SWP_NOSIZE)) { nw = cw; nh = ch; }
            bool want_fs = (w->style & WS_POPUP) && (w->style & WS_VISIBLE);   /* display.cpp toggles fullscreen this way */
            if (frame && want_fs) apply_fullscreen(w, true);
            if (frame && !want_fs) apply_fullscreen(w, false);
            if (!w->fullscreen) { SDL_SetWindowPosition(w->win, nx, ny); SDL_SetWindowSize(w->win, nw, nh); }
        }
        if (flags & SWP_SHOWWINDOW) { w->visible = true; SDL_ShowWindow(w->win); }
        if (flags & SWP_HIDEWINDOW) { w->visible = false; SDL_HideWindow(w->win); }
    }
    if (!(flags & SWP_NOMOVE)) { w->x = x; w->y = y; }
    if (!(flags & SWP_NOSIZE) && (cw != w->w || ch != w->h)) {
        w->w = cw; w->h = ch;
        pres_alloc(w);
        deliver(w, WM_SIZE, SIZE_RESTORED, MAKELPARAM(cw, ch));
    }
    wnd_dirty(w);
    return TRUE;
}
BOOL MoveWindow(HWND h, int x, int y, int cw, int ch, BOOL) { return SetWindowPos(h, nullptr, x, y, cw, ch, 0); }
BOOL ShowWindow(HWND h, int cmd) {
    Wnd *w = find_wnd(h);
    if (!w) return FALSE;
    BOOL was = w->visible;
    if (w->win && !w->parent) {
        switch (cmd) {
        case SW_HIDE: SDL_HideWindow(w->win); w->visible = false; break;
        case SW_SHOWMAXIMIZED: SDL_ShowWindow(w->win); SDL_MaximizeWindow(w->win); w->visible = true; w->maximized = true; break;
        case SW_MINIMIZE: SDL_MinimizeWindow(w->win); w->minimized = true; break;
        case SW_RESTORE: SDL_RestoreWindow(w->win); w->visible = true; w->maximized = w->minimized = false; break;
        default: SDL_ShowWindow(w->win); w->visible = true; break;
        }
    } else w->visible = cmd != SW_HIDE;
    if (w->parent) { Wnd *p = find_wnd(w->parent); if (p) wnd_dirty(p); }
    wnd_dirty(w);
    return was;
}
BOOL IsWindowVisible(HWND h) { Wnd *w = find_wnd(h); return w && w->visible; }
BOOL IsZoomed(HWND h) { Wnd *w = find_wnd(h); if (w && w->win) return (SDL_GetWindowFlags(w->win) & SDL_WINDOW_MAXIMIZED) != 0; return w ? (BOOL)w->maximized : FALSE; }
BOOL IsIconic(HWND h) { Wnd *w = find_wnd(h); if (w && w->win) return (SDL_GetWindowFlags(w->win) & SDL_WINDOW_MINIMIZED) != 0; return w ? (BOOL)w->minimized : FALSE; }
BOOL IsWindow(HWND h) { return find_wnd(h) != nullptr; }
BOOL IsWindowEnabled(HWND h) { Wnd *w = find_wnd(h); return w && w->enabled; }
BOOL IsChild(HWND p, HWND c) { return is_child_of(c, p); }
DWORD GetWindowThreadProcessId(HWND h, LPDWORD pid) { if (pid) *pid = (DWORD)getpid(); auto it = g_wnd_thread.find(h); return it != g_wnd_thread.end() ? it->second : g_main_thread; }
LONG GetWindowLongA(HWND h, int which) {
    Wnd *w = find_wnd(h); if (!w) return 0;
    if (which == GWL_STYLE) return (LONG)w->style;
    if (which == GWL_EXSTYLE) return (LONG)w->ex;
    if (which == GWLP_USERDATA) return (LONG)w->userdata;
    return 0;
}
LONG SetWindowLongA(HWND h, int which, LONG v) {
    Wnd *w = find_wnd(h); if (!w) return 0;
    LONG o = 0;
    if (which == GWL_STYLE) { o = (LONG)w->style; w->style = (DWORD)v; }
    else if (which == GWL_EXSTYLE) { o = (LONG)w->ex; w->ex = (DWORD)v; w->no_activate = (w->ex & WS_EX_NOACTIVATE) != 0; }
    else if (which == GWLP_USERDATA) { o = (LONG)w->userdata; w->userdata = v; }
    return o;
}
LONG GetWindowLongW(HWND h, int which) { return GetWindowLongA(h, which); }
LONG SetWindowLongW(HWND h, int which, LONG v) { return SetWindowLongA(h, which, v); }
LONG_PTR GetWindowLongPtrW(HWND h, int which) { return (LONG_PTR)GetWindowLongA(h, which); }
LONG_PTR SetWindowLongPtrW(HWND h, int which, LONG_PTR v) { return (LONG_PTR)SetWindowLongA(h, which, (LONG)v); }
int GetWindowTextW(HWND h, LPWSTR out, int n) {
    Wnd *w = find_wnd(h); if (!w || n <= 0) return 0;
    int c = std::min((int)w->text.size(), n - 1);
    if (c < 0) c = 0;
    memcpy(out, w->text.data(), (size_t)c * sizeof(wchar_t)); out[c] = 0;
    return c;
}
int GetWindowTextA(HWND h, LPSTR out, int n) {
    Wnd *w = find_wnd(h); if (!w || n <= 0) return 0;
    S s = tf_w2u(w->text.c_str(), false);
    int c = std::min((int)s.size(), n - 1);
    if (c < 0) c = 0;
    memcpy(out, s.data(), (size_t)c); out[c] = 0;
    return c;
}
BOOL SetWindowTextW(HWND h, LPCWSTR t) {
    Wnd *w = find_wnd(h); if (!w) return FALSE;
    w->text = t ? t : L"";
    if (w->win) { S s = tf_w2u(w->text.c_str(), false); SDL_SetWindowTitle(w->win, s.c_str()); }
    wnd_dirty(w);
    return TRUE;
}
BOOL SetWindowTextA(HWND h, LPCSTR t) { return SetWindowTextW(h, tf_u2w(t ? t : "", false).c_str()); }
int GetWindowTextLengthW(HWND h) { Wnd *w = find_wnd(h); return w ? (int)w->text.size() : 0; }
BOOL GetWindowPlacement(HWND h, WINDOWPLACEMENT *wp) {
    Wnd *w = find_wnd(h); if (!w) return FALSE;
    wp->rcNormalPosition.left = w->x; wp->rcNormalPosition.top = w->y;
    wp->rcNormalPosition.right = w->x + w->w; wp->rcNormalPosition.bottom = w->y + w->h;
    wp->showCmd = w->maximized ? SW_SHOWMAXIMIZED : w->visible ? SW_SHOWNORMAL : SW_HIDE;
    return TRUE;
}
BOOL SetWindowPlacement(HWND h, const WINDOWPLACEMENT *wp) {
    Wnd *w = find_wnd(h); if (!w) return FALSE;
    const RECT &r = wp->rcNormalPosition;
    w->x = r.left; w->y = r.top;
    if (w->w != r.right - r.left || w->h != r.bottom - r.top) { w->w = r.right - r.left; w->h = r.bottom - r.top; pres_alloc(w); }
    if (w->win && !w->parent && !w->fullscreen) { SDL_SetWindowPosition(w->win, w->x, w->y); SDL_SetWindowSize(w->win, w->w, w->h); }
    if (wp->showCmd == SW_SHOWMAXIMIZED) { w->maximized = true; if (w->win) SDL_MaximizeWindow(w->win); }
    wnd_dirty(w);
    return TRUE;
}
int GetSystemMetrics(int m) {
    sdl_init();
    SDL_DisplayMode dm{};
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        if (m == SM_CXSCREEN) return dm.w;
        if (m == SM_CYSCREEN) return dm.h;
    }
    if (m == SM_CXICON || m == SM_CYICON) return 32;
    if (m == SM_CXSMICON || m == SM_CYSMICON) return 16;
    return 32;
}
BOOL SystemParametersInfoW(UINT a, UINT, LPVOID p, UINT) {
    if (a == SPI_GETWORKAREA) {
        SDL_Rect r{};
        RECT *o = (RECT *)p;
        if (SDL_GetDisplayUsableBounds(0, &r) == 0) o->left = r.x, o->top = r.y, o->right = r.x + r.w, o->bottom = r.y + r.h;
        else o->left = o->top = 0, o->right = GetSystemMetrics(SM_CXSCREEN), o->bottom = GetSystemMetrics(SM_CYSCREEN);
        return TRUE;
    }
    return FALSE;
}
HMONITOR MonitorFromWindow(HWND, DWORD) { return (HMONITOR)1; }
HMONITOR MonitorFromPoint(POINT, DWORD) { return (HMONITOR)1; }
static void fill_monitor(MONITORINFO *mi) {
    mi->dwFlags = 1;
    SDL_Rect b{}, u{};
    SDL_GetDisplayBounds(0, &b);
    mi->rcMonitor = {b.x, b.y, b.x + b.w, b.y + b.h};
    if (SDL_GetDisplayUsableBounds(0, &u) == 0) mi->rcWork = {u.x, u.y, u.x + u.w, u.y + u.h};
    else mi->rcWork = mi->rcMonitor;
}
BOOL GetMonitorInfoA(HMONITOR, MONITORINFO *mi) { fill_monitor(mi); return TRUE; }
BOOL GetMonitorInfoW(HMONITOR, MONITORINFO *mi) { fill_monitor(mi); return TRUE; }
BOOL FlashWindow(HWND, BOOL) { return FALSE; }
DWORD GetDoubleClickTime(void) { return 500; }
BOOL UpdateWindow(HWND h) {
    Wnd *w = find_wnd(h); if (!w) return FALSE;
    if (w->needs_paint) { w->needs_paint = false; deliver(w, WM_PAINT, 0, 0); }
    wnd_present(w);
    return TRUE;
}
BOOL InvalidateRect(HWND h, const RECT *, BOOL) { wnd_dirty(find_wnd(h)); wake_pump(); return TRUE; }
BOOL ValidateRect(HWND, const RECT *) { return TRUE; }
BOOL ClientToScreen(HWND h, LPPOINT p) { Wnd *w = find_wnd(h); if (w) { LONG gx, gy; point_to_top(w, p->x, p->y, &gx, &gy); Wnd *t = top_level_wnd(w); if (t && t->win) { int ax = 0, ay = 0; SDL_GetWindowPosition(t->win, &ax, &ay); gx += ax; gy += ay; } p->x = gx; p->y = gy; } return TRUE; }
BOOL ScreenToClient(HWND h, LPPOINT p) { Wnd *w = find_wnd(h); if (w) { Wnd *t = top_level_wnd(w); if (t && t->win) { int ax = 0, ay = 0; SDL_GetWindowPosition(t->win, &ax, &ay); p->x -= ax; p->y -= ay; } /*subtract chain offsets back down*/ std::vector<Wnd *> chain; for (Wnd *a = w; a; a = find_wnd(a->parent)) chain.push_back(a); for (size_t i = 0; i + 1 < chain.size(); i++) { p->x += chain[i + 1]->x; p->y += chain[i + 1]->y; } } return TRUE; }
BOOL GetCursorPos(LPPOINT p) { int x = 0, y = 0; SDL_GetGlobalMouseState(&x, &y); p->x = x; p->y = y; return TRUE; }
BOOL SetCursorPos(int x, int y) { SDL_WarpMouseGlobal(x, y); return TRUE; }
HCURSOR SetCursor(HCURSOR c) { if (g_sdl_up) SDL_ShowCursor(c ? SDL_ENABLE : SDL_DISABLE); return c; }
HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return (HCURSOR)3; }
HCURSOR LoadCursorW(HINSTANCE, LPCWSTR) { return (HCURSOR)3; }
HICON LoadImageA(HINSTANCE, LPCSTR, UINT, int, int, UINT) { return nullptr; }
HICON LoadImageW(HINSTANCE, LPCWSTR, UINT, int, int, UINT) { return nullptr; }
BOOL DestroyIcon(HICON) { return TRUE; }
BOOL DrawIcon(HDC, int, int, HICON) { return TRUE; }
BOOL DrawIconEx(HDC, int, int, HICON, int, int, UINT, HBRUSH, UINT) { return TRUE; }
BOOL TrackMouseEvent(LPTRACKMOUSEEVENT t) { Wnd *w = find_wnd(t->hwndTrack); if (w) w->track_leave = (t->dwFlags & TME_LEAVE) != 0; return TRUE; }
BOOL MessageBeep(UINT) { return TRUE; }
BOOL PrintWindow(HWND h, HDC dc, UINT) { Wnd *w = find_wnd(h); if (w) deliver(w, WM_PRINTCLIENT, (WPARAM)dc, 0); return TRUE; }

/* ------------------------------------------------------------------ rect helpers */
BOOL PtInRect(const RECT *r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
BOOL OffsetRect(RECT *r, int dx, int dy) { r->left += dx; r->top += dy; r->right += dx; r->bottom += dy; return TRUE; }
BOOL InflateRect(RECT *r, int dx, int dy) { r->left -= dx; r->top -= dy; r->right += dx; r->bottom += dy; return TRUE; }
BOOL SetRect(RECT *r, int a, int b, int c, int d) { r->left = a; r->top = b; r->right = c; r->bottom = d; return TRUE; }
BOOL SetRectEmpty(RECT *r) { r->left = r->top = r->right = r->bottom = 0; return TRUE; }
BOOL CopyRect(RECT *d, const RECT *s) { *d = *s; return TRUE; }
BOOL IntersectRect(RECT *o, const RECT *a, const RECT *b) {
    LONG l = std::max(a->left, b->left), t = std::max(a->top, b->top), r = std::min(a->right, b->right), bt = std::min(a->bottom, b->bottom);
    if (l >= r || t >= bt) { SetRectEmpty(o); return FALSE; }
    o->left = l; o->top = t; o->right = r; o->bottom = bt;
    return TRUE;
}
BOOL UnionRect(RECT *o, const RECT *a, const RECT *b) { SetRect(o, std::min(a->left, b->left), std::min(a->top, b->top), std::max(a->right, b->right), std::max(a->bottom, b->bottom)); return TRUE; }
int MulDiv(int a, int b, int c) { return c ? (int)(((long long)a * b + (c > 0 ? c : -c) / 2) / c) : -1; }

/* ------------------------------------------------------------------ timers */
static Uint32 timer_cb(Uint32 interval, void *param) {
    TimerEnt *t = (TimerEnt *)param;
    { std::lock_guard<std::mutex> lk(g_mu);
      bool live = false;
      for (auto *x : g_timers) if (x == t) live = true;
      if (live) { MSG m{}; m.hwnd = t->hwnd; m.message = WM_TIMER; m.wParam = (WPARAM)t->id; auto it = g_wnd_thread.find(t->hwnd); g_queues[it != g_wnd_thread.end() ? it->second : g_main_thread].push_back(m); } }
    wake_pump();
    return interval;
}
UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC) {
    sdl_init();
    if (!ms) ms = 1;
    for (auto *t : g_timers) if (t->hwnd == h && t->id == id) { t->ms = ms; return id; }
    TimerEnt *t = new TimerEnt{h, id, ms, 0};
    t->tid = SDL_AddTimer(ms, timer_cb, t);
    g_timers.push_back(t);
    return id;
}
BOOL KillTimer(HWND h, UINT_PTR id) {
    for (auto it = g_timers.begin(); it != g_timers.end(); ++it)
        if ((*it)->hwnd == h && (*it)->id == id) { if ((*it)->tid) SDL_RemoveTimer((*it)->tid); delete *it; g_timers.erase(it); return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ classes and creation */
ATOM RegisterClassA(const WNDCLASSA *wc) {
    WndClass c; c.proc = wc->lpfnWndProc; c.brush = wc->hbrBackground; c.cursor = wc->hCursor;
    g_classes[std::string(wc->lpszClassName ? wc->lpszClassName : "")] = c;
    return 1;
}
ATOM RegisterClassW(const WNDCLASSW *wc) {
    WndClass c; c.proc = wc->lpfnWndProc; c.brush = wc->hbrBackground; c.cursor = wc->hCursor;
    g_classes[tf_w2u(wc->lpszClassName ? wc->lpszClassName : L"", false)] = c;
    return 1;
}
BOOL UnregisterClassA(LPCSTR, HINSTANCE) { return TRUE; }

static LRESULT button_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT static_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT edit_proc(HWND, UINT, WPARAM, LPARAM);

static HWND create_wnd(DWORD ex, const W &cls, const W &title, DWORD style, int x, int y, int cw, int ch, HWND parent, int id, HINSTANCE inst, LPVOID param) {
    sdl_init();
    Wnd *w = new Wnd();
    w->self = (HWND)w;
    w->cls = cls; w->text = title; w->style = style; w->ex = ex; w->inst = inst;
    w->parent = (parent && parent != w->self) ? parent : nullptr;
    w->id = id;
    w->no_activate = (ex & WS_EX_NOACTIVATE) != 0;
    S cn = tf_w2u(cls.c_str(), false);
    auto it = g_classes.find(cn);
    w->proc = it != g_classes.end() ? it->second.proc : nullptr;
    if (cn == "BUTTON") { w->proc = button_proc; w->ctrl_kind = 1; }
    else if (cn == "STATIC") { w->proc = static_proc; w->ctrl_kind = 2; }
    else if (cn == "EDIT") { w->proc = edit_proc; w->ctrl_kind = 3; }
    w->x = x; w->y = y; w->w = std::max(1, cw); w->h = std::max(1, ch);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_wnds[w->self] = w;
        g_wnd_thread[w->self] = my_tid();
        if (!w->parent) g_zorder.push_back(w);
    }
    if (w->parent) { Wnd *p = find_wnd(w->parent); if (p) p->children.push_back(w->self); }
    else {
        Uint32 sdlflags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
        if (style & WS_POPUP) sdlflags |= SDL_WINDOW_BORDERLESS;
        if (w->no_activate || !(style & WS_VISIBLE)) sdlflags |= SDL_WINDOW_HIDDEN;
        if (x == (int)0x80000000 || y == (int)0x80000000) { x = 80; y = 80; }   /* CW_USEDEFAULT */
        w->win = SDL_CreateWindow("", x, y, w->w, w->h, sdlflags);
        if (w->win) { S t = tf_w2u(title.c_str(), false); SDL_SetWindowTitle(w->win, t.c_str()); }
        else fprintf(stderr, "[shim] SDL_CreateWindow failed: %s\n", SDL_GetError());
    }
    pres_alloc(w);
    CREATESTRUCTW cs{};
    cs.lpCreateParams = param; cs.hInstance = inst; cs.hwndParent = w->parent; cs.cx = w->w; cs.cy = w->h;
    cs.x = w->x; cs.y = w->y; cs.style = (LONG)style; cs.lpszName = title.c_str(); cs.lpszClass = cls.c_str(); cs.dwExStyle = ex;
    if (w->proc) w->proc(w->self, WM_NCCREATE, 0, (LPARAM)&cs);
    if (w->proc) w->proc(w->self, WM_CREATE, 0, (LPARAM)&cs);
    w->visible = (style & WS_VISIBLE) != 0 && !w->no_activate;
    return w->self;
}
HWND CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int cw, int ch, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    return create_wnd(ex, cls ? cls : L"", title ? title : L"", style, x, y, cw, ch, parent, menu ? (int)(INT_PTR)menu : 0, inst, param);
}
HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int cw, int ch, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    return create_wnd(ex, tf_u2w(cls ? cls : "", false), tf_u2w(title ? title : "", false), style, x, y, cw, ch, parent, menu ? (int)(INT_PTR)menu : 0, inst, param);
}
static void destroy_wnd(Wnd *w) {
    if (!w || w->destroyed) return;
    w->destroyed = true;
    deliver(w, WM_DESTROY, 0, 0);
    for (HWND c : std::vector<HWND>(w->children)) destroy_wnd(find_wnd(c));
    w->children.clear();
    if (g_focus && is_child_of(g_focus, w->self)) g_focus = nullptr;
    if (w->parent) { Wnd *p = find_wnd(w->parent); if (p) { p->children.erase(std::remove(p->children.begin(), p->children.end(), w->self), p->children.end()); wnd_dirty(p); } }
    else {
        g_zorder.erase(std::remove(g_zorder.begin(), g_zorder.end(), w), g_zorder.end());
        if (w->win) { SDL_DestroyWindow(w->win); w->win = nullptr; }
    }
    { std::lock_guard<std::mutex> lk(g_mu); g_wnds.erase(w->self); g_wnd_thread.erase(w->self); }
    pres_free(w);
    delete w;
}
BOOL DestroyWindow(HWND h) { Wnd *w = find_wnd(h); if (!w) return FALSE; destroy_wnd(w); return TRUE; }
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }
LRESULT CallWindowProcW(WNDPROC p, HWND h, UINT m, WPARAM w, LPARAM l) { return p ? p(h, m, w, l) : 0; }
LRESULT DefWindowProcA(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

/* ------------------------------------------------------------------ hit testing */
static Wnd *wnd_from_point(int gx, int gy) {
    for (auto it = g_zorder.rbegin(); it != g_zorder.rend(); ++it) {
        Wnd *w = *it;
        if (!w->visible) continue;
        int wx = 0, wy = 0;
        if (w->win) SDL_GetWindowPosition(w->win, &wx, &wy);
        else { wx = w->x; wy = w->y; }
        int lx = gx - wx, ly = gy - wy;
        if (lx < 0 || ly < 0 || lx >= w->w || ly >= w->h) continue;
        for (;;) {                                       /* descend into enabled children */
            Wnd *hit = nullptr;
            for (auto cit = w->children.rbegin(); cit != w->children.rend(); ++cit) {
                Wnd *c = find_wnd(*cit);
                if (!c || !c->visible || !c->enabled) continue;
                if (lx >= c->x && ly >= c->y && lx < c->x + c->w && ly < c->y + c->h) { hit = c; break; }
            }
            if (!hit) break;
            w = hit; lx -= w->x; ly -= w->y;
        }
        return w;
    }
    return nullptr;
}
HWND WindowFromPoint(POINT p) { Wnd *w = wnd_from_point(p.x, p.y); return w ? w->self : nullptr; }
HWND ChildWindowFromPoint(HWND h, POINT p) {
    Wnd *w = find_wnd(h);
    if (!w) return nullptr;
    for (auto cit = w->children.rbegin(); cit != w->children.rend(); ++cit) {
        Wnd *c = find_wnd(*cit);
        if (c && c->visible && p.x >= c->x && p.y >= c->y && p.x < c->x + c->w && p.y < c->y + c->h) return c->self;
    }
    return h;
}

/* ------------------------------------------------------------------ SDL -> Win32 input translation */
static int sdlkey_to_vk(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return (int)(k - 'a' + 'A');
    if (k >= '0' && k <= '9') return (int)k;
    if (k >= SDLK_F1 && k <= SDLK_F12) return VK_F1 + (int)(k - SDLK_F1);
    if (k >= SDLK_F13 && k <= SDLK_F24) return VK_F1 + 12 + (int)(k - SDLK_F13);
    switch (k) {
    case SDLK_BACKSPACE: return VK_BACK; case SDLK_TAB: return VK_TAB; case SDLK_RETURN: return VK_RETURN;
    case SDLK_ESCAPE: return VK_ESCAPE; case SDLK_SPACE: return VK_SPACE;
    case SDLK_PAGEUP: return VK_PRIOR; case SDLK_PAGEDOWN: return VK_NEXT; case SDLK_END: return VK_END; case SDLK_HOME: return VK_HOME;
    case SDLK_LEFT: return VK_LEFT; case SDLK_UP: return VK_UP; case SDLK_RIGHT: return VK_RIGHT; case SDLK_DOWN: return VK_DOWN;
    case SDLK_INSERT: return VK_INSERT; case SDLK_DELETE: return VK_DELETE;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return VK_SHIFT;
    case SDLK_LCTRL: case SDLK_RCTRL: return VK_CONTROL;
    case SDLK_LALT: case SDLK_RALT: return VK_MENU;
    case SDLK_LGUI: return VK_LWIN; case SDLK_RGUI: return VK_RWIN;
    case SDLK_CAPSLOCK: return VK_CAPITAL; case SDLK_NUMLOCKCLEAR: return VK_NUMLOCK; case SDLK_PAUSE: return VK_PAUSE;
    case SDLK_SEMICOLON: return VK_OEM_1; case SDLK_PLUS: return VK_OEM_PLUS; case SDLK_COMMA: return VK_OEM_COMMA;
    case SDLK_MINUS: return VK_OEM_MINUS; case SDLK_PERIOD: return VK_OEM_PERIOD; case SDLK_SLASH: return VK_OEM_2;
    case SDLK_BACKQUOTE: return VK_OEM_3; case SDLK_LEFTBRACKET: return VK_OEM_4; case SDLK_BACKSLASH: return VK_OEM_5;
    case SDLK_RIGHTBRACKET: return VK_OEM_6; case SDLK_QUOTE: return VK_OEM_7;
    case SDLK_KP_ENTER: return VK_RETURN; case SDLK_KP_PLUS: return VK_OEM_PLUS; case SDLK_KP_MINUS: return VK_OEM_MINUS;
    case SDLK_KP_MULTIPLY: return VK_MULTIPLY_6A; case SDLK_KP_DIVIDE: return VK_DIVIDE_6F;
    default: return 0;
    }
}
static void mouse_local(Wnd *w, int gx, int gy, int *lx, int *ly) {
    Wnd *t = top_level_wnd(w);
    int ax = 0, ay = 0;
    if (t && t->win) SDL_GetWindowPosition(t->win, &ax, &ay);
    else { ax = t ? t->x : 0; ay = t ? t->y : 0; }
    int tx = gx - ax, ty = gy - ay;
    std::vector<Wnd *> chain;
    for (Wnd *a = w; a; a = find_wnd(a->parent)) chain.push_back(a);
    for (size_t i = chain.size() - 1; i >= 1; i--) { tx -= chain[i - 1]->x; ty -= chain[i - 1]->y; }
    *lx = tx; *ly = ty;
}
static void handle_sdl_event(SDL_Event *e) {
    if (e->type == g_wake_event) return;
    Wnd *w = e->window.windowID ? sdl_to_wnd(e->window.windowID) : nullptr;
    switch (e->type) {
    case SDL_QUIT:
        for (auto it = g_zorder.rbegin(); it != g_zorder.rend(); ++it) deliver(*it, WM_CLOSE, 0, 0);
        break;
    case SDL_WINDOWEVENT: {
        if (!w) break;
        switch (e->window.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED: {
            int nw = e->window.data1, nh = e->window.data2;
            if (nw != w->w || nh != w->h) { w->w = nw; w->h = nh; pres_alloc(w); deliver(w, WM_SIZE, SIZE_RESTORED, MAKELPARAM(nw, nh)); }
            wnd_dirty(w);
            break;
        }
        case SDL_WINDOWEVENT_EXPOSED: wnd_dirty(w); break;
        case SDL_WINDOWEVENT_FOCUS_GAINED: g_foreground = g_active = w->self; deliver(w, WM_ACTIVATEAPP, 1, 0); deliver(w, WM_ACTIVATE, (WPARAM)WA_ACTIVE, 0); break;
        case SDL_WINDOWEVENT_FOCUS_LOST: if (g_active == w->self) { deliver(w, WM_ACTIVATEAPP, 0, 0); deliver(w, WM_ACTIVATE, (WPARAM)WA_INACTIVE, 0); } break;
        case SDL_WINDOWEVENT_CLOSE: deliver(w, WM_CLOSE, 0, 0); break;
        case SDL_WINDOWEVENT_MINIMIZED: w->minimized = true; deliver(w, WM_SIZE, SIZE_MINIMIZED, 0); break;
        case SDL_WINDOWEVENT_RESTORED: w->minimized = w->maximized = false; deliver(w, WM_SIZE, SIZE_RESTORED, MAKELPARAM(w->w, w->h)); break;
        case SDL_WINDOWEVENT_MAXIMIZED: w->maximized = true; deliver(w, WM_SIZE, SIZE_MAXIMIZED, 0); break;
        case SDL_WINDOWEVENT_LEAVE:
            if (w->track_leave) { w->track_leave = false; post_msg(w, WM_MOUSELEAVE, 0, 0); }
            break;
        }
        break;
    }
    case SDL_KEYDOWN: case SDL_KEYUP: {
        if (!w) break;
        int vk = sdlkey_to_vk(e->key.keysym.sym);
        if (!vk) break;
        LPARAM lp = 1 | (e->key.repeat ? (1 << 30) : 0) | ((e->key.keysym.mod & KMOD_ALT) ? (1L << 29) : 0);
        HWND target = (g_focus && is_child_of(g_focus, w->self)) ? g_focus : w->self;
        if (e->type == SDL_KEYDOWN) {
            if (e->key.keysym.mod & KMOD_ALT) post_msg(find_wnd(target), WM_SYSKEYDOWN, vk, lp);
            else post_msg(find_wnd(target), WM_KEYDOWN, vk, lp);
        } else if (e->key.keysym.mod & KMOD_ALT) post_msg(find_wnd(target), WM_SYSKEYUP, vk, lp);
        break;
    }
    case SDL_TEXTINPUT: {
        if (!w) break;
        W t = tf_u2w(e->text.text, false);
        HWND target = (g_focus && is_child_of(g_focus, w->self)) ? g_focus : w->self;
        for (wchar_t c : t) post_msg(find_wnd(target), WM_CHAR, c, 1);
        break;
    }
    case SDL_MOUSEMOTION: {
        int gx = e->motion.x, gy = e->motion.y;
        if (w && w->win) { int ax = 0, ay = 0; SDL_GetWindowPosition(w->win, &ax, &ay); gx += ax; gy += ay; }
        Wnd *target = g_capture_wnd ? find_wnd(g_capture_wnd) : wnd_from_point(gx, gy);
        if (!target) break;
        int lx = 0, ly = 0;
        mouse_local(target, gx, gy, &lx, &ly);
        target->proc(target->self, WM_SETCURSOR, 0, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));   /* display.cpp hides the cursor over the game area */
        post_msg(target, WM_MOUSEMOVE, 0, MAKELPARAM(lx, ly));
        break;
    }
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
        int gx = e->button.x, gy = e->button.y;
        if (w && w->win) { int ax = 0, ay = 0; SDL_GetWindowPosition(w->win, &ax, &ay); gx += ax; gy += ay; }
        Wnd *target = g_capture_wnd ? find_wnd(g_capture_wnd) : wnd_from_point(gx, gy);
        if (!target) break;
        int lx = 0, ly = 0;
        mouse_local(target, gx, gy, &lx, &ly);
        UINT msg = 0;
        if (e->button.button == SDL_BUTTON_LEFT) msg = e->type == SDL_MOUSEBUTTONDOWN ? WM_LBUTTONDOWN : WM_LBUTTONUP;
        else if (e->button.button == SDL_BUTTON_RIGHT) msg = e->type == SDL_MOUSEBUTTONDOWN ? WM_RBUTTONDOWN : WM_RBUTTONUP;
        else if (e->button.button == SDL_BUTTON_MIDDLE) msg = e->type == SDL_MOUSEBUTTONDOWN ? WM_MBUTTONDOWN : WM_MBUTTONUP;
        if (!msg) break;
        if (e->type == SDL_MOUSEBUTTONDOWN && !g_focus) SetFocus(target->self);
        post_msg(target, msg, e->type == SDL_MOUSEBUTTONDOWN ? 1 : 0, MAKELPARAM(lx, ly));
        break;
    }
    case SDL_MOUSEWHEEL: {
        int gx = 0, gy = 0;
        SDL_GetMouseState(&gx, &gy);
        if (w && w->win) { int ax = 0, ay = 0; SDL_GetWindowPosition(w->win, &ax, &ay); gx += ax; gy += ay; }
        Wnd *target = g_capture_wnd ? find_wnd(g_capture_wnd) : wnd_from_point(gx, gy);
        if (!target) break;
        int lx = 0, ly = 0;
        mouse_local(target, gx, gy, &lx, &ly);
        WPARAM wp = (WPARAM)(((uint32_t)0) | (((uint32_t)(short)(e->wheel.y * 120)) << 16));   /* GET_WHEEL_DELTA_WPARAM reads HIWORD */
        post_msg(target, WM_MOUSEWHEEL, wp, MAKELPARAM(lx, ly));
        break;
    }
    default: break;
    }
}

static void run_pending_sends(void) {
    std::vector<SendCall *> mine;
    { std::lock_guard<std::mutex> lk(g_send_mu);
      while (!g_sends.empty()) { mine.push_back(g_sends.front()); g_sends.pop_front(); } }
    for (auto *c : mine) {
        Wnd *w = find_wnd(c->hwnd);
        c->result = w ? w->proc(c->hwnd, c->msg, c->w, c->l) : 0;
        { std::lock_guard<std::mutex> lk(g_send_mu); c->done = true; }
        g_send_cv.notify_all();
    }
}
static void pump_sdl(bool block) {
    if (!g_sdl_up) return;
    SDL_Event e;
    if (block) {
        for (;;) {
            run_pending_sends();
            if (SDL_WaitEventTimeout(&e, 20) == 1) handle_sdl_event(&e);
            { std::lock_guard<std::mutex> lk(g_send_mu); if (!g_sends.empty()) continue; }
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_queues[my_tid()].empty() || g_quit_posted) return;
        }
    }
    while (SDL_PollEvent(&e)) handle_sdl_event(&e);
    run_pending_sends();
}

static BOOL take_msg(MSG *out) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto &q = g_queues[my_tid()];
    if (!q.empty()) { *out = q.front(); q.pop_front(); return TRUE; }
    if (g_quit_posted) { out->message = WM_QUIT; out->wParam = (WPARAM)g_quit_code; return TRUE; }
    return FALSE;
}
static BOOL take_paint(MSG *out) {   /* WM_PAINT: generated, lowest priority, like Windows */
    for (auto it = g_zorder.rbegin(); it != g_zorder.rend(); ++it) {
        Wnd *w = *it;
        if (!w->visible || !w->needs_paint) continue;
        for (HWND c : w->children) { Wnd *cc = find_wnd(c); if (cc && cc->visible && cc->needs_paint) { cc->needs_paint = false; out->hwnd = cc->self; out->message = WM_PAINT; return TRUE; } }
        w->needs_paint = false;
        out->hwnd = w->self; out->message = WM_PAINT;
        return TRUE;
    }
    return FALSE;
}
int GetMessageW(MSG *m, HWND, UINT, UINT) {
    sdl_init();
    for (;;) {
        pump_sdl(true);
        if (take_msg(m)) return m->message == WM_QUIT ? 0 : 1;
        if (take_paint(m)) return (int)m->message;
    }
}
BOOL PeekMessageA(MSG *m, HWND h, UINT, UINT, UINT remove) {
    sdl_init();
    pump_sdl(false);
    if (remove == PM_REMOVE) {
        if (take_msg(m)) return TRUE;
        if (take_paint(m)) return TRUE;
        return FALSE;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto &q = g_queues[my_tid()];
    if (q.empty()) return FALSE;
    *m = q.front();
    return TRUE;
}
BOOL PeekMessageW(MSG *m, HWND h, UINT a, UINT b, UINT remove) { return PeekMessageA(m, h, a, b, remove); }
LRESULT DispatchMessageW(const MSG *m) {
    if (m->hwnd) { Wnd *w = find_wnd(m->hwnd); if (w) return w->proc(m->hwnd, m->message, m->wParam, m->lParam); }
    return 0;
}
LRESULT DispatchMessageA(const MSG *m) { return DispatchMessageW(m); }
BOOL TranslateMessage(const MSG *) { return FALSE; }      /* WM_CHAR is produced from SDL_TEXTINPUT */

/* ------------------------------------------------------------------ IsDialogMessage (Tab / Enter / Esc) */
BOOL IsDialogMessageW(HWND dlg, MSG *m) {
    if (!m || !m->hwnd) return FALSE;
    Wnd *box = find_wnd(dlg);
    if (!box) return FALSE;
    if (m->message != WM_KEYDOWN) return FALSE;
    const int k = (int)m->wParam;
    if (k == VK_TAB) {
        std::vector<HWND> stops;
        for (HWND c : box->children) { Wnd *cc = find_wnd(c); if (cc && cc->visible && cc->enabled && (cc->style & WS_TABSTOP)) stops.push_back(c); }
        if (stops.empty()) return FALSE;
        int idx = 0;
        for (size_t i = 0; i < stops.size(); i++) if (stops[i] == g_focus) idx = (int)i;
        idx = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? (idx + (int)stops.size() - 1) % (int)stops.size() : (idx + 1) % (int)stops.size();
        SetFocus(stops[idx]);
        return TRUE;
    }
    if (k == VK_RETURN) {
        for (HWND c : box->children) {
            Wnd *cc = find_wnd(c);
            if (cc && cc->visible && cc->ctrl_kind == 1 && (cc->style & BS_DEFPUSHBUTTON)) { SendMessageW(c, WM_KEYDOWN, VK_SPACE, 0); SendMessageW(c, WM_KEYUP, VK_SPACE, 0); return TRUE; }
        }
    }
    if (k == VK_ESCAPE) { post_msg(box, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0); return TRUE; }
    return FALSE;
}
BOOL EnumWindows(WNDENUMPROC cb, LPARAM lp) {
    std::vector<Wnd *> tops;
    { std::lock_guard<std::mutex> lk(g_mu); tops = g_zorder; }
    for (Wnd *w : tops) if (!cb(w->self, lp)) return FALSE;
    return TRUE;
}
int GetClassNameW(HWND h, LPWSTR out, int n) {
    Wnd *w = find_wnd(h);
    if (!w || !out || n <= 0) return 0;
    int c = std::min((int)w->cls.size(), n - 1);
    memcpy(out, w->cls.data(), (size_t)c * sizeof(wchar_t));
    out[c] = 0;
    return c;
}
UINT SendDlgItemMessageW(HWND dlg, int id, UINT msg, WPARAM w, LPARAM l) {
    Wnd *d = find_wnd(dlg);
    if (!d) return 0;
    for (HWND c : d->children) { Wnd *cc = find_wnd(c); if (cc && cc->id == id) return SendMessageW(c, msg, w, l); }
    return 0;
}
HWND GetParent(HWND h) { Wnd *w = find_wnd(h); if (!w) return nullptr; return w->parent; }
HWND SetParent(HWND h, HWND p) { Wnd *w = find_wnd(h); if (!w) return nullptr; HWND o = w->parent; w->parent = p; return o; }

/* ------------------------------------------------------------------ BUTTON / STATIC / EDIT */
static void button_notify_parent(Wnd *b) {
    Wnd *p = find_wnd(b->parent);
    if (p) post_msg(p, WM_COMMAND, MAKEWPARAM(b->id, BN_CLICKED), (LPARAM)b->self);
    else deliver(b, WM_COMMAND, MAKEWPARAM(b->id, BN_CLICKED), (LPARAM)b->self);
}
static LRESULT button_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Wnd *b = find_wnd(h);
    if (!b) return 0;
    switch (m) {
    case WM_LBUTTONDOWN: b->btn_down = true; SetFocus(h); SetCapture(h); wnd_dirty(b); return 0;
    case WM_LBUTTONUP: {
        bool was = b->btn_down;
        b->btn_down = false; ReleaseCapture(); wnd_dirty(b);
        int x = GET_X_LPARAM_dummy(l), y = GET_Y_LPARAM_dummy(l);
        if (was && x >= 0 && y >= 0 && x < b->w && y < b->h && b->enabled) button_notify_parent(b);
        return 0;
    }
    case WM_KEYDOWN: if (w == VK_SPACE && !b->btn_keydown) { b->btn_keydown = true; wnd_dirty(b); } return 0;
    case WM_KEYUP: if (w == VK_SPACE && b->btn_keydown) { b->btn_keydown = false; wnd_dirty(b); if (b->enabled) button_notify_parent(b); } return 0;
    case WM_KILLFOCUS: b->btn_keydown = false; wnd_dirty(b); return 0;
    case WM_SETFONT: b->ctrl_font = (HFONT)w; return 0;
    case WM_GETFONT: return (LRESULT)b->ctrl_font;
    case WM_PAINT: {
        if (b->style & BS_OWNERDRAW) {
            Wnd *p = find_wnd(b->parent);
            extern HDC tf_gdi_ctrl_dc(Wnd *);            /* DC on the PARENT's buffer, coords already parent-relative */
            DRAWITEMSTRUCT d{};
            d.CtlType = ODT_BUTTON; d.CtlID = b->id; d.itemID = 0; d.itemAction = ODA_DRAWENTIRE;
            d.itemState = (b->btn_down || b->btn_keydown ? ODS_SELECTED : 0) | (g_focus == h ? ODS_FOCUS : 0) | (b->enabled ? 0 : ODS_DISABLED);
            d.hwndItem = h; d.hDC = tf_gdi_ctrl_dc(b);
            d.rcItem = {b->x, b->y, b->x + b->w, b->y + b->h};
            if (p) deliver(p, WM_DRAWITEM, (WPARAM)b->id, (LPARAM)&d);
            else tf_gdi_default_button2(b);
        } else tf_gdi_default_button2(b);
        return 0;
    }
    default: return DefWindowProcW(h, m, w, l);
    }
}
static LRESULT static_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Wnd *s = find_wnd(h);
    if (!s) return 0;
    switch (m) {
    case WM_SETFONT: s->ctrl_font = (HFONT)w; return 0;
    case WM_GETFONT: return (LRESULT)s->ctrl_font;
    case WM_PAINT: tf_gdi_default_static(s); return 0;
    default: return DefWindowProcW(h, m, w, l);
    }
}
static LRESULT edit_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Wnd *e = find_wnd(h);
    if (!e) return 0;
    switch (m) {
    case WM_SETFONT: e->ctrl_font = (HFONT)w; return 0;
    case WM_GETFONT: return (LRESULT)e->ctrl_font;
    case WM_SETTEXT: e->text.assign((const wchar_t *)l); e->edit_caret = e->text.size(); wnd_dirty(e); return TRUE;
    case WM_GETTEXT: { int n = (int)w; if (n > 0) { int c = std::min((int)e->text.size(), n - 1); if (c < 0) c = 0; memcpy((void *)l, e->text.data(), (size_t)c * sizeof(wchar_t)); ((wchar_t *)l)[c] = 0; } return TRUE; }
    case WM_CHAR: {
        wchar_t c = (wchar_t)w;
        if (c == 8) { if (e->edit_caret) { e->text.erase(e->edit_caret - 1, 1); e->edit_caret--; } }
        else if (c == 127) { if (e->edit_caret < e->text.size()) e->text.erase(e->edit_caret, 1); }
        else if (c >= 32) { if (!e->edit_limit || (int)e->text.size() < e->edit_limit) { e->text.insert(e->edit_caret, 1, c); e->edit_caret++; } }
        else return 0;
        if (e->style & ES_NUMBER && e->text.find_first_not_of(L"0123456789") != W::npos) { e->text.pop_back(); e->edit_caret--; }
        wnd_dirty(e);
        { Wnd *p = find_wnd(e->parent); if (p) post_msg(p, WM_COMMAND, MAKEWPARAM(e->id, 0x300), (LPARAM)h); }
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_LEFT && e->edit_caret) e->edit_caret--;
        else if (w == VK_RIGHT && e->edit_caret < e->text.size()) e->edit_caret++;
        else if (w == VK_HOME) e->edit_caret = 0;
        else if (w == VK_END) e->edit_caret = e->text.size();
        else if (w == VK_DELETE && e->edit_caret < e->text.size()) { e->text.erase(e->edit_caret, 1); wnd_dirty(e); }
        return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: wnd_dirty(e); return 0;
    case EM_SETSEL: e->edit_caret = (size_t)l; return 0;
    case EM_LIMITTEXT: e->edit_limit = (int)w; return 0;
    case WM_PAINT: tf_gdi_default_edit(e); return 0;
    default: return DefWindowProcW(h, m, w, l);
    }
}

/* ------------------------------------------------------------------ presentation glue (SDL renderer per top-level window) */
static void pres_free(Wnd *w) {
    if (!w->pres) return;
    extern void tf_gdi_free_canvas(Canvas *);
    if (w->pres->tex) SDL_DestroyTexture(w->pres->tex);
    if (w->pres->ren) SDL_DestroyRenderer(w->pres->ren);
    tf_gdi_free_canvas(w->pres->canvas);
    delete w->pres;
    w->pres = nullptr;
}
static void pres_alloc(Wnd *w) {
    if (!w->pres) w->pres = new Pres();
    if (w->pres->w == w->w && w->pres->h == w->h && w->pres->canvas) return;
    extern Canvas *tf_gdi_new_canvas(int, int);
    extern void tf_gdi_free_canvas(Canvas *);
    if (w->pres->tex) { SDL_DestroyTexture(w->pres->tex); w->pres->tex = nullptr; }
    if (w->pres->canvas) tf_gdi_free_canvas(w->pres->canvas);
    w->pres->canvas = tf_gdi_new_canvas(w->w, w->h);
    w->pres->w = w->w; w->pres->h = w->h;
    w->pres->touched = true;
}
static std::mutex g_ren_mu;   /* the per-window SDL renderer is reached from several threads: the presents below, and the Vulkan handoff (tf_shim_wnd_set_vulkan) */

static void wnd_present(Wnd *w) {
    Wnd *t = top_level_wnd(w);
    if (!t || !t->pres || !t->pres->canvas || !t->win) return;
    Pres *p = t->pres;
    /* one renderer per window, but more than one thread can reach a present: the game thread
     * (ReleaseDC below), the message pumps of the UI thread (EndPaint, UpdateWindow)... SDL
     * renderers are not thread-safe */
    std::lock_guard<std::mutex> lk(g_ren_mu);
    /* a window the Vulkan backend presents to (a native Wayland window) must not get SDL/GDI
     * buffer commits as well: one producer per wl_surface, and a buffer commit without an
     * acquire point under linux-drm-syncobj-v1 (the explicit-sync protocol Mesa's WSI wraps the
     * surface with) is a fatal protocol error - the compositor kills the connection
     * ("wp_linux_drm_syncobj_surface_v1: error 4: explicit sync is used, but no acquire point
     * is set"). The game frame and the overlay go out through the swapchain instead */
    if (p->vulkan_owns) {
        static int told;
        if (told++ < 3) fprintf(stderr, "[shim] GDI draw on the Vulkan window: not presented (the swapchain owns the surface)\n");
        return;
    }
    if (!p->ren) {
        /* no PRESENTVSYNC: on Windows a GetDC/EndPaint draw reaches the screen at once and the
         * game's own limiter does the pacing (the software renderer's frame goes out through
         * here every quadro); a vsynced present would block the game thread on the compositor
         * (up to a whole refresh per frame - or forever on an occluded Wayland window) */
        p->ren = SDL_CreateRenderer(t->win, -1, SDL_RENDERER_ACCELERATED);
        if (!p->ren) p->ren = SDL_CreateRenderer(t->win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!p->ren) return;
    if (!p->tex) p->tex = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, p->w, p->h);
    if (!p->tex) return;
    void *pix = nullptr; int pitch = 0;
    if (SDL_LockTexture(p->tex, nullptr, &pix, &pitch) == 0) {
        extern void tf_gdi_canvas_pixels(Canvas *, uint32_t **, int *, int *);
        uint32_t *src = nullptr; int stride = 0;
        tf_gdi_canvas_pixels(p->canvas, &src, &stride, nullptr);
        for (int y = 0; y < p->h; y++) memcpy((uint8_t *)pix + (size_t)y * pitch, (uint8_t *)src + (size_t)y * stride, (size_t)p->w * 4);
        SDL_UnlockTexture(p->tex);
    }
    SDL_RenderCopy(p->ren, p->tex, nullptr, nullptr);
    SDL_RenderPresent(p->ren);
    p->touched = false;
}

/*the DC a window's WM_PAINT works on (shim_gdi provides canvas-backed DCs)*/
extern HDC tf_gdi_wnd_dc(Wnd *w);

HDC GetDC(HWND h) { Wnd *w = find_wnd(h); return w ? tf_gdi_wnd_dc(w) : nullptr; }
int ReleaseDC(HWND h, HDC dc) {
    extern void tf_gdi_free_dc(HDC);
    tf_gdi_free_dc(dc);
    /* On Windows a GetDC/ReleaseDC draw is on the screen at once; here the GDI draws into the
     * window's canvas and only a present pushes it to the SDL window. The game's per-frame
     * blit (display.cpp's software renderer) uses exactly this pair - EndPaint/UpdateWindow
     * never ran for it, so the window stayed black - so a draw that touched the canvas is
     * presented here, like EndPaint does for WM_PAINT */
    Wnd *w = find_wnd(h);
    if (w) { Wnd *t = top_level_wnd(w); if (t && t->pres && t->pres->touched) wnd_present(w); }
    return 1;
}
HDC BeginPaint(HWND h, PAINTSTRUCT *ps) {
    Wnd *w = find_wnd(h);
    if (!w) return nullptr;
    ps->hdc = tf_gdi_wnd_dc(w);
    ps->fErase = TRUE;
    ps->rcPaint = {0, 0, w->w, w->h};
    return ps->hdc;
}
BOOL EndPaint(HWND h, const PAINTSTRUCT *ps) {
    Wnd *w = find_wnd(h);
    extern void tf_gdi_free_dc(HDC);
    if (ps && ps->hdc) tf_gdi_free_dc(ps->hdc);
    if (w && w->pres && w->pres->touched) wnd_present(w);   /* only when GDI actually drew (Vulkan presents itself) */
    return TRUE;
}

/* ------------------------------------------------------------------ DCs bound to a window's buffer */
extern HDC tf_gdi_dc_for_canvas(Canvas *c, int orgx, int orgy);   /* shim_gdi.cpp */
extern void tf_gdi_dc_set_touched(HDC dc, bool *touched);         /* shim_gdi.cpp: GDI draws mark the window */
HDC tf_gdi_wnd_dc(Wnd *w) {
    Wnd *t = top_level_wnd(w);
    if (!t || !t->pres || !t->pres->canvas) return nullptr;
    LONG ox = 0, oy = 0;
    if (w != t) point_to_top(w, 0, 0, &ox, &oy);
    HDC dc = tf_gdi_dc_for_canvas(t->pres->canvas, (int)ox, (int)oy);
    if (dc) tf_gdi_dc_set_touched(dc, &t->pres->touched);
    return dc;
}
/*owner-draw DC: the PARENT's buffer, coordinates already parent-relative (DRAWITEMSTRUCT::rcItem)*/
HDC tf_gdi_ctrl_dc(Wnd *b) {
    Wnd *p = find_wnd(b->parent);
    Wnd *t = p ? top_level_wnd(p) : top_level_wnd(b);
    if (!t || !t->pres || !t->pres->canvas) return nullptr;
    HDC dc;
    if (p && p != t) { LONG ox, oy; point_to_top(p, 0, 0, &ox, &oy); dc = tf_gdi_dc_for_canvas(t->pres->canvas, (int)ox, (int)oy); }
    else dc = tf_gdi_dc_for_canvas(t->pres->canvas, 0, 0);
    if (dc) tf_gdi_dc_set_touched(dc, &t->pres->touched);
    return dc;
}

/* ------------------------------------------------------------------ default look of the built-in controls */
static void ctrl_frame(Wnd *c, HDC dc, COLORREF edge) {
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, c->x, c->y, c->x + c->w, c->y + c->h);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p);
}
void tf_gdi_default_button2(Wnd *b) {
    Wnd *p = find_wnd(b->parent);
    HDC dc = tf_gdi_ctrl_dc(b);
    if (!dc) return;
    const bool down = b->btn_down || b->btn_keydown;
    RECT r = {b->x, b->y, b->x + b->w, b->y + b->h};
    HBRUSH bg = CreateSolidBrush(down ? RGB(52, 62, 100) : RGB(38, 46, 76));
    FillRect(dc, &r, bg); DeleteObject(bg);
    ctrl_frame(b, dc, RGB(50, 60, 94));
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(236, 240, 250));
    HGDIOBJ of = b->ctrl_font ? SelectObject(dc, b->ctrl_font) : nullptr;
    RECT tr = r; if (down) OffsetRect(&tr, 0, 1);
    DrawTextW(dc, b->text.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (of) SelectObject(dc, of);
    if (p) wnd_dirty(p);
    extern void tf_gdi_free_dc(HDC);
    tf_gdi_free_dc(dc);
}
void tf_gdi_default_static(Wnd *s) {
    Wnd *p = find_wnd(s->parent);
    HDC dc = tf_gdi_ctrl_dc(s);
    if (!dc) return;
    HBRUSH bg = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (p) {   /* the parent picks the colors (WM_CTLCOLORSTATIC, like Windows) */
        COLORREF tcol = RGB(236, 240, 250);
        HBRUSH got = (HBRUSH)SendMessageW(p->self, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)s->self);
        if (got) bg = got;
        tcol = GetTextColor(dc);
        SetTextColor(dc, tcol);
    } else SendMessageW(s->self, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)s->self);
    RECT r = {s->x, s->y, s->x + s->w, s->y + s->h};
    FillRect(dc, &r, bg);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = s->ctrl_font ? SelectObject(dc, s->ctrl_font) : nullptr;
    UINT f = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    if (s->style & SS_CENTER) f = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    if (s->style & SS_ENDELLIPSIS) f |= DT_END_ELLIPSIS;
    RECT tr = r;
    DrawTextW(dc, s->text.c_str(), -1, &tr, f);
    if (of) SelectObject(dc, of);
    if (p) wnd_dirty(p);
    extern void tf_gdi_free_dc(HDC);
    tf_gdi_free_dc(dc);
}
void tf_gdi_default_edit(Wnd *e) {
    Wnd *p = find_wnd(e->parent);
    HDC dc = tf_gdi_ctrl_dc(e);
    if (!dc) return;
    COLORREF tcol = RGB(236, 240, 250);
    if (p) SendMessageW(p->self, WM_CTLCOLOREDIT, (WPARAM)dc, (LPARAM)e->self);
    RECT r = {e->x, e->y, e->x + e->w, e->y + e->h};
    HBRUSH bg = CreateSolidBrush(GetBkColor(dc));
    FillRect(dc, &r, bg); DeleteObject(bg);
    ctrl_frame(e, dc, RGB(50, 60, 94));
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, tcol);
    HGDIOBJ of = e->ctrl_font ? SelectObject(dc, e->ctrl_font) : nullptr;
    RECT tr = {e->x + 6, e->y + 2, e->x + e->w - 4, e->y + e->h - 2};
    W shown = e->text;
    RECT caretr = tr;
    DrawTextW(dc, shown.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (GetFocus() == e->self) {   /* caret as a thin bar after the text */
        wchar_t pre[256] = L"";
        size_t n = std::min(e->edit_caret, (size_t)255);
        wcsncpy(pre, e->text.c_str(), n); pre[n] = 0;
        RECT pr = caretr;
        DrawTextW(dc, pre, -1, &pr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        int cx = caretr.left + (pr.right - pr.left);
        RECT cr = {cx, caretr.top + 3, cx + 1, caretr.bottom - 3};
        HBRUSH cb = CreateSolidBrush(RGB(222, 182, 92));
        FillRect(dc, &cr, cb); DeleteObject(cb);
    }
    if (of) SelectObject(dc, of);
    if (p) wnd_dirty(p);
    extern void tf_gdi_free_dc(HDC);
    tf_gdi_free_dc(dc);
}

/*the X11 handles behind a shim HWND (the Vulkan backend builds its Xlib surface from them)*/
#include <SDL_syswm.h>
extern "C" int tf_shim_x11_window(HWND h, void **display, unsigned long *xid) {
    Wnd *w = find_wnd(h);
    while (w && w->parent) w = find_wnd(w->parent);
    if (!w || !w->win) return 0;
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w->win, &info)) return 0;
#ifdef SDL_VIDEO_DRIVER_X11
    if (info.subsystem != SDL_SYSWM_X11) return 0;
    if (display) *display = (void *)info.info.x11.display;
    if (xid) *xid = (unsigned long)info.info.x11.window;
    return 1;
#else
    (void)display; (void)xid;
    return 0;
#endif
}

/*the Wayland handles behind a shim HWND (the Vulkan backend builds its Wayland surface from
 * them when SDL gave the window to the compositor natively: no XWayland, or SDL_VIDEODRIVER=wayland)*/
extern "C" int tf_shim_wl_window(HWND h, void **display, void **surface) {
    Wnd *w = find_wnd(h);
    while (w && w->parent) w = find_wnd(w->parent);
    if (!w || !w->win) return 0;
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w->win, &info)) return 0;
#ifdef SDL_VIDEO_DRIVER_WAYLAND
    if (info.subsystem != SDL_SYSWM_WAYLAND) return 0;
    if (display) *display = (void *)info.info.wl.display;
    if (surface) *surface = (void *)info.info.wl.surface;
    return 1;
#else
    (void)display; (void)surface;
    return 0;
#endif
}

/*the Vulkan backend claims the window (on = it built a Wayland Vulkan surface on it, and Mesa's
 * WSI will wrap the wl_surface with the explicit-sync protocol when the compositor offers it) or
 * releases it (off): while it owns the window the shim's present path stays off (see wnd_present)
 * - Wayland allows one buffer producer per surface*/
extern "C" void tf_shim_wnd_set_vulkan(HWND h, int on) {
    Wnd *w = find_wnd(h);
    if (!w) return;
    Wnd *t = top_level_wnd(w);
    if (!t || !t->pres) return;
    std::lock_guard<std::mutex> lk(g_ren_mu);
    if (on) {
        if (t->pres->vulkan_owns) return;
        t->pres->vulkan_owns = true;
        /* drop the SDL renderer, if one existed before Vulkan took the window: its EGL window
         * would be a second buffer producer on the same surface */
        if (t->pres->tex) { SDL_DestroyTexture(t->pres->tex); t->pres->tex = nullptr; }
        if (t->pres->ren) { SDL_DestroyRenderer(t->pres->ren); t->pres->ren = nullptr; }
    } else t->pres->vulkan_owns = false;
}

/*screen size for the log (shim_kern's EnumDisplaySettingsW)*/
extern "C" int tf_shim_screen_w(void) { return GetSystemMetrics(SM_CXSCREEN); }
extern "C" int tf_shim_screen_h(void) { return GetSystemMetrics(SM_CYSCREEN); }

/*GetDlgItem (shim_kern forwards here: the window manager owns the children)*/
extern "C" DWORD tf_shim_dlg_item(HWND dlg, int id) {
    Wnd *d = find_wnd(dlg);
    if (!d) return 0;
    for (HWND c : d->children) { Wnd *cc = find_wnd(c); if (cc && cc->id == id) return (DWORD)(uintptr_t)cc->self; }
    return 0;
}
