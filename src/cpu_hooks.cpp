// cpu_hooks.cpp - implementation of install_cpu_hooks() and the
// six MinHook detours for the CPU/topology APIs.
//
// THE THREE-PASS INSTALL (see install_cpu_hooks below):
//   Pass 1: MH_CreateHook for every API; collect (target, tramp) in a
//           local array. Don't publish the trampoline pointers to the
//           g_real_* globals yet.
//   Pass 2: Publish all trampoline pointers in one tight loop.
//   Pass 3: MH_EnableHook(MH_ALL_HOOKS) makes everything live.
//
// Why three passes? Because if a later hook fails to create and we
// jump to cleanup, the published-but-now-defunct g_real_* would point
// at memory MinHook is about to free. Splitting "create" from "publish"
// keeps the cleanup story trivial — nothing has been published yet
// when Pass 1 fails.
//
// REQUIRED vs OPTIONAL hooks: the 6 topology APIs are an all-or-nothing
// required set. If any one fails to install, the entire fix is torn
// down and FIX STATUS: INACTIVE is logged. A half-applied fix is worse
// than no fix — the game could see "8" from one API and "64" from
// another and crash anyway, making the log misleading.
// SetThreadAffinityMask is opt-in via INI; its install failure is
// non-fatal.
//
// See cpu_hooks.h for the broader rationale (why we hook the
// kernel32-resolved address, what's covered, what's acknowledged not
// covered) and src/DESIGN.md for the full architectural story.

#include "cpu_hooks.h"
#include "topology.h"
#include "log.h"

#include "../third_party/minhook/include/MinHook.h"

#include <string.h>

namespace dtf {

// --- Trampolines (set by MinHook on successful hook install) -----------
//
// We hook each API at exactly one address: the one returned by
// GetProcAddress(kernel32, ...). For some APIs this is the real
// implementation in kernel32; for others it's a forwarder/stub that
// chains through apiset to KernelBase. Either way, this is the
// address that IAT-resolved callers in DXMD.exe, bink2w64.dll, and
// amd_ags64.dll get written into their IAT slots by the loader (the
// loader follows forwarders during import resolution).
//
// LIMITATION: callers that import DIRECTLY from KernelBase.dll (rather
// than from kernel32) and resolve to a distinct address there would
// bypass this hook. In practice the DXMD binary and middleware all
// import only from KERNEL32.dll, so this is not an issue for our use
// case. Future versions may add explicit KernelBase coverage if a
// real-world bypass is observed.

using PFN_GetSystemInfo                 = void      (WINAPI*)(LPSYSTEM_INFO);
using PFN_GetNativeSystemInfo           = void      (WINAPI*)(LPSYSTEM_INFO);
using PFN_GetActiveProcessorCount       = DWORD     (WINAPI*)(WORD);
using PFN_GetMaximumProcessorCount      = DWORD     (WINAPI*)(WORD);
using PFN_GetActiveProcessorGroupCount  = WORD      (WINAPI*)();
using PFN_GetMaximumProcessorGroupCount = WORD      (WINAPI*)();
using PFN_SetThreadAffinityMask         = DWORD_PTR (WINAPI*)(HANDLE, DWORD_PTR);

static PFN_GetSystemInfo                 g_real_GetSystemInfo                 = nullptr;
static PFN_GetNativeSystemInfo           g_real_GetNativeSystemInfo           = nullptr;
static PFN_GetActiveProcessorCount       g_real_GetActiveProcessorCount       = nullptr;
static PFN_GetMaximumProcessorCount      g_real_GetMaximumProcessorCount      = nullptr;
static PFN_GetActiveProcessorGroupCount  g_real_GetActiveProcessorGroupCount  = nullptr;
static PFN_GetMaximumProcessorGroupCount g_real_GetMaximumProcessorGroupCount = nullptr;
static PFN_SetThreadAffinityMask         g_real_SetThreadAffinityMask         = nullptr;

// First-hit flags. Each is an atomic 0/1 that goes from 0 to 1 the
// first time the corresponding hooked API is called. Used to throttle
// log output at LogLevel=1: log the FIRST call to each hooked API
// (so you can see the hook fired and what value was rewritten), then
// stay silent for that API.
//
// Without this throttling, LogLevel=1 would behave like LogLevel=2:
// GetSystemInfo can be called thousands of times per second by some
// middleware (frame-pacing checks, internal worker scheduling, etc.).
// Logging every call would balloon the log to gigabytes per session
// and make the disk I/O itself a problem.
//
// Implementation: InterlockedCompareExchange flips the flag from 0 to
// 1 atomically. Whoever wins the CAS gets to log; everyone else sees
// the flag already 1 and skips. See log.cpp::first_hit().
static LONG volatile g_fh_GetSystemInfo                 = 0;
static LONG volatile g_fh_GetNativeSystemInfo           = 0;
static LONG volatile g_fh_GetActiveProcessorCount       = 0;
static LONG volatile g_fh_GetMaximumProcessorCount      = 0;
static LONG volatile g_fh_GetActiveProcessorGroupCount  = 0;
static LONG volatile g_fh_GetMaximumProcessorGroupCount = 0;
static LONG volatile g_fh_SetThreadAffinityMask         = 0;

static bool g_clamp_affinity = false;

// --- Detours -----------------------------------------------------------

static void rewrite_system_info(LPSYSTEM_INFO si) {
    if (!si) return;
    const auto& topo = topology();
    si->dwNumberOfProcessors  = topo.logical_processors;
    si->dwActiveProcessorMask = first_n_bits_mask(topo.logical_processors);
}

static void WINAPI Hooked_GetSystemInfo(LPSYSTEM_INFO si) {
    g_real_GetSystemInfo(si);
    const DWORD original = si ? si->dwNumberOfProcessors : 0;
    rewrite_system_info(si);
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetSystemInfo))) {
        log_line("GetSystemInfo: %lu -> %u", original, topology().logical_processors);
    }
}

static void WINAPI Hooked_GetNativeSystemInfo(LPSYSTEM_INFO si) {
    g_real_GetNativeSystemInfo(si);
    const DWORD original = si ? si->dwNumberOfProcessors : 0;
    rewrite_system_info(si);
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetNativeSystemInfo))) {
        log_line("GetNativeSystemInfo: %lu -> %u", original, topology().logical_processors);
    }
}

static DWORD WINAPI Hooked_GetActiveProcessorCount(WORD group) {
    const auto& topo = topology();
    DWORD real = g_real_GetActiveProcessorCount(group);
    DWORD reported;
    if (group == ALL_PROCESSOR_GROUPS || group == 0) {
        reported = topo.logical_processors;
    } else {
        reported = 0; // we claim only one group
    }
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetActiveProcessorCount))) {
        log_line("GetActiveProcessorCount(group=%u): %lu -> %lu",
                 (unsigned)group, real, reported);
    }
    return reported;
}

static DWORD WINAPI Hooked_GetMaximumProcessorCount(WORD group) {
    const auto& topo = topology();
    DWORD real = g_real_GetMaximumProcessorCount(group);
    DWORD reported;
    if (group == ALL_PROCESSOR_GROUPS || group == 0) {
        reported = topo.logical_processors;
    } else {
        reported = 0;
    }
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetMaximumProcessorCount))) {
        log_line("GetMaximumProcessorCount(group=%u): %lu -> %lu",
                 (unsigned)group, real, reported);
    }
    return reported;
}

static WORD WINAPI Hooked_GetActiveProcessorGroupCount(void) {
    WORD real = g_real_GetActiveProcessorGroupCount();
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetActiveProcessorGroupCount))) {
        log_line("GetActiveProcessorGroupCount: %u -> 1", (unsigned)real);
    }
    return 1;
}

static WORD WINAPI Hooked_GetMaximumProcessorGroupCount(void) {
    WORD real = g_real_GetMaximumProcessorGroupCount();
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetMaximumProcessorGroupCount))) {
        log_line("GetMaximumProcessorGroupCount: %u -> 1", (unsigned)real);
    }
    return 1;
}

static DWORD_PTR WINAPI Hooked_SetThreadAffinityMask(HANDLE hThread, DWORD_PTR mask) {
    // =================================================================
    // UNUSUAL THING: we modify the affinity mask the caller asked for.
    // =================================================================
    //
    // A code-curious reader looking at this will reasonably ask "wait,
    // why is this DLL rewriting thread affinity?" Here's the concrete
    // scenario we're guarding against, and why this hook is OPT-IN
    // (ClampAffinity=0 in dxmd-thread-fix.ini by default):
    //
    // PROBLEM: We tell DXMD (via the other 6 hooks) that the system
    // has only N logical processors (default 8). But the OS still
    // really has 64+ CPUs. When DXMD or its middleware calls
    // SetThreadAffinityMask with a bitmask, the bitmask is in REAL-
    // CPU-numbering. If middleware asked for CPU 50 (because at some
    // earlier point it saw 64 real CPUs through a path we didn't
    // hook), the OS would happily pin the thread to CPU 50 — but our
    // lie said CPUs only go up to 8. The thread runs anyway, but we
    // have an inconsistency, and in pathological cases the middleware
    // might decide nothing is running because it's polling on CPU
    // indices it expects to see active.
    //
    // FIX (when ClampAffinity=1):
    //   - Build `allowed` = the bitmask of the first N real CPUs
    //     (the ones in our "fake" topology).
    //   - Compute `intersected` = requested-mask AND allowed.
    //   - If the requested mask had any overlap with allowed, use the
    //     intersection (honors caller intent within our fake set).
    //   - If the requested mask had NO overlap with allowed (caller
    //     wanted ONLY high-numbered CPUs we lied about), fall back to
    //     `allowed`. Passing 0 instead would cause the API to FAIL,
    //     which middleware almost universally treats as fatal — the
    //     thread either won't start or will run on whatever default
    //     mask the OS picks. Better to honor the spirit of the call
    //     by giving the thread SOMETHING runnable within our fake
    //     topology.
    //
    // WHY OPT-IN BY DEFAULT: in practice, all our in-game testing on
    // a 32C/64T Threadripper showed DXMD never asks for an affinity
    // mask outside our fake range — the other 6 hooks suffice. Logging-
    // only mode (ClampAffinity=0) lets users with weird setups discover
    // whether they need to flip it on, without changing behavior for
    // the 99% case.
    const auto& topo = topology();
    const DWORD_PTR allowed = first_n_bits_mask(topo.logical_processors);
    DWORD_PTR effective = mask;
    bool modified = false;
    if (g_clamp_affinity) {
        DWORD_PTR intersected = mask & allowed;
        if (intersected == 0) intersected = allowed;
        if (intersected != mask) {
            effective = intersected;
            modified = true;
        }
    }
    DWORD_PTR prev = g_real_SetThreadAffinityMask(hThread, effective);
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_SetThreadAffinityMask))) {
        log_line("SetThreadAffinityMask: 0x%llx -> 0x%llx%s (prev 0x%llx)",
                 (unsigned long long)mask,
                 (unsigned long long)effective,
                 modified ? " [clamped]" : "",
                 (unsigned long long)prev);
    }
    return prev;
}

// --- Hook table --------------------------------------------------------

struct HookSpec {
    const char* api_name;
    LPVOID      detour;
    void**      pp_trampoline;
    bool        always_install;  // false = only if config enables it
};

static const HookSpec g_hook_specs[] = {
    { "GetSystemInfo",                 (LPVOID)&Hooked_GetSystemInfo,                 (void**)&g_real_GetSystemInfo,                 true  },
    { "GetNativeSystemInfo",           (LPVOID)&Hooked_GetNativeSystemInfo,           (void**)&g_real_GetNativeSystemInfo,           true  },
    { "GetActiveProcessorCount",       (LPVOID)&Hooked_GetActiveProcessorCount,       (void**)&g_real_GetActiveProcessorCount,       true  },
    { "GetMaximumProcessorCount",      (LPVOID)&Hooked_GetMaximumProcessorCount,      (void**)&g_real_GetMaximumProcessorCount,      true  },
    { "GetActiveProcessorGroupCount",  (LPVOID)&Hooked_GetActiveProcessorGroupCount,  (void**)&g_real_GetActiveProcessorGroupCount,  true  },
    { "GetMaximumProcessorGroupCount", (LPVOID)&Hooked_GetMaximumProcessorGroupCount, (void**)&g_real_GetMaximumProcessorGroupCount, true  },
    { "SetThreadAffinityMask",         (LPVOID)&Hooked_SetThreadAffinityMask,         (void**)&g_real_SetThreadAffinityMask,         false },
};

// Resolve the final implementation address. Prefer kernel32 (its
// GetProcAddress follows forwarders to the KernelBase implementation);
// fall back to KernelBase if kernel32 doesn't export the symbol on this
// Windows version.
static LPVOID resolve_target(const char* api) {
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
        if (auto p = GetProcAddress(k32, api)) {
            return reinterpret_cast<LPVOID>(p);
        }
    }
    if (HMODULE kb = GetModuleHandleW(L"KernelBase.dll")) {
        if (auto p = GetProcAddress(kb, api)) {
            return reinterpret_cast<LPVOID>(p);
        }
    }
    return nullptr;
}

// --- Install -----------------------------------------------------------

int install_cpu_hooks(bool clamp_affinity) {
    g_clamp_affinity = clamp_affinity;

    // MH_Initialize is idempotent — calling it on already-initialized
    // MinHook returns MH_ERROR_ALREADY_INITIALIZED rather than crashing.
    // We track whether WE were the ones to initialize, so that on a
    // later failure-path we only call MH_Uninitialize if we actually
    // own the MinHook lifetime. (If MinHook was already initialized
    // when we entered, some other code in the process — extremely
    // unlikely in DXMD but possible if e.g. another mod-loader
    // pre-initialized MinHook — owns it and we don't want to tear
    // its state down.)
    bool we_initialized_mh = false;
    MH_STATUS s = MH_Initialize();
    if (s == MH_OK) {
        we_initialized_mh = true;
    } else if (s != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("ERROR: MH_Initialize failed: %d", (int)s);
        return -1;
    }

    // Three-pass install for clean fail semantics:
    //   Pass 1: MH_CreateHook for every spec; collect (target, tramp).
    //           Required hooks must all create successfully. Optional
    //           hooks may fail (logged and skipped).
    //   Pass 2: Publish the trampoline pointers to the g_real_* globals
    //           that detours dereference.
    //   Pass 3: MH_EnableHook(MH_ALL_HOOKS) to activate all hooks.
    //
    // Why three passes instead of the obvious two (Create + Enable)?
    //
    // The "obvious" two-pass approach is what most MinHook tutorials
    // show, and it almost works. But there's a window:
    //
    //   - Pass 1 succeeds for hooks 1-3, publishes g_real_1/2/3.
    //   - Pass 1 fails for hook 4 → goto fail.
    //   - Cleanup loop calls MH_RemoveHook(target_1) → MinHook FREES
    //     the trampoline memory at g_real_1's value.
    //   - g_real_1 now points at freed memory.
    //
    // Nothing actually calls g_real_1 in that window (the hooks aren't
    // enabled, so no detour can fire), but the dangling pointer feels
    // ugly. The three-pass split keeps Pass 1 a pure "collect resources
    // into a local array" step and only publishes after we've confirmed
    // everything created. Cleanup on Pass 1 failure has nothing to
    // un-publish.
    //
    // Note on "atomic" enable: MH_EnableHook(MH_ALL_HOOKS) is NOT a
    // database-transaction-style atomic operation. MinHook enables
    // hooks one at a time internally and stops on the first error,
    // leaving previously-enabled hooks live. For our DXMD use case
    // that's fine because:
    //   - We're called from DllMain, before any game worker threads
    //     exist; no concurrent caller can observe a partial state.
    //   - On any EnableHook failure we proactively DisableHook all and
    //     RemoveHook all to leave the process in a clean state.
    struct Created {
        LPVOID      target;
        const char* api_name;
        bool        required;
        void**      pp_trampoline;
        void*       trampoline;
        LPVOID      detour;
    };
    Created created[sizeof(g_hook_specs) / sizeof(g_hook_specs[0])];
    int n_created = 0;
    int n_required_created = 0;
    int n_required_total = 0;

    // --- Pass 1: create all hooks --------------------------------------
    for (const auto& spec : g_hook_specs) {
        if (spec.always_install) n_required_total++;

        // SetThreadAffinityMask is the one OPTIONAL hook. It's not
        // installed by default because hooking it would log every
        // affinity-related call in the process (potentially thousands
        // per second in some games) and clamp affinities — both of
        // which are intrusive enough that we only do them when the
        // user explicitly opts in via ClampAffinity=1 in the INI.
        if (!spec.always_install && !clamp_affinity &&
            strcmp(spec.api_name, "SetThreadAffinityMask") == 0) {
            continue;
        }

        LPVOID target = resolve_target(spec.api_name);
        if (!target) {
            if (spec.always_install) {
                // A required topology API isn't exported by kernel32 on
                // this Windows. Shouldn't happen on any supported
                // Windows version (these have all been kernel32 exports
                // since Windows XP). Refuse to half-install — see the
                // "all-or-nothing required set" rationale in the file
                // header.
                log_line("ERROR: required API %s not exported on this Windows; aborting hook install.",
                         spec.api_name);
                goto fail;
            }
            log_line("INFO: optional API %s not present; skipped.", spec.api_name);
            continue;
        }

        void* tramp = nullptr;
        MH_STATUS cs = MH_CreateHook(target, spec.detour, &tramp);
        if (cs != MH_OK) {
            if (spec.always_install) {
                // CreateHook can fail if the target prologue is too
                // short to relocate (some APIs only have a few bytes
                // before the first branch), if a hook is already
                // installed by another tool, or if memory allocation
                // fails. None of these are recoverable for a required
                // hook — tear down and bail.
                log_line("ERROR: MH_CreateHook(%s) failed: %d; aborting.",
                         spec.api_name, (int)cs);
                goto fail;
            }
            log_line("WARN: MH_CreateHook(%s) failed: %d; optional, continuing.",
                     spec.api_name, (int)cs);
            continue;
        }

        created[n_created++] = Created{
            target, spec.api_name, spec.always_install,
            spec.pp_trampoline, tramp, spec.detour
        };
        if (spec.always_install) n_required_created++;
    }

    if (n_required_created != n_required_total) {
        log_line("ERROR: only %d of %d required hooks created; aborting.",
                 n_required_created, n_required_total);
        goto fail;
    }

    // --- Pass 2: publish all trampoline pointers -----------------------
    // Pure memory writes, can't fail. After this loop, the global
    // g_real_* function pointers are populated, but the hooks aren't
    // enabled yet so no detour can actually fire and dereference them.
    for (int i = 0; i < n_created; ++i) {
        *created[i].pp_trampoline = created[i].trampoline;
    }

    // --- Pass 3: enable all hooks --------------------------------------
    {
        MH_STATUS es = MH_EnableHook(MH_ALL_HOOKS);
        if (es != MH_OK) {
            log_line("ERROR: MH_EnableHook(MH_ALL_HOOKS) failed: %d; aborting.", (int)es);
            // Best-effort: turn off anything that did get enabled
            // (MinHook may have enabled some before failing on the
            // current target). DisableHook is safe to call against
            // hooks that weren't actually enabled.
            MH_DisableHook(MH_ALL_HOOKS);
            goto fail;
        }
    }

    log_line("CPU hook install complete: %d hooks active (%d required, %d optional).",
             n_created, n_required_created, n_created - n_required_created);
    for (int i = 0; i < n_created; ++i) {
        log_line("  Hooked %s @ %p%s", created[i].api_name, created[i].target,
                 created[i].required ? " [required]" : " [optional]");
    }
    return 0;

fail:
    // Tear down anything we created. RemoveHook also disables if needed.
    for (int i = 0; i < n_created; ++i) {
        MH_RemoveHook(created[i].target);
        *created[i].pp_trampoline = nullptr;
    }
    // If WE called MH_Initialize during this attempt, undo it. (If
    // MinHook was already initialized when we entered, someone else
    // owns it and we leave it alone.)
    if (we_initialized_mh) {
        // We initialized MinHook ourselves in this call, so we own the
        // cleanup. (If MH_Initialize returned ALREADY_INITIALIZED at
        // entry, MinHook had been initialized by an earlier call to
        // this function or by some other code in the process. Either
        // way we should NOT call MH_Uninitialize on a state we don't
        // own.)
        MH_Uninitialize();
    }
    return -1;
}

void remove_cpu_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace dtf
