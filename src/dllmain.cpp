// dllmain.cpp - process attach/detach for the dxgi.dll proxy shim.
//
// Sequence on DLL_PROCESS_ATTACH:
//   1. DisableThreadLibraryCalls (we don't care about per-thread notifications)
//   2. Load config from dxmd-thread-fix.ini next to us
//   3. Initialize logging
//   4. Load System32\dxgi.dll and resolve every export pointer the asm
//      stubs in dxgi_stubs.asm tail-jump through. MUST happen before any
//      stub is invoked. The loader will start calling our exports as
//      soon as it processes DXMD.exe's import table for dxgi, so we
//      front-load this synchronously.
//   5. Compute and store the fake CPU topology.
//   6. Install CPU/topology hooks via MinHook, SYNCHRONOUSLY.
//
// Why synchronous hook install (against generic "don't do work in
// DllMain" guidance)?
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
// Sequence on DLL_PROCESS_DETACH:
//   - Best-effort hook removal (Windows is tearing down anyway).
//   - Free the real dxgi.
//   - Shut down logging.

#include "config.h"
#include "topology.h"
#include "log.h"
#include "dxgi_exports.h"
#include "cpu_hooks.h"

namespace dtf {

static HMODULE g_self = nullptr;
static HMODULE g_real_dxgi = nullptr;

static void attach() {
    Config cfg = load_config(g_self);

    log_init(g_self, cfg.log_level);
    log_line("dxmd-thread-fix attach. config: LogicalProcessors=%d ClampAffinity=%d LogLevel=%d",
             cfg.logical_processors, cfg.clamp_affinity, cfg.log_level);

    g_real_dxgi = load_system_dxgi_and_resolve();
    if (!g_real_dxgi) {
        log_line("ERROR: real dxgi could not be loaded. Game rendering will fail.");
        // We still try to install CPU hooks; even without dxgi forwarding
        // the user will at least get a log file telling them what happened.
    }

    set_topology(static_cast<WORD>(cfg.logical_processors));
    log_line("Fake topology: %u logical processors, %u group(s)  (real: %u in %u group(s))",
             (unsigned)topology().logical_processors,
             (unsigned)topology().groups,
             (unsigned)topology().real_logical_processors,
             (unsigned)topology().real_groups);

    int rc = install_cpu_hooks(cfg.clamp_affinity != 0);
    if (rc != 0) {
        log_line("ERROR: install_cpu_hooks returned %d. The thread-count fix is NOT active.", rc);
    }
}

static void detach() {
    log_line("dxmd-thread-fix detach.");
    remove_cpu_hooks();
    free_system_dxgi(g_real_dxgi);
    g_real_dxgi = nullptr;
    log_shutdown();
}

} // namespace dtf

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            dtf::g_self = hModule;
            DisableThreadLibraryCalls(hModule);
            dtf::attach();
            return TRUE;
        case DLL_PROCESS_DETACH:
            dtf::detach();
            return TRUE;
        default:
            return TRUE;
    }
}
