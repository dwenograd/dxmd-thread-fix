// dllmain.cpp - process attach/detach for the dxgi.dll proxy shim.
//
// Most of the choices in this file look unusual to someone reading the
// source for the first time. This header explains WHY each unusual
// thing is the way it is, and what specifically breaks if you change
// it back to the "normal" version.
//
// ============================================================
// UNUSUAL THING #1: DLL_PROCESS_ATTACH does a LOT of work
// ============================================================
//
// Standard Windows guidance is "do almost nothing in DllMain". We do
// the entire fix here: load the real System32 dxgi.dll, install six
// MinHook detours, etc. This looks alarming. The reason we do it
// anyway:
//
// The whole point of this DLL is to lie about CPU count BEFORE the
// game starts calling GetSystemInfo. At the moment our DllMain runs,
// the Windows loader has only just begun processing DXMD's static
// import list. ApexFramework, PhysX*, fmod, bink, amd_ags - and their
// DllMains - have NOT been loaded yet. Several of those DllMains
// (verified via dumpbin) call GetSystemInfo themselves. If we punted
// our hook install to a worker thread, the worker wouldn't get
// scheduled before those other DllMains ran, they'd get the REAL CPU
// count, size their thread pools for 64 cores, and the original
// crash would still happen.
//
// Doing the install synchronously in DllMain is safe in our specific
// case because:
//   (a) MinHook does NOT call LoadLibrary or take the loader lock.
//       It writes to executable code pages via VirtualProtect on
//       addresses we resolved via GetProcAddress (whose results are
//       stable in DllMain).
//   (b) MinHook's SuspendThread-on-other-threads loop has no other
//       threads to freeze — we don't create any, and the only thread
//       at this point in process startup is the loader thread (us).
//   (c) The recursive LoadLibrary of System32\dxgi.dll is the one
//       form of recursive load that's actually safe in practice;
//       system DLLs have trivial DllMains.
//
// Every dxgi-proxy game mod in the wild (Special K, ReShade, ENBSeries,
// etc.) uses this exact pattern. Decades of production use confirms
// it works for this specific shape.
//
// ============================================================
// UNUSUAL THING #2: We do NOT call DisableThreadLibraryCalls
// ============================================================
//
// Every proxy-DLL tutorial says to call DisableThreadLibraryCalls in
// DllMain. We deliberately don't. Reason: combining it with the
// static CRT (/MT) is a known proxy-DLL hazard — the static CRT
// relies on per-thread DLL_THREAD_ATTACH / DLL_THREAD_DETACH
// notifications for some internal state cleanup. ReShade documents
// the same conclusion in their source.
//
// The cost of NOT calling it: we get DLL_THREAD_ATTACH/_DETACH
// callbacks (which we ignore — see the `default: return TRUE` arm of
// DllMain). The cost is negligible. The cost of calling it: subtle
// CRT state corruption that's basically impossible to debug.
//
// ============================================================
// UNUSUAL THING #3: We check the host process is DXMD.exe
// ============================================================
//
// A proxy DLL that activates in ANY process it's loaded into would
// modify the CPU-count behavior of arbitrary applications. That's:
//   (a) Not what users want (they installed this to fix one game).
//   (b) A LOLBin risk (living-off-the-land binary) — someone could
//       drop our DLL next to a different EXE to silently alter that
//       app's thread-pool sizing.
//
// So host_is_dxmd() refuses to install the CPU hooks unless the host
// EXE is literally named "DXMD.exe". We still forward the dxgi
// exports correctly (so the unintended host isn't broken), but the
// thread-count-fix part is gated.
//
// ============================================================
// UNUSUAL THING #4: We return FALSE on real-dxgi load failure
// ============================================================
//
// If LoadLibraryExW("C:\Windows\System32\dxgi.dll") fails (which
// should be impossible on a working Windows install, but might
// happen with aggressive antivirus blocking System32 loads), we
// return FALSE from DllMain. That makes the loader unmap us and
// report the host process "failed to load dxgi.dll" — a clean OS
// error.
//
// The alternative is returning TRUE with the pfn_FOO traps still in
// place: the game would call CreateDXGIFactory, our trap would
// return DXGI_ERROR_NOT_FOUND, and the game would behave however it
// behaves with no D3D — possibly a confusing crash later. Failing
// fast at load time is much easier for users to diagnose.
//
// ============================================================
// UNUSUAL THING #5: Detach skips cleanup when lpReserved != nullptr
// ============================================================
//
// Standard Windows etiquette that most code ignores: DLL_PROCESS_DETACH
// is called in two distinct situations, and the lpReserved parameter
// tells you which:
//   - lpReserved == NULL → an EXPLICIT FreeLibrary call. The DLL is
//     being unloaded but the process keeps running. Clean up resources.
//   - lpReserved != NULL → the PROCESS IS TERMINATING. Other threads
//     may be killed mid-execution, the loader is past the point where
//     calling LoadLibrary / locks is safe, and the OS is going to
//     reclaim every resource we own anyway.
//
// In the second case, doing cleanup (especially MinHook teardown,
// which manipulates code pages) risks loader-lock deadlocks per the
// MSDN "DllMain entry point" best-practices document. We skip it.
// "Leaking" at process exit is correct here — the OS reclaims it all.
//
// ============================================================
// PRE-DLLMAIN APPHELP CRASH (this is the big one)
// ============================================================
//
// The Windows loader runs an application-compatibility shim pass
// (apphelp.dll) on every just-mapped DLL BEFORE running its
// DllMain. For dxgi specifically, that pass calls some of dxgi's
// compat-namespace exports (notably SetAppCompatStringPointer) to
// apply OS-level compat fixups for the just-loaded DLL.
//
// The first version of this DLL initialized every pfn_FOO to nullptr
// (the normal C++ default) and resolved them in DllMain. That meant
// our asm stubs did `jmp QWORD PTR [pfn_FOO]` → loaded nullptr →
// jumped to address 0 → process died at fault offset 0, INSIDE
// apphelp's call into our SetAppCompatStringPointer, before our
// DllMain ever ran. Confirmed via minidump analysis.
//
// Fix: every pfn_FOO is initialized at COMPILE TIME to point at a
// no-op trap function (see dtf_traps.cpp + dxgi_exports.cpp). The
// asm stubs always land on valid code. Apphelp calls our stub, the
// stub jumps to the trap, the trap returns 0 ("no shim applied;
// nothing further to do"), and apphelp moves on. Then the loader
// runs our DllMain, which overwrites the trap pointers with real
// System32 dxgi addresses.
//
// This is the single most important thing about this codebase. If
// you "fix" the pfn_FOO globals to default to nullptr (or
// optimize-away the traps as "unused"), the entire DLL stops
// working in DXMD, and the failure mode (crash before our log file
// is even created) is hostile to diagnose.
//
// ============================================================
// DLLMAIN SEQUENCE (what attach() actually does, in order)
// ============================================================
//
//   1. log_init_deferred — record the log file path but DON'T open
//      it yet. Open-on-first-write means a LogLevel=0 user sees no
//      log artifact appear on disk at all (a trust signal).
//   2. load_config — read dxmd-thread-fix.ini next to this DLL.
//   3. log_set_level — apply the parsed LogLevel; opens the log
//      file if level > 0.
//   4. host_is_dxmd — refuse to install CPU hooks in non-DXMD
//      hosts (see UNUSUAL THING #3).
//   5. load_system_dxgi_and_resolve — load System32 dxgi and
//      overwrite every pfn_FOO with the real address. Returns FALSE
//      from DllMain if this fails (see UNUSUAL THING #4).
//   6. set_topology — capture the REAL CPU count via direct
//      GetProcAddress (must run BEFORE step 7, because once hooks
//      are installed the API code itself is patched and every
//      caller — including us — enters the detour).
//   7. install_cpu_hooks — three-pass MinHook install. See
//      cpu_hooks.cpp for the design.
//   8. Log FIX STATUS: ACTIVE or INACTIVE so the user can spot it.
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

// Returns true if the current process executable's basename is "DXMD.exe".
// Uses the long-path-safe get_module_path() helper so it works correctly
// on installs with paths longer than legacy MAX_PATH.
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
    //
    // Note: this uses a fixed MAX_PATH buffer rather than the
    // long-path-safe get_module_path() helper because the result is
    // purely for human-readable logging — truncation here is a
    // log-quality issue, not a correctness one. The actual DXMD
    // identity check (host_is_dxmd) above DOES use the long-path-safe
    // helper because that decision gates whether the fix installs at
    // all.
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
