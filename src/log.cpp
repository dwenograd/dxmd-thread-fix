#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace dtf {

static CRITICAL_SECTION g_lock;
static bool             g_lock_inited = false;
static wchar_t          g_log_path[MAX_PATH] = {0};
static int              g_level = 1;
static bool             g_inited = false;

static void compute_log_path(HMODULE self) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        g_log_path[0] = 0;
        return;
    }
    // Strip filename, append our log name
    for (DWORD i = n; i > 0; --i) {
        if (buf[i - 1] == L'\\' || buf[i - 1] == L'/') {
            buf[i] = 0;
            break;
        }
    }
    wcscpy_s(g_log_path, MAX_PATH, buf);
    wcscat_s(g_log_path, MAX_PATH, L"dxmd-thread-fix.log");
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

void log_init_deferred(HMODULE self) {
    if (g_inited) return;
    InitializeCriticalSection(&g_lock);
    g_lock_inited = true;
    g_level = 0;
    compute_log_path(self);
    g_inited = true;
    // Do NOT open the file yet; wait until config tells us LogLevel > 0.
}

void log_open() {
    if (!g_inited) return;
    if (g_log_path[0] == 0) return;
    // Truncate on every fresh process so the log is a record of *this* run.
    HANDLE h = CreateFileW(g_log_path, GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

void log_shutdown() {
    if (!g_inited) return;
    if (g_lock_inited) {
        DeleteCriticalSection(&g_lock);
        g_lock_inited = false;
    }
    g_inited = false;
}

int log_level() { return g_level; }
void log_set_level(int level) {
    const int prev = g_level;
    g_level = level;
    // If we were silent and now want to log, open the file lazily.
    if (prev == 0 && level > 0) log_open();
}

void log_line(const char* fmt, ...) {
    if (!g_inited || g_level <= 0) return;
    if (g_log_path[0] == 0) return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    // _vsnprintf_s returns -1 on truncation with _TRUNCATE, but still
    // writes a null-terminated truncated string. Recover the length so
    // long lines aren't silently dropped.
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

bool first_hit(LONG volatile* flag) {
    return InterlockedCompareExchange(flag, 1, 0) == 0;
}

} // namespace dtf
