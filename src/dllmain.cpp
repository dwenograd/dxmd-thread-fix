// dllmain.cpp - process attach/detach for the dxgi.dll proxy shim.
//
// HIGH-LEVEL DESIGN
// =================
//
// This DLL is a drop-in proxy for `dxgi.dll` placed next to DXMD.exe.
// On load (via DXMD's static import of "dxgi.dll"), the OS loader maps
// us in place of System32's dxgi. We then:
//
//   1. Forward all 20 dxgi exports to System32's real dxgi.dll via
//      tail-jump asm stubs (see dxgi_stubs.asm, dxgi_exports.cpp).
//
//   2. Install MinHook inline detours on 6 CPU/topology APIs so the
//      game and its middleware see "8 logical processors" instead of
//      the host's real (potentially 64+) count. This fixes DXMD's
//      long-standing high-core-CPU crash.
//
// PRE-DLLMAIN CALLS
// =================
//
// The Windows loader runs an app-compatibility shim pass (apphelp.dll)
// on every just-mapped DLL BEFORE running its DllMain. For dxgi, that
// pass may call `SetAppCompatStringPointer` and a few related compat
// exports to apply OS compat fixups. Our asm stubs handle this safely
// because every pfn_FOO slot is initialized at compile time to a trap
// function (see dtf_traps.cpp + dxgi_exports.cpp). Apphelp calls our
// stub, the stub jumps to the trap, the trap returns 0 (= "no shim
// applied"), and apphelp moves on. Then the loader runs our DllMain.
//
// DLLMAIN SEQUENCE
// ================
//
// On DLL_PROCESS_ATTACH we:
//   1. Initialize the log subsystem in DEFERRED mode (record where the
//      log file would go, but don't open it yet — so LogLevel=0 leaves
//      no artifact).
//   2. Load config from dxmd-thread-fix.ini next to us.
//   3. If LogLevel > 0, open the log file now and emit startup banner.
//   4. Check host process is DXMD.exe — if not, we're being loaded into
//      an unintended host. Log a warning, install the dxgi forwarders
//      (so we don't break the host), but SKIP the CPU hooks (which are
//      DXMD-specific and could affect unrelated apps).
//   5. Load System32\dxgi.dll. If that fails, return FALSE — better to
//      give the OS a clean "DLL failed to load" than to let the host
//      get garbage dxgi.dll behaviour.
//   6. Resolve every export pointer.
//   7. Compute and store the fake CPU topology.
//   8. Install CPU/topology hooks via MinHook (all-or-nothing for the
//      6 required APIs). If install fails, log a loud INACTIVE banner
//      so the user can see the fix didn't apply.
//
// WHY MINHOOK FROM DLLMAIN
// ========================
//
//   - At the time our DllMain runs, the OS loader has only just begun
//     processing DXMD.exe's static imports. ApexFramework_x64.dll,
//     PhysX*, fmod, bink, amd_ags - and their DllMains - have NOT yet
//     been loaded. Several of those DllMains call GetSystemInfo. If we
//     deferred hook install to a worker thread, we'd lose every
//     GetSystemInfo call those DllMains make. The game would crash
//     during early init exactly as before.
//
//   - Synchronous MH_Initialize + MH_CreateHook + MH_EnableHook from
//     DllMain works because:
//       a) The loader lock is held by the current thread (us).
//       b) The only "other" thread that could exist is one we created
//          ourselves; we don't create any. MinHook's SuspendThread loop
//          finds no other threads to freeze.
//       c) MinHook does not call LoadLibrary or take the loader lock
//          itself; it writes to executable code pages we resolve via
//          GetProcAddress (whose results are stable in DllMain).
//
//   - This is the standard pattern for dxgi.dll proxy mods (Special K,
//     ReShade, ENBSeries, etc.). Decades of production use across
//     thousands of games corroborate it for this specific shape.
//
// We do NOT call DisableThreadLibraryCalls. Using it together with the
// static CRT (/MT) is a known proxy-DLL hazard — ReShade explicitly
// avoids the combination because the static CRT relies on per-thread
// notifications for some internal state.
//
// DETACH
// ======
//
// On DLL_PROCESS_DETACH, behaviour depends on `lpReserved`:
//   - If lpReserved is NULL: explicit FreeLibrary. Tear down hooks,
//     free real dxgi, shut down log.
//   - If lpReserved is non-NULL: process is terminating. Skip cleanup
//     entirely — Windows is going to reclaim everything anyway, and
//     non-trivial DllMain work during process exit risks loader-lock
//     deadlocks (see MSDN: "DllMain entry point").

#include "config.h"
#include "topology.h"
#include "log.h"
#include "dxgi_exports.h"
#include "cpu_hooks.h"

#include <string.h>

namespace dtf {

static HMODULE g_self = nullptr;
static HMODULE g_real_dxgi = nullptr;
static bool    g_fix_active = false;

// Case-insensitive ASCII compare for executable basenames.
static bool ieq_ascii(const wchar_t* a, const wchar_t* b) {
    while (*a && *b) {
        wchar_t ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca + (L'a' - L'A'));
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb + (L'a' - L'A'));
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

// Returns true if the current process executable's basename is "DXMD.exe".
static bool host_is_dxmd() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const wchar_t* base = path;
    for (DWORD i = 0; i < n; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') base = &path[i + 1];
    }
    return ieq_ascii(base, L"DXMD.exe");
}

// Returns BOOL for DllMain. Sets g_fix_active as a side effect.
static BOOL attach() {
    // Step 1: deferred log init (records path, doesn't open file yet).
    log_init_deferred(g_self);

    // Step 2: load config.
    Config cfg = load_config(g_self);
    log_set_level(cfg.log_level);   // opens the file if level > 0
    log_line("dxmd-thread-fix attach: loaded at module handle %p", g_self);
    log_line("config: LogicalProcessors=%d ClampAffinity=%d LogLevel=%d",
             cfg.logical_processors, cfg.clamp_affinity, cfg.log_level);

    // Step 3: identify host process.
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

    // Step 4: load real dxgi.
    g_real_dxgi = load_system_dxgi_and_resolve();
    if (!g_real_dxgi) {
        log_line("FATAL: real dxgi could not be loaded. Returning FALSE from");
        log_line("FATAL: DllMain so the OS reports a clean DLL load failure.");
        return FALSE;
    }

    // Step 5: skip CPU hooks if we're not in DXMD.
    if (!is_dxmd) {
        log_line("FIX STATUS: forwarder-only mode (non-DXMD host).");
        g_fix_active = false;
        return TRUE;
    }

    // Step 6: compute fake topology.
    set_topology(static_cast<WORD>(cfg.logical_processors));
    log_line("Fake topology: %u logical processors, %u group(s)  (real: %u in %u group(s))",
             (unsigned)topology().logical_processors,
             (unsigned)topology().groups,
             (unsigned)topology().real_logical_processors,
             (unsigned)topology().real_groups);

    // Step 7: install hooks (all-or-nothing for the required set).
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
            // Do NOT call DisableThreadLibraryCalls when linking the
            // static CRT — it suppresses per-thread CRT notifications
            // the static CRT relies on. See header comment.
            return dtf::attach();
        case DLL_PROCESS_DETACH:
            // Only do non-trivial cleanup on EXPLICIT FreeLibrary
            // (lpReserved == nullptr). At process termination
            // (lpReserved != nullptr) the OS will reclaim everything;
            // doing work here risks loader-lock deadlocks per the
            // MSDN DllMain best-practices document.
            if (lpReserved == nullptr) {
                dtf::detach();
            }
            return TRUE;
        default:
            return TRUE;
    }
}
