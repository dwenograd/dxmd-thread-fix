// log.cpp - implementation of the file logger described in log.h.
//
// Lifecycle (driven by dllmain.cpp's attach()):
//   1. log_init_deferred(self) — records the log file path (via
//      path_util's long-path-safe helper) but does NOT open the
//      file yet. Safe to call before config is parsed.
//   2. After config is loaded, log_set_level(cfg.log_level) is called.
//      If level > 0, this opens (and truncates) the log file.
//   3. log_line(fmt, ...) writes a single timestamped line, taking the
//      critical section for thread safety. Each call opens-writes-closes
//      the file, so writes survive a game crash (the line is committed
//      to the OS file handle before the next log call runs; we don't
//      call FlushFileBuffers, so this is crash-safe but not power-loss-safe
//      — see log.cpp's log_line for the full rationale).
//   4. log_shutdown() (called on explicit FreeLibrary, not at process
//      exit) tears down the critical section and frees the heap path.
//
// Why open-write-close every line instead of holding the file open?
//   - Durability: if DXMD crashes (the entire reason this DLL exists),
//     we don't want the crash to leave the log buffer-flushed.
//   - Simplicity: no separate flush logic, no buffered-write races.
//   - Cost: negligible — we write a few dozen lines per game session
//     at LogLevel=1.
//
// Why heap-allocated g_log_path instead of wchar_t[MAX_PATH]?
//   - Long-path support (Windows allows >MAX_PATH paths with long-path
//     opt-in). path_util's helper grows up to 32K wchars; we follow.

#include "log.h"
#include "path_util.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace dtf {

static CRITICAL_SECTION g_lock;
static bool             g_lock_inited = false;
static wchar_t*         g_log_path = nullptr;   // heap-allocated; can be very long
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

void log_init_deferred(HMODULE self) {
    if (g_inited) return;
    InitializeCriticalSection(&g_lock);
    g_lock_inited = true;
    g_level = 0;
    compute_log_path(self);
    g_inited = true;
    // Do NOT open the file yet; wait until config tells us LogLevel > 0.
    //
    // This "deferred" pattern is a deliberate trust signal for users
    // who set LogLevel=0 in dxmd-thread-fix.ini. If we always created
    // the log file (even empty), a privacy-aware user couldn't easily
    // verify the DLL is silent. With deferred init:
    //
    //   - LogLevel=0  → no log file is ever created or touched. The
    //                   DLL is provably silent (delete the log file
    //                   and confirm it doesn't reappear after launch).
    //   - LogLevel>=1 → log_set_level(level) below calls log_open()
    //                   which creates (and truncates) the log file
    //                   immediately — BEFORE the first log_line call,
    //                   so the file exists from the start of the
    //                   session even if no log line has been emitted
    //                   yet. Subsequent log_line calls append per-
    //                   line with the open/write/close pattern.
    //
    // The cost is two-stage init in dllmain.cpp: log_init_deferred
    // FIRST (so the critical section exists before any thread might
    // hit log_line), then load_config, then log_set_level which
    // calls log_open if LogLevel > 0.
}

// Truncating CreateFileW (CREATE_ALWAYS) used here, NOT append. This
// is the SESSION log file: each game launch starts a fresh log. Append
// mode would let the file grow indefinitely across runs, which is bad
// for both disk space and for the user's ability to find "what
// happened in this run" (they'd have to scroll past months of history).
void log_open() {
    if (!g_inited) return;
    if (!g_log_path || g_log_path[0] == 0) return;
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
    free_wstr(g_log_path);
    g_log_path = nullptr;
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
    if (!g_log_path || g_log_path[0] == 0) return;

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
    // CreateFileW + WriteFile + CloseHandle PER LINE looks weird — most
    // loggers hold the file open. We deliberately don't, for two reasons:
    //
    // 1. Durability. The entire reason this DLL exists is that DXMD
    //    crashes. If our log doesn't flush before the crash, the most
    //    important lines (FIX STATUS, last hooked call before crash)
    //    are lost. CreateFile+Write+CloseHandle means each line is
    //    committed to the OS file handle (and thus visible to
    //    whoever opens the file next) before the next line runs.
    //    The line survives a process crash. (It does NOT guarantee
    //    physical-disk durability against power loss — we don't
    //    call FlushFileBuffers — but the threat we're protecting
    //    against is "DXMD crashed mid-game", not "the user yanked
    //    the power cord", and crash-survival is all we need.)
    //
    // 2. File sharing. If a curious user (or our own install script)
    //    opens the log file in Notepad while the game is running, a
    //    held-open exclusive handle would prevent that. The per-call
    //    open uses FILE_SHARE_READ | FILE_SHARE_WRITE so anyone can
    //    peek at the log mid-session.
    //
    // The cost is a few syscalls per logged line. At LogLevel=1 that's
    // ~10 lines per game session (startup banner only). At LogLevel=2
    // it's more, but that level is only for bug reports.
    //
    // OPEN_ALWAYS opens the file if it exists, creates if not (which
    // matters when LogLevel was raised from 0 → 1 mid-attach and the
    // file wasn't pre-opened by log_open).
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

// Used by detours to throttle log output at LogLevel=1. Returns true
// the FIRST time it's called for a given flag, false thereafter.
// Implemented as an atomic compare-and-swap so multiple game threads
// can race into the hooked API simultaneously and exactly one wins
// the "log this" race.
bool first_hit(LONG volatile* flag) {
    return InterlockedCompareExchange(flag, 1, 0) == 0;
}

} // namespace dtf
