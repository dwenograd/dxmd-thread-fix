// dllmain.cpp - process attach/detach for the dxgi.dll proxy shim.
//
// Notable departures from generic DllMain advice:
//
// 1. We do the entire fix synchronously in DLL_PROCESS_ATTACH (load
//    real dxgi, install hooks). This is intentional: a dxgi proxy
//    guarantees our DllMain runs before DXMD's entry point, but only
//    BEFORE-our-init guarantees us priority over middleware
//    (ApexFramework, PhysX, fmod, bink, amd_ags) that also imports
//    topology APIs. Punting to a worker thread would be too late.
//    MinHook doesn't touch the loader lock, and the recursive
//    LoadLibrary of System32\dxgi.dll is constrained to an absolute
//    path so it's safe to call here.
//
// 2. We do NOT call DisableThreadLibraryCalls. Combining it with the
//    static CRT (/MT) breaks per-thread CRT state cleanup (ReShade
//    documents the same conclusion).
//
// 3. host_is_dxmd() gates installing the CPU hooks: a proxy DLL that
//    altered any host process would be a living-off-the-land risk.
//    Non-DXMD hosts still get correctly forwarded dxgi.
//
// 4. On real-dxgi load failure we return FALSE from DllMain so the OS
//    reports a clean "failed to load dxgi.dll" instead of a confusing
//    deferred crash when the game calls CreateDXGIFactory.
//
// 5. On DLL_PROCESS_DETACH we skip cleanup when lpReserved != nullptr
//    (process termination): doing MinHook teardown there risks
//    loader-lock deadlocks per MSDN.
//
// PRE-DLLMAIN APPHELP CRASH (load-bearing detail):
// In the DXMD startup path, apphelp.dll's SE_DllLoaded code path calls
// some dxgi exports (notably SetAppCompatStringPointer) BEFORE our
// DllMain runs. If pfn_FOO were nullptr at that point the asm stubs
// would jump to address 0 and the process would die before any log
// was created. Every pfn_FOO is therefore initialized at compile time
// to a trap function. See dtf_traps.cpp for the full story.
//
// Attach sequence:
//   1. log_init_deferred (record path; don't open file yet).
//   2. load_config + log_set_level (opens log if level > 0).
//   3. host_is_dxmd (gate hook install).
//   4. load_system_dxgi_and_resolve (overwrite pfn_FOO traps with real
//      System32 dxgi addresses; return FALSE on failure).
//   5. If non-DXMD: forwarder-only mode, return TRUE.
//   6. set_topology (must run BEFORE step 7; once hooks are installed
//      the APIs are patched in place).
//   7. install_cpu_hooks (logs FIX STATUS: ACTIVE or INACTIVE).
//
// See src/DESIGN.md for the architectural narrative.

#include "config.h"
#include "topology.h"
#include "log.h"
#include "dxgi_exports.h"
#include "cpu_hooks.h"
#include "path_util.h"

#include <string.h>

namespace dtf {

static HMODULE g_self = nullptr;
static HMODULE g_real_dxgi = nullptr;
static bool    g_fix_active = false;

// Returns true if the host EXE basename is "DXMD.exe". Uses the
// long-path-safe helper so it works for installs with paths > MAX_PATH.
static bool host_is_dxmd() {
    wchar_t* path = get_module_path(nullptr);
    if (!path) return false;
    const wchar_t* base = path;
    for (size_t i = 0; path[i]; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') base = &path[i + 1];
    }
    bool result = wstr_ieq_ascii(base, L"DXMD.exe");
    free_wstr(path);
    return result;
}

static BOOL attach() {
    log_init_deferred(g_self);

    Config cfg = load_config(g_self);
    log_set_level(cfg.log_level);   // opens the file if level > 0
    log_line("dxmd-thread-fix attach: loaded at module handle %p", g_self);
    log_line("config: LogicalProcessors=%d ClampAffinity=%d LogLevel=%d",
             cfg.logical_processors, cfg.clamp_affinity, cfg.log_level);

    // Fixed MAX_PATH buffer here is fine — this is for human-readable
    // logging only. host_is_dxmd() uses the long-path-safe helper.
    wchar_t host_path[MAX_PATH];
    DWORD host_n = GetModuleFileNameW(nullptr, host_path, MAX_PATH);
    if (host_n > 0 && host_n < MAX_PATH) {
        log_line("Host process: %ls", host_path);
    }
    const bool is_dxmd = host_is_dxmd();
    if (!is_dxmd) {
        log_line("WARNING: host process is NOT DXMD.exe. CPU topology hooks");
        log_line("WARNING: will NOT be installed (they're DXMD-specific). The");
        log_line("WARNING: dxgi proxy will still forward to System32 dxgi.");
    }

    g_real_dxgi = load_system_dxgi_and_resolve();
    if (!g_real_dxgi) {
        log_line("FATAL: real dxgi could not be loaded. Returning FALSE from");
        log_line("FATAL: DllMain so the OS reports a clean DLL load failure.");
        return FALSE;
    }

    if (!is_dxmd) {
        log_line("FIX STATUS: forwarder-only mode (non-DXMD host).");
        g_fix_active = false;
        return TRUE;
    }

    set_topology(static_cast<WORD>(cfg.logical_processors));
    log_line("Fake topology: %u logical processors, %u group(s)  (real: %u in %u group(s))",
             (unsigned)topology().logical_processors,
             (unsigned)topology().groups,
             (unsigned)topology().real_logical_processors,
             (unsigned)topology().real_groups);

    int rc = install_cpu_hooks(cfg.clamp_affinity != 0);
    if (rc != 0) {
        log_line("============================================================");
        log_line("FIX STATUS: *** INACTIVE ***  (install_cpu_hooks rc=%d)", rc);
        log_line("The thread-count fix did NOT install. The game will run with");
        log_line("its original CPU detection and may crash on high-core CPUs.");
        log_line("Please file a bug report including this log file.");
        log_line("============================================================");
        g_fix_active = false;
    } else {
        log_line("============================================================");
        log_line("FIX STATUS: ACTIVE  (reporting %u logical processors to game)",
                 (unsigned)topology().logical_processors);
        log_line("============================================================");
        g_fix_active = true;
    }
    return TRUE;
}

static void detach() {
    log_line("dxmd-thread-fix detach (explicit unload).");
    remove_cpu_hooks();
    free_system_dxgi(g_real_dxgi);
    g_real_dxgi = nullptr;
    log_shutdown();
}

} // namespace dtf

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            dtf::g_self = hModule;
            // No DisableThreadLibraryCalls — see header comment.
            return dtf::attach();
        case DLL_PROCESS_DETACH:
            // Only do non-trivial cleanup on explicit FreeLibrary
            // (lpReserved == nullptr). At process termination, let
            // Windows reclaim everything; MinHook teardown here would
            // risk loader-lock deadlocks.
            if (lpReserved == nullptr) {
                dtf::detach();
            }
            return TRUE;
        default:
            return TRUE;
    }
}
