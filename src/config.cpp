#include "config.h"

#include <string.h>

namespace dtf {

static void ini_path(HMODULE self, wchar_t* out, DWORD outLen) {
    out[0] = 0;
    DWORD n = GetModuleFileNameW(self, out, outLen);
    if (n == 0 || n >= outLen) { out[0] = 0; return; }
    for (DWORD i = n; i > 0; --i) {
        if (out[i - 1] == L'\\' || out[i - 1] == L'/') {
            out[i] = 0;
            break;
        }
    }
    wcscat_s(out, outLen, L"dxmd-thread-fix.ini");
}

Config load_config(HMODULE self) {
    Config c;
    wchar_t path[MAX_PATH];
    ini_path(self, path, MAX_PATH);
    if (path[0] == 0) return c;

    // GetPrivateProfileIntW lives in kernel32 - no extra dependency.
    c.logical_processors = static_cast<int>(GetPrivateProfileIntW(
        L"ThreadFix", L"LogicalProcessors", c.logical_processors, path));
    c.clamp_affinity = static_cast<int>(GetPrivateProfileIntW(
        L"ThreadFix", L"ClampAffinity", c.clamp_affinity, path));
    c.log_level = static_cast<int>(GetPrivateProfileIntW(
        L"ThreadFix", L"LogLevel", c.log_level, path));

    // Sanity clamps (further clamped to real CPU count by set_topology()).
    if (c.logical_processors < 1)   c.logical_processors = 1;
    if (c.logical_processors > 256) c.logical_processors = 256;
    if (c.clamp_affinity < 0) c.clamp_affinity = 0;
    if (c.clamp_affinity > 1) c.clamp_affinity = 1;
    if (c.log_level < 0) c.log_level = 0;
    if (c.log_level > 2) c.log_level = 2;
    return c;
}

} // namespace dtf
