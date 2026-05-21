// path_util.cpp - implementation of the long-path-safe helpers
// described in path_util.h.
//
// The core pattern is the MSDN-recommended "double-the-buffer-and-retry"
// approach for GetModuleFileNameW: start at MAX_PATH, double on every
// truncation, cap at 32K wchars to avoid unbounded growth.
//
// Three correctness invariants in this code (each broken in earlier
// review rounds, hence the explicit care):
//   1. No memory leak on HeapReAlloc failure. On failure we keep the
//      old `path` alive (it's still valid) and free it on the way out.
//   2. No use of a truncated buffer. We track `success` explicitly so
//      a loop that hits MAX_BUF without ever fitting returns nullptr
//      rather than returning a possibly-truncated path.
//   3. GetProcessHeap() null-checked once. Real Windows never returns
//      NULL, but the defensive guard is cheap and the cost of being
//      wrong is process death.

#include "path_util.h"

namespace dtf {

static HANDLE process_heap() {
    static HANDLE h = GetProcessHeap();
    return h;
}

void free_wstr(wchar_t* s) {
    if (s) HeapFree(process_heap(), 0, s);
}

wchar_t* get_module_path(HMODULE module) {
    HANDLE heap = process_heap();
    if (!heap) return nullptr;

    // MSDN-recommended pattern: GetModuleFileNameW returns the number of
    // wchars actually written. If that equals the buffer size, the path
    // was truncated and we need a bigger buffer. (It does NOT report
    // the required size, unlike most "give me a buffer" Windows APIs.)
    //
    // Most code uses a fixed MAX_PATH buffer and either accepts
    // silent truncation or just calls it failure. We can't do that
    // because Windows has had opt-in long-path support since 10 1607,
    // and a game install at `\\?\C:\very\long\nested\path\...\retail\`
    // is perfectly legal. With a fixed MAX_PATH buffer:
    //   - host_is_dxmd would always return false (no DXMD detection,
    //     so CPU hooks never install — the fix silently breaks).
    //   - Config path discovery would silently fail (default values
    //     used, user's INI ignored).
    //   - Log path discovery would silently fail (no log file at all,
    //     user can't debug anything).
    // So we grow.
    const DWORD MAX_BUF = 32 * 1024;  // 32K wchars; well above any real path
    DWORD bufLen = MAX_PATH;
    wchar_t* path = static_cast<wchar_t*>(HeapAlloc(heap, 0, bufLen * sizeof(wchar_t)));
    if (!path) return nullptr;

    DWORD n = 0;
    bool success = false;
    for (;;) {
        n = GetModuleFileNameW(module, path, bufLen);
        if (n == 0) break;                          // API error
        if (n < bufLen) { success = true; break; }  // fits

        // n == bufLen means truncation. Grow and retry.
        // Cap at MAX_BUF to bound the loop on a (theoretical / hostile)
        // infinite path.
        if (bufLen >= MAX_BUF) break;
        DWORD newLen = bufLen * 2;
        if (newLen > MAX_BUF) newLen = MAX_BUF;

        // CRITICAL: HeapReAlloc behavior on failure.
        // If HeapReAlloc returns NULL, the ORIGINAL `path` is still
        // alive and owned by us. (This is unlike `realloc()` in C —
        // same semantics but easy to forget.) We must NOT do
        //     path = HeapReAlloc(...);
        // because if it returns NULL we'd lose the original pointer
        // and leak it. Instead we capture into a temporary and only
        // overwrite `path` on success.
        wchar_t* newPath = static_cast<wchar_t*>(
            HeapReAlloc(heap, 0, path, newLen * sizeof(wchar_t)));
        if (!newPath) break;                        // realloc failed; old `path` still owned
        path   = newPath;
        bufLen = newLen;
    }
    // Single cleanup path: if anything went wrong, free and return null.
    // Notably, the loop can exit with `success == false` after MAX_BUF
    // attempts that all hit truncation — we explicitly DO NOT return the
    // truncated buffer in that case. (An earlier version of this code
    // had a fall-through bug where it would proceed with a truncated
    // path; caught by Round 10 review.)
    if (!success) { HeapFree(heap, 0, path); return nullptr; }
    return path;
}

wchar_t* get_module_dir(HMODULE module) {
    wchar_t* path = get_module_path(module);
    if (!path) return nullptr;
    // Strip the filename, leaving trailing separator.
    size_t len = 0;
    while (path[len]) ++len;
    while (len > 0 && path[len - 1] != L'\\' && path[len - 1] != L'/') {
        --len;
    }
    path[len] = 0;
    return path;
}

bool wstr_ieq_ascii(const wchar_t* a, const wchar_t* b) {
    while (*a && *b) {
        wchar_t ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca + (L'a' - L'A'));
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb + (L'a' - L'A'));
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

} // namespace dtf
