#include "config.h"
#include "path_util.h"

#include <string.h>

namespace dtf {

static Config c_defaults() { return Config{}; }

Config load_config(HMODULE self) {
    Config c;
    wchar_t* dir = get_module_dir(self);
    if (!dir) return c;
    // Build "<dir>dxmd-thread-fix.ini" into a fresh heap buffer.
    static const wchar_t kIniName[] = L"dxmd-thread-fix.ini";
    size_t dirLen = 0; while (dir[dirLen]) ++dirLen;
    size_t pathLen = dirLen + (sizeof(kIniName) / sizeof(wchar_t));  // includes null
    wchar_t* path = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, pathLen * sizeof(wchar_t)));
    if (!path) { free_wstr(dir); return c; }
    for (size_t i = 0; i < dirLen; ++i) path[i] = dir[i];
    for (size_t i = 0; ; ++i) {
        path[dirLen + i] = kIniName[i];
        if (kIniName[i] == 0) break;
    }
    free_wstr(dir);

    // GetPrivateProfileIntW lives in kernel32 - no extra dependency.
    c.logical_processors = static_cast<int>(GetPrivateProfileIntW(
        L"ThreadFix", L"LogicalProcessors", c.logical_processors, path));
    c.clamp_affinity = static_cast<int>(GetPrivateProfileIntW(
        L"ThreadFix", L"ClampAffinity", c.clamp_affinity, path));
    c.log_level = static_cast<int>(GetPrivateProfileIntW(
        L"ThreadFix", L"LogLevel", c.log_level, path));

    HeapFree(GetProcessHeap(), 0, path);

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
