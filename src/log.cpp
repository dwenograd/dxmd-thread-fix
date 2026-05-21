// log.cpp - file logger described in log.h.

#include "log.h"
#include "path_util.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace dtf {

static CRITICAL_SECTION g_lock;
static bool             g_lock_inited = false;
static wchar_t*         g_log_path = nullptr;   // heap-allocated; can be long
static int              g_level = 1;
static bool             g_inited = false;

static void compute_log_path(HMODULE self) {
    free_wstr(g_log_path);
    g_log_path = nullptr;

    wchar_t* dir = get_module_dir(self);
    if (!dir) return;

    static const wchar_t kLogName[] = L"dxmd-thread-fix.log";
    size_t dirLen = 0; while (dir[dirLen]) ++dirLen;
    size_t pathLen = dirLen + (sizeof(kLogName) / sizeof(wchar_t));  // includes null
    g_log_path = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, pathLen * sizeof(wchar_t)));
    if (!g_log_path) { free_wstr(dir); return; }
    for (size_t i = 0; i < dirLen; ++i) g_log_path[i] = dir[i];
    for (size_t i = 0; ; ++i) {
        g_log_path[dirLen + i] = kLogName[i];
        if (kLogName[i] == 0) break;
    }
    free_wstr(dir);
}

void log_init(HMODULE self, int level) {
    if (g_inited) return;
    InitializeCriticalSection(&g_lock);
    g_lock_inited = true;
    g_level = level;
    compute_log_path(self);
    g_inited = true;
    if (level > 0) log_open();
}

// Records path but doesn't open the file. Deferring the open until
// log_set_level() means LogLevel=0 leaves no log file on disk at all
// (a trust signal — users can verify the DLL is silent).
void log_init_deferred(HMODULE self) {
    if (g_inited) return;
    InitializeCriticalSection(&g_lock);
    g_lock_inited = true;
    g_level = 0;
    compute_log_path(self);
    g_inited = true;
}

// Truncating open (CREATE_ALWAYS): each game launch starts a fresh log.
// Permissive sharing flags here match log_line() so truncation succeeds
// even when the prior session's file is open in an editor.
void log_open() {
    if (!g_inited) return;
    if (!g_log_path || g_log_path[0] == 0) return;
    HANDLE h = CreateFileW(g_log_path, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

void log_shutdown() {
    if (!g_inited) return;
    if (g_lock_inited) {
        DeleteCriticalSection(&g_lock);
        g_lock_inited = false;
    }
    free_wstr(g_log_path);
    g_log_path = nullptr;
    g_inited = false;
}

int log_level() { return g_level; }
void log_set_level(int level) {
    const int prev = g_level;
    g_level = level;
    if (prev == 0 && level > 0) log_open();
}

void log_line(const char* fmt, ...) {
    if (!g_inited || g_level <= 0) return;
    if (!g_log_path || g_log_path[0] == 0) return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    // _vsnprintf_s returns -1 on truncation but still null-terminates.
    if (n < 0) {
        msg[sizeof(msg) - 1] = '\0';
        n = static_cast<int>(strlen(msg));
    }
    if (n <= 0) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[64];
    int pn = _snprintf_s(prefix, sizeof(prefix), _TRUNCATE,
                         "[%02u:%02u:%02u.%03u] ",
                         st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (pn <= 0) return;

    EnterCriticalSection(&g_lock);
    // Open-write-close per line so each line is committed to the OS
    // file handle before the next call — important because the whole
    // point of this DLL is that DXMD crashes, and we want the last
    // logged lines (FIX STATUS, last hook call) to survive. Permissive
    // sharing lets the user tail the log while the game is running.
    // OPEN_ALWAYS covers the case where LogLevel was raised 0 → 1
    // mid-attach before log_open ran.
    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, prefix, static_cast<DWORD>(pn), &written, nullptr);
        WriteFile(h, msg,    static_cast<DWORD>(n),  &written, nullptr);
        WriteFile(h, "\r\n", 2, &written, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_lock);
}

// Returns true exactly once per `flag` via atomic CAS. Used to throttle
// LogLevel=1 output so each hooked API logs only on its first call.
bool first_hit(LONG volatile* flag) {
    return InterlockedCompareExchange(flag, 1, 0) == 0;
}

} // namespace dtf
