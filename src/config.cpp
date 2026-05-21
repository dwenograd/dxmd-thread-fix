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
    // Defensive: confirm there's room for the filename before wcscat_s,
    // which would otherwise invoke the CRT invalid-parameter handler
    // (and crash the process during DllMain) on extremely long install
    // paths. If too long, leave the path empty; load_config() will
    // treat that as "no INI" and use defaults.
    const wchar_t kIniName[] = L"dxmd-thread-fix.ini";
    if (wcslen(out) + wcslen(kIniName) + 1 > outLen) {
        out[0] = 0;
        return;
    }
    wcscat_s(out, outLen, kIniName);
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

    // Sanity clamps. `LogicalProcessors` is later re-clamped to
    // `min(real_count, 64)` by set_topology(); we cap here at 64 too
    // so the "config: LogicalProcessors=X" log line agrees with the
    // effective topology cap and users aren't confused by a config
    // value > 64 that's silently reduced later.
    if (c.logical_processors < 1)  c.logical_processors = 1;
    if (c.logical_processors > 64) c.logical_processors = 64;
    if (c.clamp_affinity < 0) c.clamp_affinity = 0;
    if (c.clamp_affinity > 1) c.clamp_affinity = 1;
    if (c.log_level < 0) c.log_level = 0;
    if (c.log_level > 2) c.log_level = 2;
    return c;
}

} // namespace dtf
