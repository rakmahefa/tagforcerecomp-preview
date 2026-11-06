/* shim_kern.cpp - the kernel half of the Win32 compatibility layer (see windows.h).
 * Memory (VirtualAlloc -> mmap), time, threads/events/mutexes/timers, processes with pipes
 * (CreateProcessW -> fork/exec), wide-char files and directory scans, INI files, wide/UTF-8
 * conversion, the module loader (LoadLibrary -> dlopen, with the project's DLL names mapped to
 * their Linux equivalents) and the SEH emulation (SIGSEGV/SIGBUS -> vectored handlers).
 */
#include "windows.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <ucontext.h>
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <new>
#include <limits.h>
#include <atomic>
#include <algorithm>

#include <malloc.h>
#include <strings.h>
#include <wctype.h>

typedef std::string S; typedef std::wstring W;

/*the shim's own fake module exports (XInput via SDL, avrt, uxtheme): shim_xinput/shim_misc*/
extern "C" void *tf_shim_proc(void *module, const char *name);

/* ------------------------------------------------------------------ conversions */
/*UTF-32 (Linux wchar_t) <-> UTF-8. Paths additionally map '\\' -> '/' (a backslash is never a
 * filename character on Windows, so every one in a Win32 path is a separator).*/
static size_t u8_len(const unsigned char *p) {
    if (*p < 0x80) return 1;
    if ((*p & 0xE0) == 0xC0) return 2;
    if ((*p & 0xF0) == 0xE0) return 3;
    if ((*p & 0xF8) == 0xF0) return 4;
    return 1;
}
static void u8_put(char *o, uint32_t c) {
    if (c < 0x80) { o[0] = (char)c; }
    else if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); }
    else { o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F)); o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F)); }
}
S tf_w2u(const wchar_t *w, bool path) {
    S s;
    if (!w) return s;
    for (; *w; w++) {
        uint32_t c = (uint32_t)*w;
        if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;   /* lone surrogate (should not happen with UTF-32) */
        char b[4]; u8_put(b, c);
        if (path && c == L'\\') s += '/'; else s.append(b, u8_len((const unsigned char *)b));
    }
    return s;
}
W tf_u2w(const char *p, bool path) {
    W w;
    if (!p) return w;
    for (; *p;) {
        const unsigned char *q = (const unsigned char *)p;
        size_t n = u8_len(q);
        uint32_t c = 0;
        if (n == 1) c = q[0];
        else if (n == 2) c = ((q[0] & 0x1F) << 6) | (q[1] & 0x3F);
        else if (n == 3) c = ((q[0] & 0x0F) << 12) | ((q[1] & 0x3F) << 6) | (q[2] & 0x3F);
        else c = ((q[0] & 0x07) << 18) | ((q[1] & 0x3F) << 12) | ((q[2] & 0x3F) << 6) | (q[3] & 0x3F);
        if (c > 0x10FFFF) c = 0xFFFD;
        w += (wchar_t)(path && c == '/' ? L'\\' : c);
        p += n;
    }
    return w;
}
int MultiByteToWideChar(UINT cp, DWORD, LPCSTR src, int slen, LPWSTR dst, int dlen) {
    if (!src) return 0;
    bool nul = slen < 0;                       /* -1: the source is null-terminated and the null is copied */
    if (slen < 0) slen = (int)strlen(src);
    W w = tf_u2w(std::string(src, slen).c_str(), false);
    if (!dst) return (int)w.size() + 1;        /* size query: with the null */
    if (dlen <= 0) return 0;
    int c = std::min(dlen - (nul ? 1 : 0), (int)w.size());
    memcpy(dst, w.data(), (size_t)c * sizeof(wchar_t));
    if (nul) dst[c] = 0;                       /* Windows semantics: the terminator is written and counted */
    return c + (nul ? 1 : 0);
}
int WideCharToMultiByte(UINT cp, DWORD, LPCWSTR src, int slen, LPSTR dst, int dlen, LPCSTR, LPBOOL) {
    if (!src) return 0;
    bool nul = slen < 0;
    if (slen < 0) slen = (int)wcslen(src);
    S s = tf_w2u(std::wstring(src, slen).c_str(), false);
    if (!dst) return (int)s.size() + 1;
    if (dlen <= 0) return 0;
    int c = std::min(dlen - (nul ? 1 : 0), (int)s.size());
    memcpy(dst, s.data(), (size_t)c);
    if (nul) dst[c] = 0;
    return c + (nul ? 1 : 0);
}
wchar_t *_wgetenv(const wchar_t *name) {
    static thread_local std::wstring keep;
    const char *v = getenv(tf_w2u(name, false).c_str());
    if (!v) return nullptr;
    keep = tf_u2w(v, false);
    return &keep[0];
}
int _wputenv(const wchar_t *s) { return putenv(strdup(tf_w2u(s, false).c_str())) == 0 ? 0 : -1; }
int _wtoi(const wchar_t *s) { return (int)wcstol(s, nullptr, 10); }
int _stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
int _strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n) { return wcsncasecmp(a, b, n); }
DWORD CharUpperBuffW(LPWSTR s, DWORD n) { for (DWORD i = 0; i < n && s[i]; i++) s[i] = (wchar_t)towupper(s[i]); return n; }
int _wcsicmp(const wchar_t *a, const wchar_t *b) { return wcscasecmp(a, b); }
wchar_t *_wcsdup(const wchar_t *s) { return wcsdup(s); }
int _wremove(const wchar_t *p) { return remove(tf_w2u(p, true).c_str()); }
FILE *_wfopen(const wchar_t *path, const wchar_t *mode) {
    FILE *f = fopen(tf_w2u(path, true).c_str(), tf_w2u(mode, false).c_str());
    return f;
}
wchar_t *_wgetcwd(wchar_t *buf, int size) {
    char c[4096];
    if (!getcwd(c, sizeof c)) return nullptr;
    W w = tf_u2w(c, true);
    if ((int)w.size() + 1 > size) return nullptr;
    wcscpy(buf, w.c_str());
    return buf;
}
int _wchdir(const wchar_t *dir) { return chdir(tf_w2u(dir, true).c_str()); }
LONG InterlockedExchange(LONG volatile *dst, LONG value) { return __atomic_exchange_n(dst, value, __ATOMIC_SEQ_CST); }

/* ------------------------------------------------------------------ handles */
struct tf_handle_s {
    enum K { H_FILE, H_PIPE_RD, H_PIPE_WR, H_THREAD, H_EVENT, H_MUTEX, H_PROCESS, H_TIMER, H_DL, H_FIND, H_CONSOLE } kind;
    int refcnt = 1;
    /*file/pipe*/ int fd = -1;
    /*thread*/ pthread_t th{}; std::atomic<int> *t_done = nullptr; DWORD *t_code = nullptr; bool joinable = false;
    /*event*/ bool manual = false, signaled = false; pthread_mutex_t mu{}; pthread_cond_t cv{};
    /*mutex(named)*/ std::string flock_path; int flock_fd = -1;
    /*process*/ pid_t pid = -1; std::atomic<int> *p_done = nullptr; int p_status = 0; int pipefd[2] = {-1, -1};  /* pipefd: SIGCHLD-ish waker */
    /*timer*/ uint64_t deadline_ns = 0;   /* CLOCK_MONOTONIC */
    /*dl*/ void *dso = nullptr;
    /*find*/ void *find = nullptr;
};
static std::mutex g_hmu;
static DWORD g_lasterr;
DWORD GetLastError(void) { return g_lasterr; }
void SetLastError(DWORD e) { g_lasterr = e; }

static HANDLE hnew(tf_handle_s::K k) { tf_handle_s *h = new tf_handle_s; h->kind = k; return h; }

/* ------------------------------------------------------------------ memory */
/* VirtualAlloc/VirtualFree: the runtime reserves big regions (the PSP's 1 GiB window,
 * coroutine stacks, the ISO cache) and commits slices of them later, like on Windows.
 * A Linux mmap() never overlays an existing mapping without MAP_FIXED, so a previous
 * version treated the address as a "hint": every commit landed somewhere else, p != addr,
 * and host_create() failed with "falha ao mapear memoria" on every start. Now a
 * reservation is a PROT_NONE mapping and a commit inside it is an mprotect() on those
 * pages. VirtualFree(base, 0, MEM_RELEASE) has no munmap() equivalent ("free the whole
 * region"), so the reservations are remembered here with their size. */
static std::mutex g_va_mu;
static std::map<uint8_t *, size_t> g_va_regions;   /* reservation base -> bytes */
static int va_prot(DWORD protect) {
    if (protect == PAGE_NOACCESS) return PROT_NONE;
    if (protect == PAGE_READONLY) return PROT_READ;
    if (protect == PAGE_EXECUTE_READWRITE) return PROT_READ | PROT_WRITE | PROT_EXEC;
    return PROT_READ | PROT_WRITE;
}
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    uint8_t *addr = (uint8_t *)address;
    const int prot = va_prot(protect);
    if (addr && !(type & MEM_RESERVE)) {   /* commit pages inside an existing reservation */
        if (mprotect(addr, size, prot) != 0) return nullptr;
        return addr;
    }
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (addr) {
#ifdef MAP_FIXED_NOREPLACE
        flags |= MAP_FIXED_NOREPLACE;   /* refuse instead of clobbering (kernel >= 4.17) */
#else
        flags |= MAP_FIXED;
#endif
    } else if (!(type & MEM_COMMIT)) {
        flags |= MAP_NORESERVE;   /* a reservation is address space only, like on Windows */
    }
    void *p = mmap(addr, size, prot, flags, -1, 0);
    if (p == MAP_FAILED) return nullptr;   /* EEXIST: the address is taken; VirtualAlloc fails too */
    { std::lock_guard<std::mutex> lk(g_va_mu); g_va_regions[(uint8_t *)p] = size; }
    return p;
}
BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
    uint8_t *addr = (uint8_t *)address;
    if (type & MEM_DECOMMIT) { mprotect(addr, size, PROT_NONE); madvise(addr, size, MADV_DONTNEED); return TRUE; }
    if (type & MEM_RELEASE) {
        size_t len = size;   /* Windows wants 0 here: the whole reservation, looked up by base */
        { std::lock_guard<std::mutex> lk(g_va_mu);
          auto it = g_va_regions.find(addr);
          if (it != g_va_regions.end()) { if (!len) len = it->second; g_va_regions.erase(it); } }
        if (!len) return FALSE;
        return munmap(addr, len) == 0 ? TRUE : FALSE;
    }
    return FALSE;
}
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, DWORD *old) {
    int p = va_prot(protect);
    if (old) {   /* not tracked precisely: report the common RW */
        *old = PAGE_READWRITE;
    }
    static const uintptr_t pg = sysconf(_SC_PAGESIZE) - 1;   /* Windows rounds to pages; mprotect requires them */
    uintptr_t lo = (uintptr_t)addr & ~pg, hi = ((uintptr_t)addr + size + pg) & ~pg;
    return mprotect((void *)lo, (size_t)(hi - lo), p) == 0 ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ time */
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
void Sleep(DWORD ms) {
    struct timespec ts = {(time_t)ms / 1000, (long)((ms % 1000) * 1000000)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}
int SleepEx(DWORD ms, BOOL) { Sleep(ms); return 0; }
DWORD GetTickCount(void) { return (DWORD)(now_ns() / 1000000ull); }
ULONGLONG GetTickCount64(void) { return now_ns() / 1000000ull; }
DWORD timeGetTime(void) { return GetTickCount(); }
UINT timeBeginPeriod(UINT) { return 0; }   /* Linux timers are already fine-grained */
UINT timeEndPeriod(UINT) { return 0; }
BOOL QueryPerformanceCounter(LARGE_INTEGER *v) { v->QuadPart = (LONGLONG)now_ns(); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER *v) { v->QuadPart = 1000000000ll; return TRUE; }
void GetSystemTime(SYSTEMTIME *st) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; gmtime_r(&ts.tv_sec, &tm);
    st->wYear = tm.tm_year + 1900; st->wMonth = tm.tm_mon + 1; st->wDayOfWeek = tm.tm_wday; st->wDay = tm.tm_mday;
    st->wHour = tm.tm_hour; st->wMinute = tm.tm_min; st->wSecond = tm.tm_sec; st->wMilliseconds = (WORD)(ts.tv_nsec / 1000000);
}
void GetLocalTime(SYSTEMTIME *st) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    st->wYear = tm.tm_year + 1900; st->wMonth = tm.tm_mon + 1; st->wDayOfWeek = tm.tm_wday; st->wDay = tm.tm_mday;
    st->wHour = tm.tm_hour; st->wMinute = tm.tm_min; st->wSecond = tm.tm_sec; st->wMilliseconds = (WORD)(ts.tv_nsec / 1000000);
}
BOOL SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft) {
    struct tm tm{}; tm.tm_year = st->wYear - 1900; tm.tm_mon = st->wMonth - 1; tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour; tm.tm_min = st->wMinute; tm.tm_sec = st->wSecond;
    uint64_t t = (uint64_t)timegm(&tm) * 10000000ull + 116444736000000000ull + st->wMilliseconds * 10000ull;
    ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32);
    return TRUE;
}
void GetTimeZoneInformation(LPVOID) {}

DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD GetProcessId(HANDLE h) { return h && h->kind == tf_handle_s::H_PROCESS ? h->pid : (DWORD)getpid(); }
static __thread DWORD t_tid;
DWORD GetCurrentThreadId(void) { if (!t_tid) t_tid = (DWORD)syscall(SYS_gettid); return t_tid; }
HANDLE GetCurrentThread(void) { return (HANDLE)1; }
HANDLE GetCurrentProcess(void) { return (HANDLE)2; }
BOOL SetThreadPriority(HANDLE h, int pri) {
    /*the background I/O modes (THREAD_MODE_BACKGROUND_*) are only a hint: accept and ignore*/
    if (pri == THREAD_MODE_BACKGROUND_BEGIN || pri == THREAD_MODE_BACKGROUND_END) return TRUE;
    (void)h;
    int nice = 0;
    if (pri >= THREAD_PRIORITY_TIME_CRITICAL) nice = -10;
    else if (pri >= THREAD_PRIORITY_ABOVE_NORMAL) nice = -5;
    else if (pri >= THREAD_PRIORITY_NORMAL) nice = 0;
    else if (pri >= THREAD_PRIORITY_BELOW_NORMAL) nice = 5;
    else nice = 10;
    if (nice < 0 && geteuid() != 0) return TRUE;   /* cannot raise priority unprivileged: pretend, like Windows often does anyway */
    setpriority(PRIO_PROCESS, 0, nice);
    return TRUE;
}
int GetThreadPriority(HANDLE) { return THREAD_PRIORITY_NORMAL; }

/* ------------------------------------------------------------------ threads */
struct ThreadCtx { LPTHREAD_START_ROUTINE fn; LPVOID arg; std::atomic<int> *done; DWORD *code; };
static void *thread_trampoline(void *p) {
    ThreadCtx *c = (ThreadCtx *)p;
    /*each emulated thread gets an alternate signal stack: a SIGSEGV on a coroutine stack (small,
     * guard-paged) must still be able to run the handler*/
    static thread_local std::vector<char> altstack(1 << 17);   /* 128 KiB (SIGSTKSZ is not constant in modern glibc) */
    stack_t ss{}; ss.ss_sp = altstack.data(); ss.ss_size = altstack.size(); ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
    ss.ss_flags = SS_ONSTACK; (void)ss;
    DWORD rc = c->fn(c->arg);
    if (c->code) *c->code = rc;
    c->done->store(1); delete c;
    return nullptr;
}
HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD, LPDWORD) {
    HANDLE h = hnew(tf_handle_s::H_THREAD);
    auto *done = new std::atomic<int>(0);
    auto *code = new DWORD(0);
    ThreadCtx *c = new ThreadCtx{fn, arg, done, code};
    h->t_done = done; h->t_code = code; h->joinable = true;
    if (pthread_create(&h->th, nullptr, thread_trampoline, c) != 0) { delete c; delete h; return nullptr; }
    return h;
}
BOOL GetExitCodeThread(HANDLE h, DWORD *code) { if (!h || h->kind != tf_handle_s::H_THREAD) return FALSE; *code = h->t_done->load() ? *h->t_code : STILL_ACTIVE; return TRUE; }

/* ------------------------------------------------------------------ events / mutexes / timers */
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCWSTR) {
    HANDLE h = hnew(tf_handle_s::H_EVENT);
    h->manual = manual != FALSE; h->signaled = initial != FALSE;
    pthread_mutex_init(&h->mu, nullptr); pthread_cond_init(&h->cv, nullptr);
    return h;
}
BOOL SetEvent(HANDLE h) { if (!h || h->kind != tf_handle_s::H_EVENT) return FALSE; pthread_mutex_lock(&h->mu); h->signaled = true; pthread_cond_broadcast(&h->cv); pthread_mutex_unlock(&h->mu); return TRUE; }
BOOL ResetEvent(HANDLE h) { if (!h || h->kind != tf_handle_s::H_EVENT) return FALSE; pthread_mutex_lock(&h->mu); h->signaled = false; pthread_mutex_unlock(&h->mu); return TRUE; }
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES, BOOL initial, LPCWSTR name) {
    HANDLE h = hnew(tf_handle_s::H_MUTEX);
    /*named mutex -> an advisory lock file (the save lock survives process crashes exactly like
     * a Windows named mutex held by a dead process being abandoned)*/
    std::string n = name ? tf_w2u(name, false) : "";
    for (char &c : n) if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') c = '_';
    if (!n.empty()) {
        h->flock_path = "/tmp/tf_winmutex_" + n;
        h->flock_fd = open(h->flock_path.c_str(), O_CREAT | O_RDWR, 0666);
    }
    if (initial && h->flock_fd >= 0) flock(h->flock_fd, LOCK_EX | LOCK_NB);
    return h;
}
BOOL ReleaseMutex(HANDLE h) { if (!h || h->kind != tf_handle_s::H_MUTEX || h->flock_fd < 0) return FALSE; flock(h->flock_fd, LOCK_UN); return TRUE; }
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR) { return hnew(tf_handle_s::H_TIMER); }
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *, LPVOID, BOOL) {
    if (!h || h->kind != tf_handle_s::H_TIMER || !due) return FALSE;
    int64_t rel = -due->QuadPart;   /* Windows: negative = relative, in 100 ns */
    h->deadline_ns = now_ns() + (uint64_t)rel * 100ull;
    (void)period;   /* one-shot only in this codebase */
    return TRUE;
}
BOOL CancelWaitableTimer(HANDLE h) { if (!h || h->kind != tf_handle_s::H_TIMER) return FALSE; h->deadline_ns = 0; return TRUE; }

static DWORD wait_one(HANDLE h, DWORD ms) {
    if (!h) { g_lasterr = 6; return WAIT_FAILED; }
    if (h == (HANDLE)1 || h == (HANDLE)2) return WAIT_OBJECT_0;   /* current thread/process: instant */
    switch (h->kind) {
    case tf_handle_s::H_EVENT: {
        uint64_t deadline = ms == INFINITE ? ~0ull : now_ns() + (uint64_t)ms * 1000000ull;
        pthread_mutex_lock(&h->mu);
        while (!h->signaled) {
            if (deadline == ~0ull) pthread_cond_wait(&h->cv, &h->mu);
            else {
                uint64_t left = deadline > now_ns() ? deadline - now_ns() : 0;
                if (!left) { pthread_mutex_unlock(&h->mu); return WAIT_TIMEOUT; }
                struct timespec ts{(time_t)(left / 1000000000ull), (long)(left % 1000000000ull)};
                if (pthread_cond_timedwait(&h->cv, &h->mu, &ts) == ETIMEDOUT && !h->signaled) { pthread_mutex_unlock(&h->mu); return WAIT_TIMEOUT; }
            }
        }
        if (!h->manual) h->signaled = false;
        pthread_mutex_unlock(&h->mu);
        return WAIT_OBJECT_0;
    }
    case tf_handle_s::H_MUTEX: {
        if (h->flock_fd < 0) return WAIT_OBJECT_0;
        int flags = ms == INFINITE ? 0 : LOCK_NB;
        uint64_t deadline = ms == INFINITE ? ~0ull : now_ns() + (uint64_t)ms * 1000000ull;
        for (;;) {
            if (flock(h->flock_fd, LOCK_EX | LOCK_NB) == 0) return WAIT_OBJECT_0;
            if (deadline == ~0ull || now_ns() >= deadline) return WAIT_TIMEOUT;
            usleep(2000);
        }
    }
    case tf_handle_s::H_THREAD: {
        if (!h->t_done->load()) {
            uint64_t deadline = ms == INFINITE ? ~0ull : now_ns() + (uint64_t)ms * 1000000ull;
            while (!h->t_done->load()) {
                if (deadline != ~0ull && now_ns() >= deadline) return WAIT_TIMEOUT;
                usleep(500);
            }
        }
        if (h->joinable) { pthread_join(h->th, nullptr); h->joinable = false; }
        return WAIT_OBJECT_0;
    }
    case tf_handle_s::H_PROCESS: {
        if (h->p_done && !h->p_done->load()) {
            uint64_t deadline = ms == INFINITE ? ~0ull : now_ns() + (uint64_t)ms * 1000000ull;
            while (!h->p_done->load()) {
                if (deadline != ~0ull && now_ns() >= deadline) return WAIT_TIMEOUT;
                usleep(1000);
            }
        }
        return WAIT_OBJECT_0;
    }
    case tf_handle_s::H_TIMER: {
        if (!h->deadline_ns) return WAIT_OBJECT_0;
        uint64_t left = h->deadline_ns > now_ns() ? h->deadline_ns - now_ns() : 0;
        if (ms != INFINITE && left > (uint64_t)ms * 1000000ull) left = (uint64_t)ms * 1000000ull;
        if (left) {
            struct timespec ts{(time_t)(left / 1000000000ull), (long)(left % 1000000000ull)};
            while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
        }
        h->deadline_ns = 0;
        return WAIT_OBJECT_0;
    }
    default: return WAIT_OBJECT_0;
    }
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return wait_one(h, ms); }
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL) { return wait_one(h, ms); }
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    if (n == 1) return wait_one(hs[0], ms);
    uint64_t deadline = ms == INFINITE ? ~0ull : now_ns() + (uint64_t)ms * 1000000ull;
    for (;;) {
        for (DWORD i = 0; i < n; i++) if (wait_one(hs[i], 0) == WAIT_OBJECT_0) { if (!all) return WAIT_OBJECT_0 + i; }
        if (all) {
            bool every = true;
            for (DWORD i = 0; i < n; i++) if (wait_one(hs[i], 0) != WAIT_OBJECT_0) every = false;
            if (every) return WAIT_OBJECT_0;
        }
        if (deadline != ~0ull && now_ns() >= deadline) return WAIT_TIMEOUT;
        if (deadline != ~0ull) { uint64_t left = deadline - now_ns(); struct timespec ts{(time_t)(left / 1000000000ull), (long)(left % 1000000000ull)}; nanosleep(&ts, nullptr); }
        else usleep(500);
    }
}
BOOL CloseHandle(HANDLE h) {
    if (!h || h == (HANDLE)1 || h == (HANDLE)2) return TRUE;
    switch (h->kind) {
    case tf_handle_s::H_FILE: case tf_handle_s::H_PIPE_RD: case tf_handle_s::H_PIPE_WR:
        if (h->fd >= 0) close(h->fd);
        break;
    case tf_handle_s::H_THREAD:
        if (h->joinable) pthread_detach(h->th);
        delete h->t_done; delete h->t_code;
        break;
    case tf_handle_s::H_EVENT: pthread_mutex_destroy(&h->mu); pthread_cond_destroy(&h->cv); break;
    case tf_handle_s::H_MUTEX: if (h->flock_fd >= 0) { flock(h->flock_fd, LOCK_UN); close(h->flock_fd); } break;
    case tf_handle_s::H_PROCESS: delete h->p_done; if (h->pipefd[0] >= 0) close(h->pipefd[0]); break;
    default: break;
    }
    delete h;
    return TRUE;
}

/* ------------------------------------------------------------------ pipes + processes */
BOOL CreatePipe(PHANDLE rd, PHANDLE wr, LPSECURITY_ATTRIBUTES, DWORD) {
    int fds[2];
    if (pipe(fds) != 0) return FALSE;
    *rd = hnew(tf_handle_s::H_PIPE_RD); (*rd)->fd = fds[0];
    *wr = hnew(tf_handle_s::H_PIPE_WR); (*wr)->fd = fds[1];
    return TRUE;
}
BOOL SetHandleInformation(HANDLE, DWORD, DWORD) { return TRUE; }   /* inheritance handled in CreateProcessW */

/*split a Windows command line: quoted exe, then arguments separated by spaces (quotes group them)*/
static std::vector<S> split_cmd(const W &cmd) {
    std::vector<S> out; S cur; bool q = false;
    for (wchar_t c : cmd) {
        if (c == L'"') q = !q;
        else if (c == L' ' && !q) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += (char)(c < 128 ? c : '?');
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
struct ChildReaper {   /* SIGCHLD -> mark the matching process handle done */
    std::map<pid_t, tf_handle_s *> procs; std::mutex mu;
};
static ChildReaper g_reaper;
static void sigchld(int) {
    for (;;) {
        int st = 0; pid_t pid = waitpid(-1, &st, WNOHANG);
        if (pid <= 0) break;
        std::lock_guard<std::mutex> lk(g_reaper.mu);
        auto it = g_reaper.procs.find(pid);
        if (it != g_reaper.procs.end()) {
            it->second->p_status = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
            it->second->p_done->store(1);
        }
    }
}
BOOL CreateProcessW(LPWSTR cmdW, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL inherit, DWORD flags, LPWSTR, LPCWSTR dir,
                    LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    /*the runtime only ever spawns one child (the Python translator) with redirected stdout/err*/
    W cmd = cmdW ? cmdW : L"";
    std::vector<S> argv = split_cmd(cmd);
    if (argv.empty()) return FALSE;
    /*re-expand argv[0] from UTF-16 so that paths with accents survive*/
    S exe = tf_w2u(cmd.c_str(), true);
    size_t sp = exe.find(' ');
    if (exe.size() > 2 && exe[0] == '"') { size_t e = exe.find('"', 1); if (e != S::npos) exe = exe.substr(1, e - 1); }
    else if (sp != S::npos) exe = exe.substr(0, sp);
    argv[0] = exe;

    HANDLE out_wr = si && (si->dwFlags & STARTF_USESTDHANDLES) ? si->hStdOutput : nullptr;
    HANDLE in_rd = si && (si->dwFlags & STARTF_USESTDHANDLES) ? si->hStdInput : nullptr;

    pid_t pid = fork();
    if (pid == 0) {
        /*child*/
        if (dir) chdir(tf_w2u(dir, true).c_str());
        if (out_wr) { dup2(out_wr->fd, 1); dup2(out_wr->fd, 2); }
        if (in_rd) dup2(in_rd->fd, 0);
        else { int dn = open("/dev/null", O_RDONLY); dup2(dn, 0); }
        /*close every other descriptor (we do not track them all; close the ones that matter: none here)*/
        std::vector<char *> cargv;
        for (auto &a : argv) cargv.push_back(&a[0]);
        cargv.push_back(nullptr);
        setenv("PYTHONIOENCODING", "utf-8", 1);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    if (pid < 0) return FALSE;
    HANDLE hp = hnew(tf_handle_s::H_PROCESS);
    hp->pid = pid; hp->p_done = new std::atomic<int>(0);
    { std::lock_guard<std::mutex> lk(g_reaper.mu); g_reaper.procs[pid] = hp; }
    pi->hProcess = hp; pi->hThread = (HANDLE)2; pi->dwProcessId = (DWORD)pid; pi->dwThreadId = 0;
    return TRUE;
}
BOOL GetExitCodeProcess(HANDLE h, DWORD *code) {
    if (!h || h->kind != tf_handle_s::H_PROCESS) return FALSE;
    if (!h->p_done->load()) { int st = 0; if (waitpid(h->pid, &st, WNOHANG) == h->pid) { h->p_status = WIFEXITED(st) ? WEXITSTATUS(st) : 1; h->p_done->store(1); } }
    *code = h->p_done->load() ? (DWORD)h->p_status : STILL_ACTIVE;
    return TRUE;
}
BOOL TerminateProcess(HANDLE h, UINT code) {
    if (!h || h->kind != tf_handle_s::H_PROCESS) return FALSE;
    kill(h->pid, SIGTERM);
    (void)code;
    return TRUE;
}

/* ------------------------------------------------------------------ stdio plumbing */
static HANDLE std_handles[3];
HANDLE GetStdHandle(DWORD which) {
    int i = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : 2;
    if (!std_handles[i]) {
        std_handles[i] = hnew(tf_handle_s::H_FILE);
        /*dup: the handle must keep pointing at the ORIGINAL target when diag.cpp later dup2()s
         * the stderr fd over to the log pipe (otherwise the tee writes back into the pipe) */
        std_handles[i]->fd = dup(i);
        if (std_handles[i]->fd < 0) std_handles[i]->fd = i;
    }
    return std_handles[i];
}
BOOL SetStdHandle(DWORD which, HANDLE h) {
    int i = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : 2;
    std_handles[i] = h;
    return TRUE;
}
BOOL AttachConsole(DWORD) { return FALSE; }   /* Linux already has a working stderr */
void LocalFree(void *p) { free(p); }
HGLOBAL LocalAlloc(UINT, SIZE_T n) { return (HGLOBAL)malloc(n); }
void GlobalFree(void *p) { free(p); }
HGLOBAL GlobalAlloc(UINT, SIZE_T n) { return (HGLOBAL)malloc(n); }
LPVOID GlobalLock(HGLOBAL p) { return p; }
BOOL GlobalUnlock(HGLOBAL) { return TRUE; }
SIZE_T GlobalSize(HGLOBAL p) { return p ? malloc_usable_size(p) : 0; }

/* ------------------------------------------------------------------ command line */
static std::vector<std::wstring> g_argv_w;
static std::wstring g_cmdline;
extern "C" void tf_shim_set_argv(int argc, char **argv) {   /* called from main_run.cpp */
    /* args are verbatim: a value is not necessarily a path, and std::filesystem paths built from
     * them must keep the real separator (the file APIs map '\\' -> '/' either way, tf_u2w(true)) */
    for (int i = 0; i < argc; i++) { g_argv_w.push_back(tf_u2w(argv[i], false)); }
    for (size_t i = 0; i < g_argv_w.size(); i++) { if (i) g_cmdline += L" "; g_cmdline += g_argv_w[i]; }
}
LPCWSTR GetCommandLineW(void) { return g_cmdline.c_str(); }
LPWSTR *CommandLineToArgvW(LPCWSTR, int *argc) {
    *argc = (int)g_argv_w.size();
    LPWSTR *v = (LPWSTR *)malloc(sizeof(LPWSTR *) * (g_argv_w.size() + 1));
    for (size_t i = 0; i < g_argv_w.size(); i++) v[i] = wcsdup(g_argv_w[i].c_str());
    v[g_argv_w.size()] = nullptr;
    return v;
}

/* ------------------------------------------------------------------ environment */
BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) {
    S n = tf_w2u(name, false);
    if (!value || !*value) { unsetenv(n.c_str()); return TRUE; }
    return setenv(n.c_str(), tf_w2u(value, false).c_str(), 1) == 0 ? TRUE : FALSE;
}
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size) {
    const char *v = getenv(tf_w2u(name, false).c_str());
    if (!v) return 0;
    W w = tf_u2w(v, false);
    if (!buf) return (DWORD)w.size() + 1;
    if (w.size() + 1 > size) return w.size() + 1;
    wcscpy(buf, w.c_str());
    return (DWORD)w.size();
}


/* ------------------------------------------------------------------ MSVC CRT odds and ends */
int _fseeki64(FILE *f, long long off, int from) { return fseeko64(f, off, from); }
int _commit(int fd) { return fsync(fd) == 0 ? 0 : -1; }
long long _ftelli64(FILE *f) { return ftello64(f); }
BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *ms) {
    memset(ms, 0, sizeof *ms);
    ms->dwLength = sizeof *ms;
    long pages = sysconf(_SC_PHYS_PAGES), pgsize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pgsize > 0) ms->ullTotalPhys = (DWORDLONG)pages * pgsize;
    long avail = sysconf(_SC_AVPHYS_PAGES);
    if (avail > 0) ms->ullAvailPhys = (DWORDLONG)avail * pgsize;
    if (ms->ullTotalPhys) ms->dwMemoryLoad = (DWORD)(100 - ms->ullAvailPhys * 100 / ms->ullTotalPhys);
    ms->ullTotalPageFile = ms->ullTotalPhys; ms->ullAvailPageFile = ms->ullAvailPhys;
    ms->ullTotalVirtual = ms->ullTotalPhys; ms->ullAvailVirtual = ms->ullAvailPhys;
    return TRUE;
}

/* ------------------------------------------------------------------ Toolhelp32 (the crash log lists the threads) */
#include "tlhelp32.h"
struct TfSnap { std::vector<pid_t> tids; size_t pos = 0; };
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD) {
    if (!(flags & TH32CS_SNAPTHREAD)) return nullptr;
    auto *sn = new TfSnap();
    DIR *d = opendir("/proc/self/task");
    if (d) { struct dirent *e; while ((e = readdir(d))) if (e->d_name[0] >= '0' && e->d_name[0] <= '9') sn->tids.push_back((pid_t)atoi(e->d_name)); closedir(d); }
    HANDLE h = hnew(tf_handle_s::H_FIND);
    h->find = sn;
    return h;
}
BOOL Thread32First(HANDLE h, LPTHREADENTRY32 te) {
    if (!h || h->kind != tf_handle_s::H_FIND || !h->find || !te || te->dwSize < sizeof *te) return FALSE;
    auto *sn = (TfSnap *)h->find;
    sn->pos = 0;
    return Thread32Next(h, te);
}
BOOL Thread32Next(HANDLE h, LPTHREADENTRY32 te) {
    if (!h || h->kind != tf_handle_s::H_FIND || !h->find || !te) return FALSE;
    auto *sn = (TfSnap *)h->find;
    if (sn->pos >= sn->tids.size()) return FALSE;
    DWORD sz = te->dwSize;
    memset(te, 0, sz);
    te->dwSize = sz;
    te->th32ThreadID = (DWORD)sn->tids[sn->pos++];
    te->th32OwnerProcessID = (DWORD)getpid();
    return TRUE;
}

/* ------------------------------------------------------------------ files */
static HANDLE file_open(const W &path, DWORD access, DWORD share, DWORD disp, DWORD flags) {
    S p = tf_w2u(path.c_str(), true);
    int f = 0;
    if (access & GENERIC_WRITE) f = (disp == OPEN_EXISTING) ? O_RDWR : O_WRONLY | (disp == CREATE_ALWAYS ? O_TRUNC : 0);
    else f = O_RDONLY;
    if (disp == CREATE_ALWAYS) f |= O_CREAT | O_TRUNC;
    else if (disp == CREATE_NEW) f |= O_CREAT | O_EXCL;
    else if (disp == OPEN_ALWAYS) f |= O_CREAT;
    int fd = open(p.c_str(), f | O_CLOEXEC, 0666);
    if (fd < 0) { g_lasterr = errno == ENOENT ? 2 : 5; return INVALID_HANDLE_VALUE; }
    if (flags & FILE_FLAG_SEQUENTIAL_SCAN) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    if (flags & FILE_FLAG_RANDOM_ACCESS) posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
    HANDLE h = hnew(tf_handle_s::H_FILE); h->fd = fd;
    return h;
}
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES, DWORD disp, DWORD flags, HANDLE) {
    return file_open(path ? path : L"", access, share, disp, flags);
}
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPVOID) {
    if (!h || h->fd < 0) { if (got) *got = 0; return FALSE; }
    ssize_t r = read(h->fd, buf, n);
    if (r < 0) { if (got) *got = 0; return FALSE; }
    if (got) *got = (DWORD)r;
    return TRUE;
}
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD got, LPVOID) {
    if (!h || h->fd < 0) { if (got) *got = 0; return FALSE; }
    ssize_t r = write(h->fd, buf, n);
    if (got) *got = (DWORD)(r < 0 ? 0 : r);
    return r >= 0;
}
DWORD SetFilePointer(HANDLE h, LONG dist, LONG *high, DWORD method) {
    if (!h || h->fd < 0) return INVALID_SET_FILE_POINTER;
    off_t d = high ? ((off_t)*high << 32) | (uint32_t)dist : dist;
    off_t r = lseek(h->fd, d, method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    if (r < 0) return INVALID_SET_FILE_POINTER;
    if (high) *high = (LONG)(r >> 32);
    return (DWORD)r;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LONGLONG *newpos, DWORD method) {
    if (!h || h->fd < 0) return FALSE;
    off_t r = lseek(h->fd, (off_t)dist.QuadPart, method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    if (r < 0) return FALSE;
    if (newpos) *newpos = r;
    return TRUE;
}
BOOL GetFileSizeEx(HANDLE h, LONGLONG *size) {
    if (!h || h->fd < 0) return FALSE;
    struct stat st;
    if (fstat(h->fd, &st) != 0) return FALSE;
    *size = st.st_size;
    return TRUE;
}
DWORD GetFileSize(HANDLE h, LPDWORD high) {
    LONGLONG s = 0;
    if (!GetFileSizeEx(h, &s)) return INVALID_FILE_ATTRIBUTES;
    if (high) *high = (DWORD)(s >> 32);
    return (DWORD)s;
}
BOOL FlushFileBuffers(HANDLE h) { if (!h || h->fd < 0) return FALSE; return fsync(h->fd) == 0; }
BOOL DeleteFileW(LPCWSTR p) { return unlink(tf_w2u(p, true).c_str()) == 0; }
BOOL DeleteFileA(LPCSTR p) { return unlink(p) == 0; }
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    S f = tf_w2u(from, true), t = tf_w2u(to, true);
    if (!(flags & MOVEFILE_REPLACE_EXISTING)) { struct stat st; if (lstat(t.c_str(), &st) == 0) { g_lasterr = 183; return FALSE; } }
    if (rename(f.c_str(), t.c_str()) == 0) return TRUE;
    /*fall back to copy+unlink across filesystems*/
    int in = open(f.c_str(), O_RDONLY), out = open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0) { if (in >= 0) close(in); if (out >= 0) close(out); return FALSE; }
    char buf[1 << 16]; ssize_t n; BOOL ok = TRUE;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, n) != n) { ok = FALSE; break; }
    close(in); close(out);
    if (ok) unlink(f.c_str());
    return ok;
}
BOOL MoveFileW(LPCWSTR from, LPCWSTR to) { return MoveFileExW(from, to, 0); }
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) { return MoveFileExW(tf_u2w(from ? from : "", true).c_str(), tf_u2w(to ? to : "", true).c_str(), flags); }
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists) {
    S f = tf_w2u(from, true), t = tf_w2u(to, true);
    if (fail_if_exists) { struct stat st; if (stat(t.c_str(), &st) == 0) { SetLastError(183); return FALSE; } }
    int in = open(f.c_str(), O_RDONLY), out = open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0) { if (in >= 0) close(in); if (out >= 0) close(out); SetLastError(2); return FALSE; }
    char buf[1 << 16]; ssize_t n; BOOL ok = TRUE;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, n) != n) { ok = FALSE; break; }
    close(in); close(out);
    return ok;
}
BOOL CreateDirectoryW(LPCWSTR p, LPSECURITY_ATTRIBUTES) { return mkdir(tf_w2u(p, true).c_str(), 0777) == 0 || errno == EEXIST; }
BOOL CreateDirectoryA(LPCSTR p, LPSECURITY_ATTRIBUTES) { return mkdir(p, 0777) == 0 || errno == EEXIST; }
BOOL RemoveDirectoryW(LPCWSTR p) { return rmdir(tf_w2u(p, true).c_str()) == 0; }
static DWORD stat_to_attrs(const S &p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
DWORD GetFileAttributesW(LPCWSTR p) { return stat_to_attrs(tf_w2u(p, true)); }
BOOL GetFileAttributesExW(LPCWSTR p, DWORD, LPVOID out_) {
    WIN32_FILE_ATTRIBUTE_DATA *out = (WIN32_FILE_ATTRIBUTE_DATA *)out_;
    S s = tf_w2u(p, true);
    struct stat st;
    if (stat(s.c_str(), &st) != 0) return FALSE;
    out->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    out->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); out->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}
DWORD GetFullPathNameW(LPCWSTR path, DWORD n, LPWSTR buf, LPWSTR *part) {
    W w(path);
    if (!w.empty() && w[0] != L'\\' && w[0] != L'/' && (w.size() < 2 || w[1] != L':')) {
        wchar_t cwd[4096];
        if (_wgetcwd(cwd, 4096)) { W base(cwd); while (!base.empty() && base.back() == L'\\') base.pop_back(); if (!base.empty()) w = base + L"\\" + w; }
    }
    S s = tf_w2u(w.c_str(), true);
    char resolved[8192];
    if (realpath(s.c_str(), resolved)) w = tf_u2w(resolved, true);
    else w = tf_u2w(s.c_str(), true);
    if (part) { size_t k = w.find_last_of(L'\\'); *part = k == W::npos ? nullptr : &buf[k + 1]; }
    if (w.size() + 1 > n) return (DWORD)w.size() + 1;
    wcscpy(buf, w.c_str());
    return (DWORD)w.size();
}
DWORD GetCurrentDirectoryW(DWORD n, LPWSTR buf) {
    wchar_t cwd[4096];
    if (!_wgetcwd(cwd, 4096)) return 0;
    W w(cwd);
    if (w.size() + 1 > n) return w.size() + 1;
    wcscpy(buf, w.c_str());
    return (DWORD)w.size();
}
BOOL SetCurrentDirectoryW(LPCWSTR dir) { return chdir(tf_w2u(dir, true).c_str()) == 0; }
BOOL GetDiskFreeSpaceExW(LPCWSTR, LPVOID, LPVOID, LPVOID) { return TRUE; }

/* ------------------------------------------------------------------ FindFirstFile */
struct FindState { DIR *dir = nullptr; S pattern; S dirpart; };
static bool wc_match(const S &pat, const S &name) {
    size_t p = 0, n = 0, star = S::npos, sn = 0;
    while (n < name.size()) {
        if (p < pat.size() && (pat[p] == '?' || tolower((unsigned char)pat[p]) == tolower((unsigned char)name[n]))) { p++; n++; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; sn = n; }
        else if (star != S::npos) { p = star + 1; n = ++sn; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}
static BOOL find_fill(HANDLE h, LPWIN32_FIND_DATAW fd) {
    FindState *fs = (FindState *)h->find;
    if (!fs->dir) return FALSE;
    struct dirent *e;
    while ((e = readdir(fs->dir))) {
        if (!wc_match(fs->pattern, e->d_name)) continue;
        memset(fd, 0, sizeof *fd);
        W w = tf_u2w(e->d_name, false);
        wcscpy(fd->cFileName, w.c_str());
        struct stat st;
        S full = fs->dirpart + "/" + e->d_name;
        if (stat(full.c_str(), &st) == 0) {
            fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            fd->nFileSizeLow = (DWORD)st.st_size; fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        }
        return TRUE;
    }
    return FALSE;
}
HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW fd) {
    S s = tf_w2u(pattern, true);
    size_t k = s.find_last_of('/');
    S dirpart = k == S::npos ? "." : (k == 0 ? "/" : s.substr(0, k));
    S pat = k == S::npos ? s : s.substr(k + 1);
    DIR *d = opendir(dirpart.c_str());
    if (!d) { g_lasterr = 2; return INVALID_HANDLE_VALUE; }
    HANDLE h = hnew(tf_handle_s::H_FIND);
    h->find = new FindState{d, pat, dirpart};
    if (!find_fill(h, fd)) { FindClose(h); SetLastError(18); return INVALID_HANDLE_VALUE; }
    return h;
}
BOOL FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW fd) {
    if (!h || h->kind != tf_handle_s::H_FIND) return FALSE;
    return find_fill(h, fd);
}
BOOL FindClose(HANDLE h) {
    if (!h || h->kind != tf_handle_s::H_FIND) return FALSE;
    FindState *fs = (FindState *)h->find;
    if (fs->dir) closedir(fs->dir);
    delete fs;
    delete h;
    return TRUE;
}

/* ------------------------------------------------------------------ INI (Windows semantics) */
static S ini_key(const W &w) { return tf_w2u(w.c_str(), false); }
static void ini_rstrip(S &s) { while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == 9)) s.pop_back(); }
static S read_file_str(const W &path) {
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return {};
    S s; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
static std::vector<S> split_lines(S s) {
    std::vector<S> v; size_t p;
    while ((p = s.find('\n')) != S::npos) { v.push_back(s.substr(0, p)); s.erase(0, p + 1); }
    if (!s.empty()) v.push_back(s);
    return v;
}
UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, INT def, LPCWSTR file) {
    DWORD n = GetPrivateProfileStringW(section, key, L"", nullptr, 0, file);
    std::wstring buf(n + 1, L'\0');
    GetPrivateProfileStringW(section, key, L"", &buf[0], (DWORD)buf.size(), file);
    S v = tf_w2u(buf.c_str(), false);
    return v.empty() ? (UINT)def : (UINT)strtol(v.c_str(), nullptr, 0);
}
DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size, LPCWSTR file) {
    S want_sec = ini_key(section), want_key = ini_key(key);
    std::vector<S> lines = split_lines(read_file_str(file ? file : L""));
    bool insec = false;
    for (auto &l : lines) {
        ini_rstrip(l);
        S t = l;
        while (!t.empty() && (t.front() == ' ' || t.front() == 9)) t.erase(t.begin());
        if (!t.empty() && t.front() == '[') { size_t e = t.find(']'); insec = strcasecmp(t.substr(1, e - 1).c_str(), want_sec.c_str()) == 0; continue; }
        if (!insec || t.empty() || t[0] == '#' || t[0] == ';') continue;
        size_t eq = t.find('=');
        if (eq == S::npos) continue;
        S k = t.substr(0, eq), v = t.substr(eq + 1);
        ini_rstrip(k); while (!k.empty() && (k.back() == ' ' || k.back() == 9)) k.pop_back();
        if (strcasecmp(k.c_str(), want_key.c_str()) != 0) continue;
        W wv = tf_u2w(v.c_str(), false);
        if (!out) return (DWORD)wv.size();
        DWORD n = std::min((DWORD)wv.size(), size - 1);
        memcpy(out, wv.data(), (size_t)n * sizeof(wchar_t)); out[n] = 0;
        return n;
    }
    if (def) { W wd(def); if (!out) return (DWORD)wd.size(); DWORD n = std::min((DWORD)wd.size(), size - 1); memcpy(out, wd.data(), (size_t)n * sizeof(wchar_t)); out[n] = 0; return n; }
    if (out && size) out[0] = 0;
    return 0;
}
BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file) {
    S want_sec = ini_key(section), want_key = ini_key(key);
    S path = tf_w2u(file ? file : L"", true);
    std::vector<S> lines = split_lines(read_file_str(file ? file : L""));
    bool insec = false; size_t ins_at = S::npos;
    for (size_t i = 0; i < lines.size(); i++) {
        S t = lines[i]; ini_rstrip(t);
        while (!t.empty() && (t.front() == ' ' || t.front() == 9)) t.erase(t.begin());
        if (!t.empty() && t.front() == '[') { size_t e = t.find(']'); insec = strcasecmp(t.substr(1, e - 1).c_str(), want_sec.c_str()) == 0; if (insec) ins_at = i + 1; continue; }
        if (!insec || t.empty() || t[0] == '#' || t[0] == ';') continue;
        size_t eq = t.find('=');
        if (eq == S::npos) continue;
        S k = t.substr(0, eq); ini_rstrip(k); while (!k.empty() && (k.back() == ' ' || k.back() == 9)) k.pop_back();
        if (strcasecmp(k.c_str(), want_key.c_str()) == 0) {
            if (!value) lines.erase(lines.begin() + (long)i);
            else lines[i] = want_key + "=" + tf_w2u(value, false);
            goto write;
        }
    }
    {   /*key not in the section (or no section): append under it*/
        if (ins_at == S::npos) { lines.push_back("[" + want_sec + "]"); ins_at = lines.size(); }
        lines.insert(lines.begin() + (long)ins_at, want_key + "=" + tf_w2u(value ? value : L"", false));
    }
write:;
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return FALSE;
    for (auto &l : lines) { fputs(l.c_str(), f); fputc('\n', f); }
    fclose(f);
    return TRUE;
}

/* ------------------------------------------------------------------ modules (dlopen with the project's DLL names) */
#include <dlfcn.h>
static std::vector<void *> g_dsos;
static std::map<void *, S> g_dso_names;
/*fake modules implemented by the shim itself (xinput/uxtheme/avrt): tagged pointers*/
extern "C" void *tf_shim_proc(void *module, const char *name);
static void *const kFakeXInput = (void *)0x11, *const kFakeUxTheme = (void *)0x12, *const kFakeAvrt = (void *)0x13, *const kFakeKernel32 = (void *)0x14;
static bool is_fake_module(void *m) { return m == kFakeXInput || m == kFakeUxTheme || m == kFakeAvrt || m == kFakeKernel32; }
static S dll_to_so(const char *name) {
    S n = name;
    /*libtcc.dll -> libtcc.so (the JIT loads it by this name)*/
    if (n == "libtcc.dll" || n == "libtcc.so") return "libtcc.so";
    /*avcodec-61.dll -> libavcodec.so.61 (FFmpeg, loaded by number)*/
    size_t dash = n.rfind('-');
    if (n.size() > 4 && n.substr(n.size() - 4) == ".dll" && dash != S::npos && n.find_first_not_of("0123456789", dash + 1) == n.size() - 4)
        return "lib" + n.substr(0, dash) + ".so." + n.substr(dash + 1, n.size() - dash - 5);
    if (n.size() > 4 && n.substr(n.size() - 4) == ".dll") return n.substr(0, n.size() - 4) + ".so";
    return n;
}
static HMODULE dlopen_try(const S &name, const char *explicit_dir) {
    std::vector<S> tries;
    if (explicit_dir) tries.push_back(S(explicit_dir) + name);
    tries.push_back(name);
    const char *env = getenv("TF_LIBDIR");
    if (env) tries.push_back(S(env) + "/" + name);
    tries.push_back("/usr/lib/x86_64-linux-gnu/" + name);
    tries.push_back("/usr/local/lib/" + name);
    for (auto &t : tries) { void *m = dlopen(t.c_str(), RTLD_NOW | RTLD_GLOBAL); if (m) { g_dsos.push_back(m); g_dso_names[m] = t; return (HMODULE)m; } }
    return nullptr;
}
HMODULE LoadLibraryA(LPCSTR name) {
    if (!name) return nullptr;
    S n = name;
    if (n.find("xinput") == 0) return (HMODULE)kFakeXInput;
    if (n == "uxtheme.dll") return (HMODULE)kFakeUxTheme;
    if (n == "avrt.dll") return (HMODULE)kFakeAvrt;
    S so = dll_to_so(name);
    if (HMODULE m = dlopen_try(so, nullptr)) return m;
    /*the MSYS2 fallback path in ffmpeg_dec.cpp: C:/msys64/... -> just try the plain name*/
    if (n.find("msys64") != S::npos) return dlopen_try(dll_to_so(n.substr(n.find_last_of('/') + 1).c_str()).c_str(), nullptr);
    return nullptr;
}
HMODULE LoadLibraryW(LPCWSTR name) {
    if (!name) return nullptr;
    S n = tf_w2u(name, true);
    if (n.find("xinput") == 0) return (HMODULE)kFakeXInput;
    if (n == "uxtheme.dll") return (HMODULE)kFakeUxTheme;
    if (n == "avrt.dll") return (HMODULE)kFakeAvrt;
    if (n == "kernel32.dll") return (HMODULE)kFakeKernel32;
    return dlopen_try(dll_to_so(n.c_str()), nullptr);
}
HMODULE LoadLibraryExA(LPCSTR name, HANDLE, DWORD) { return LoadLibraryA(name); }
HMODULE LoadLibraryExW(LPCWSTR name, HANDLE, DWORD) { return LoadLibraryW(name); }
FARPROC GetProcAddress(HMODULE m, LPCSTR name) {
    if (!m || !name) return nullptr;
    if (is_fake_module(m)) return (FARPROC)tf_shim_proc(m, name);
    void *p = dlsym(m, name);
    if (p) return (FARPROC)p;
    return (FARPROC)tf_shim_proc(m, name);   /* the shim's own fake exports (xinput, avrt, uxtheme) */
}
BOOL FreeLibrary(HMODULE) { return TRUE; }   /*kept open: the JIT's code lives in libtcc's memory*/
HMODULE GetModuleHandleA(LPCSTR name) {
    if (!name) {   /* what every GetModuleHandle(nullptr) caller wants is the exe's LOAD ADDRESS:
                      the crash handler's exe+offset, the profiler's rebasing, RegisterClass. A dlopen
                      handle is a link_map pointer, not the base: a real crash printed
                      "exe+0xffffd61392606811" and an addr2line hint that could not work. */
        static void *self;
        if (!self) {
            Dl_info di;
            if (dladdr((void *)&GetModuleHandleA, &di) && di.dli_fbase) self = di.dli_fbase;   /* this function lives in the exe */
            else self = dlopen(nullptr, RTLD_NOW);
        }
        return (HMODULE)self;
    }
    S so = dll_to_so(name);
    if (void *m = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD)) return (HMODULE)m;
    return nullptr;
}
HMODULE GetModuleHandleW(LPCWSTR name) {
    if (!name) return GetModuleHandleA(nullptr);
    S n = tf_w2u(name, true);
    if (n == "kernel32.dll") return (HMODULE)kFakeKernel32;   /* the shim exports K32GetProcessMemoryInfo from it */
    if (n.find("xinput") == 0) return (HMODULE)kFakeXInput;
    if (n == "uxtheme.dll") return (HMODULE)kFakeUxTheme;
    if (n == "avrt.dll") return (HMODULE)kFakeAvrt;
    return GetModuleHandleA(n.c_str());
}
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *mod) {
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) && name) {
        /*which mapping does the address fall into? (hle_ge prints the module of a RIP)*/
        uintptr_t a = (uintptr_t)name;
        FILE *f = fopen("/proc/self/maps", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                uintptr_t lo, hi; char perms[8], path[384] = "";
                if (sscanf(line, "%lx-%lx %7s %*x %*x:%*x %*u %383s", &lo, &hi, perms, path) >= 3 && a >= lo && a < hi) {
                    fclose(f);
                    void *m = nullptr;
                    if (path[0] == '/') m = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
                    if (!m) m = dlopen(nullptr, RTLD_NOW);
                    *mod = (HMODULE)m;
                    return TRUE;
                }
            }
            fclose(f);
        }
        *mod = GetModuleHandleA(nullptr);
        return TRUE;
    }
    *mod = GetModuleHandleA(name);
    return *mod != nullptr;
}
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n) {
    if (!m) { ssize_t r = readlink("/proc/self/exe", buf, n - 1); if (r <= 0) return 0; buf[r] = 0; return (DWORD)r; }
    auto it = g_dso_names.find((void *)m);
    if (it == g_dso_names.end()) { buf[0] = 0; return 0; }
    snprintf(buf, n, "%s", it->second.c_str());
    return (DWORD)strlen(buf);
}
DWORD GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD n) {
    char a[4096];
    DWORD r = GetModuleFileNameA(m, a, sizeof a);
    if (!r) return 0;
    W w = tf_u2w(a, true);
    if (w.size() + 1 > n) return 0;
    wcscpy(buf, w.c_str());
    return (DWORD)w.size();
}

/* ------------------------------------------------------------------ SEH emulation (SIGSEGV/SIGBUS) */
static std::vector<PVECTORED_EXCEPTION_HANDLER> g_vectored;
static PTOP_LEVEL_EXCEPTION_FILTER g_unhandled;
static thread_local bool g_in_handler;
PVECTORED_EXCEPTION_HANDLER AddVectoredExceptionHandler(UINT, PVECTORED_EXCEPTION_HANDLER h) { g_vectored.push_back(h); return h; }
ULONG RemoveVectoredExceptionHandler(PVECTORED_EXCEPTION_HANDLER h) { for (size_t i = 0; i < g_vectored.size(); i++) if (g_vectored[i] == h) { g_vectored.erase(g_vectored.begin() + (long)i); return 1; } return 0; }
PTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(PTOP_LEVEL_EXCEPTION_FILTER f) { PTOP_LEVEL_EXCEPTION_FILTER o = g_unhandled; g_unhandled = f; return o; }

static void tf_shim_signal(int sig, siginfo_t *si, void *uctx) {
    if (g_in_handler) {   /*fault inside the handler: give up (like Windows' second-chance death)*/
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_in_handler = true;
    ucontext_t *uc = (ucontext_t *)uctx;
    EXCEPTION_RECORD rec{};
    CONTEXT ctx{};
    rec.ExceptionCode = sig == SIGBUS ? (UINT)EXCEPTION_ACCESS_VIOLATION : (sig == SIGFPE ? (UINT)EXCEPTION_INT_DIVIDE_BY_ZERO : (UINT)EXCEPTION_ACCESS_VIOLATION);
    rec.ExceptionAddress = (LPVOID)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    rec.NumberParameters = 2;
    rec.ExceptionInformation[0] = 1;   /*assume write; x86 REG_ERR bit 1 says read/write*/
#ifdef REG_ERR
    if (!((uc->uc_mcontext.gregs[REG_ERR]) & 2)) rec.ExceptionInformation[0] = 0;
#endif
    rec.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
    ctx.Rip = (DWORD64)uc->uc_mcontext.gregs[REG_RIP];
    EXCEPTION_POINTERS ep{&rec, &ctx};
    for (auto h : g_vectored) {   /*vram_fault: fixes the page and resumes*/
        if (h(&ep) == (LONG)EXCEPTION_CONTINUE_EXECUTION) { g_in_handler = false; return; }
    }
    if (g_unhandled) {
        LONG r = g_unhandled(&ep);
        if (r == (LONG)EXCEPTION_CONTINUE_EXECUTION) { g_in_handler = false; return; }
    }
    /*not handled: die like an unhandled Windows exception*/
    fflush(nullptr);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void tf_shim_install_signals(void) {
    struct sigaction sa{};
    sa.sa_sigaction = tf_shim_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    struct sigaction sc{};
    sc.sa_handler = sigchld;
    sc.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction(SIGCHLD, &sc, nullptr);
    static std::vector<char> main_altstack(1 << 17);
    stack_t ss{}; ss.ss_sp = main_altstack.data(); ss.ss_size = main_altstack.size();
    sigaltstack(&ss, nullptr);
    /*the signal actions must run on the alt stack too*/
    struct sigaction sa2{};
    sa2.sa_sigaction = tf_shim_signal;
    sa2.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa2, nullptr);
    sigaction(SIGBUS, &sa2, nullptr);
    sigaction(SIGFPE, &sa2, nullptr);
}
extern "C" void tf_shim_early_init(void);   /* called as a constructor: signals before anything else */
__attribute__((constructor)) static void shim_ctor(void) { tf_shim_early_init(); }
extern "C" void tf_shim_early_init(void) { static bool once; if (once) return; once = true; tf_shim_install_signals(); }

/* ------------------------------------------------------------------ exit */
void ExitProcess(UINT code) {
    /*the comment in host.cpp: exit() would run destructors of joinable std::threads -> std::terminate.
     * Flush what we can, then stop the process the hard way, like ExitProcess does.*/
    fflush(nullptr);
    struct timespec ts{0, 20000000};
    nanosleep(&ts, nullptr);   /*one frame for the log tee thread*/
    _exit((int)code);
}

/* ------------------------------------------------------------------ psapi (session log: process memory) */
#include "psapi.h"
extern "C" BOOL tf_k32_get_process_memory_info(HANDLE h, void *out, DWORD size) {
    PROCESS_MEMORY_COUNTERS_EX *mc = (PROCESS_MEMORY_COUNTERS_EX *)out;
    if (!mc || size < sizeof(PROCESS_MEMORY_COUNTERS)) return FALSE;
    struct rusage ru;
    DWORD cb = mc->cb;
    memset(mc, 0, cb);
    mc->cb = cb;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        mc->PageFaultCount = (DWORD)ru.ru_minflt;
        mc->WorkingSetSize = (SIZE_T)ru.ru_maxrss * 1024;   /* Linux: KiB */
        mc->PeakWorkingSetSize = mc->WorkingSetSize;
        if (cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) ((PROCESS_MEMORY_COUNTERS_EX *)mc)->PrivateUsage = mc->WorkingSetSize;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ system info (log header) */
void GetNativeSystemInfo(SYSTEM_INFO *si) {
    memset(si, 0, sizeof *si);
    si->dwNumberOfProcessors = (DWORD)std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
}
BOOL GetSystemPowerStatus(SYSTEM_POWER_STATUS *ps) {
    memset(ps, 0, sizeof *ps);
    ps->ACLineStatus = 1;   /* no battery info: assume plugged */
    ps->BatteryFlag = 128;
    ps->BatteryLifePercent = 255;
    return FALSE;
}
LSTATUS RegGetValueA(HKEY, LPCSTR subkey, LPCSTR value, DWORD, void *, void *data, DWORD *size) {
    /*the CPU name: the one registry read the log header makes (answer from /proc/cpuinfo)*/
    if (data && size && *size > 8 && subkey && value && !strcmp(value, "ProcessorNameString") &&
        strstr(subkey, "CentralProcessor")) {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "model name", 10) && strchr(line, ':')) {
                    const char *v = strchr(line, ':') + 1;
                    while (*v == ' ') v++;
                    size_t n = strlen(v);
                    while (n && (v[n - 1] == '\n' || v[n - 1] == ' ')) n--;
                    if (n >= (size_t)*size) n = (size_t)*size - 1;
                    memcpy(data, v, n);
                    ((char *)data)[n] = 0;
                    *size = (DWORD)n;
                    fclose(f);
                    return 0;
                }
            fclose(f);
        }
        return 2;
    }
    return 2;   /* not found: the log falls back to "?" */
}

/*fd plumbing of the log tee (io.h macros call these)*/
int tf_shim_handle_fd(HANDLE h) {
    if (!h) return -1;
    switch (h->kind) {
    case tf_handle_s::H_FILE: case tf_handle_s::H_PIPE_RD: case tf_handle_s::H_PIPE_WR: return h->fd;
    default: return -1;
    }
}
int tf_shim_open_osfhandle(HANDLE h, int) { return tf_shim_handle_fd(h); }

/* ------------------------------------------------------------------ process/thread times (resource monitor) */
void GetSystemInfo(SYSTEM_INFO *si) { GetNativeSystemInfo(si); }
static void times_to_ft(struct timespec &ts, FILETIME *ft) {
    uint64_t t = (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100ull + 116444736000000000ull;
    ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32);
}
void GetSystemTimeAsFileTime(FILETIME *ft) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    times_to_ft(ts, ft);
}
BOOL GetProcessTimes(HANDLE, FILETIME *creat, FILETIME *exit_, FILETIME *kernel, FILETIME *user) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return FALSE;
    struct timespec k{(time_t)ru.ru_stime.tv_sec, (long)ru.ru_stime.tv_usec * 1000}, u{(time_t)ru.ru_utime.tv_sec, (long)ru.ru_utime.tv_usec * 1000};
    if (kernel) times_to_ft(k, kernel);
    if (user) times_to_ft(u, user);
    if (creat) GetSystemTimeAsFileTime(creat);
    if (exit_) { exit_->dwLowDateTime = exit_->dwHighDateTime = 0; }
    return TRUE;
}
HANDLE OpenThread(DWORD, BOOL, DWORD tid) {
    HANDLE h = hnew(tf_handle_s::H_FILE);   /* reuse: fd = the tid */
    h->fd = (int)tid;
    return h;
}
BOOL GetThreadTimes(HANDLE h, FILETIME *creat, FILETIME *exit_, FILETIME *kernel, FILETIME *user) {
    if (!h || h->kind != tf_handle_s::H_FILE) return FALSE;
    clockid_t cid;
    if (pthread_getcpuclockid(pthread_self(), &cid) == 0) {   /* only meaningful for the calling thread; others read 0 */
        struct timespec ts;
        if (clock_gettime(cid, &ts) == 0) { times_to_ft(ts, kernel); if (user) *user = *kernel; }
    }
    if (creat) GetSystemTimeAsFileTime(creat);
    if (exit_) { exit_->dwLowDateTime = exit_->dwHighDateTime = 0; }
    return TRUE;
}

/* ------------------------------------------------------------------ WaitOnAddress family (futex) */
#include <linux/futex.h>
#include <sys/syscall.h>
static long futex_wait(void *addr, uint32_t expected) { return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, nullptr); }
static long futex_wake(void *addr, int n) { return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, n, nullptr); }
BOOL WaitOnAddress(void volatile *addr, void *compare, SIZE_T size, DWORD ms) {
    if (size != 4 && size != 8) { Sleep(ms); return FALSE; }
    uint64_t deadline = ms == INFINITE ? ~0ull : now_ns() + (uint64_t)ms * 1000000ull;
    for (;;) {
        uint64_t cur = size == 4 ? *(volatile uint32_t *)addr : *(volatile uint64_t *)addr;
        uint64_t want = size == 4 ? *(uint32_t *)compare : *(uint64_t *)compare;
        if (cur != want) return TRUE;
        uint64_t left = deadline == ~0ull ? 0 : (deadline > now_ns() ? deadline - now_ns() : 1);
        struct timespec ts{0, (long)left};
        syscall(SYS_futex, (void *)addr, FUTEX_WAIT_PRIVATE, (uint32_t)(size == 4 ? cur : (uint32_t)(cur ^ (cur >> 32))), left ? &ts : nullptr);
        if (deadline != ~0ull && now_ns() >= deadline) return FALSE;
    }
}
void WakeByAddressAll(void *addr) { futex_wake(addr, INT_MAX); }
void WakeByAddressSingle(void *addr) { futex_wake(addr, 1); }
void YieldProcessor(void) { __builtin_ia32_pause(); }
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR n, DWORD, DWORD) { return CreateWaitableTimerW(sa, FALSE, n); }

/* ------------------------------------------------------------------ critical sections + 64-bit interlocked */
void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init((pthread_mutex_t *)cs, &a);
    pthread_mutexattr_destroy(&a);
}
void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy((pthread_mutex_t *)cs); }
void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock((pthread_mutex_t *)cs); }
void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock((pthread_mutex_t *)cs); }
BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs) { return pthread_mutex_trylock((pthread_mutex_t *)cs) == 0; }
LONG64 InterlockedAdd64(LONG64 volatile *dst, LONG64 v) { return __atomic_fetch_add(dst, v, __ATOMIC_SEQ_CST); }
LONG64 InterlockedIncrement64(LONG64 volatile *dst) { return __atomic_add_fetch(dst, 1, __ATOMIC_SEQ_CST); }
LONG64 InterlockedDecrement64(LONG64 volatile *dst) { return __atomic_sub_fetch(dst, 1, __ATOMIC_SEQ_CST); }
LONG64 InterlockedCompareExchange64(LONG64 volatile *dst, LONG64 v, LONG64 expected) { return __atomic_compare_exchange_n(dst, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? expected : *dst; }

/* ------------------------------------------------------------------ A-variants of the file API */
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd) {
    WIN32_FIND_DATAW wfd;
    W pat = tf_u2w(pattern ? pattern : "", true);
    HANDLE h = FindFirstFileW(pat.c_str(), &wfd);
    if (h == INVALID_HANDLE_VALUE) return h;
    memset(fd, 0, sizeof *fd);
    fd->dwFileAttributes = wfd.dwFileAttributes;
    fd->nFileSizeHigh = wfd.nFileSizeHigh; fd->nFileSizeLow = wfd.nFileSizeLow;
    S n = tf_w2u(wfd.cFileName, false);
    strncpy(fd->cFileName, n.c_str(), sizeof fd->cFileName - 1);
    return h;
}
BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd) {
    WIN32_FIND_DATAW wfd;
    if (!FindNextFileW(h, &wfd)) return FALSE;
    memset(fd, 0, sizeof *fd);
    fd->dwFileAttributes = wfd.dwFileAttributes;
    fd->nFileSizeHigh = wfd.nFileSizeHigh; fd->nFileSizeLow = wfd.nFileSizeLow;
    S n = tf_w2u(wfd.cFileName, false);
    strncpy(fd->cFileName, n.c_str(), sizeof fd->cFileName - 1);
    return TRUE;
}
DWORD GetFileAttributesExA(LPCSTR path, DWORD which, LPVOID out) {
    return GetFileAttributesExW(tf_u2w(path ? path : "", true).c_str(), which, out);
}
BOOL RemoveDirectoryA(LPCSTR p) { return rmdir(p ? p : "") == 0; }

/* ------------------------------------------------------------------ thread context (sampling profiler: returns the PC) */
#include <sys/ptrace.h>
#include <sys/user.h>
#include <signal.h>
DWORD SuspendThread(HANDLE h) {
    if (!h) return (DWORD)-1;
    if (h->kind != tf_handle_s::H_FILE) return (DWORD)-1;
    if (ptrace(PTRACE_SEIZE, h->fd, nullptr, nullptr) != 0 && errno != ESRCH) return (DWORD)-1;
    if (ptrace(PTRACE_INTERRUPT, h->fd, nullptr, nullptr) != 0 && errno != ESRCH) return (DWORD)-1;
    return 0;
}
DWORD ResumeThread(HANDLE h) {
    if (!h || h->kind != tf_handle_s::H_FILE) return (DWORD)-1;
    ptrace(PTRACE_DETACH, h->fd, nullptr, nullptr);
    return 0;
}
BOOL GetThreadContext(HANDLE h, CONTEXT *cx) {
    if (!h || !cx || h->kind != tf_handle_s::H_FILE) return FALSE;
    memset(cx, 0, sizeof *cx);
#ifdef __x86_64__
    user_regs_struct regs;   /* RIP via PTRACE_GETREGS: the simplest regset to fetch */
    if (ptrace(PTRACE_GETREGS, h->fd, nullptr, &regs) != 0) return FALSE;
    cx->Rip = regs.rip;
    return TRUE;
#else
    (void)cx;
    return FALSE;
#endif
}
/*duplicate handle: the profiler needs the thread handle; the shim's OpenThread already gives a live handle*/
BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, HANDLE *out, DWORD, BOOL, DWORD) {
    if (!out) return FALSE;
    *out = src;
    return TRUE;
}

/* ------------------------------------------------------------------ display settings (log: the monitor's mode) */
extern "C" int tf_shim_screen_w(void), tf_shim_screen_h(void);   /* shim_user.cpp (SDL) */
BOOL EnumDisplaySettingsW(LPCWSTR, DWORD, DEVMODEW *dm) {
    memset(dm, 0, sizeof *dm);
    dm->dmSize = sizeof *dm;
    dm->dmPelsWidth = (DWORD)tf_shim_screen_w();
    dm->dmPelsHeight = (DWORD)tf_shim_screen_h();
    dm->dmDisplayFrequency = 60;
    return TRUE;
}
BOOL EnumDisplaySettingsA(LPCSTR d, DWORD m, DEVMODEW *dm) { return EnumDisplaySettingsW(d ? tf_u2w(d, true).c_str() : nullptr, m, dm); }

/* ------------------------------------------------------------------ launcher odds and ends */
int IntersectClipRect(HDC, int, int, int, int) { return 1; }   /* clip is already the canvas bounds */
BOOL SetBrushOrgEx(HDC, int, int, LPPOINT) { return TRUE; }
extern "C" DWORD tf_shim_dlg_item(HWND, int);   /* shim_user.cpp */
HWND GetDlgItem(HWND dlg, int id) { return (HWND)(uintptr_t)tf_shim_dlg_item(dlg, id); }
HICON LoadIconW(HINSTANCE, LPCWSTR) { return nullptr; }
BOOL EnumDisplayDevicesW(LPCWSTR, DWORD, DISPLAY_DEVICEW *dd, DWORD) {
    if (!dd) return FALSE;
    memset(dd, 0, sizeof *dd);
    dd->cb = sizeof *dd;
    wcscpy(dd->DeviceString, L"Display");
    return FALSE;   /* the launcher lists devices to label the resolution; one generic entry is enough */
}
