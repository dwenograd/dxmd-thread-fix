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
        if (bufLen >= MAX_BUF) break;
        DWORD newLen = bufLen * 2;
        if (newLen > MAX_BUF) newLen = MAX_BUF;
        wchar_t* newPath = static_cast<wchar_t*>(
            HeapReAlloc(heap, 0, path, newLen * sizeof(wchar_t)));
        if (!newPath) break;                        // realloc failed; old `path` still owned
        path   = newPath;
        bufLen = newLen;
    }
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
