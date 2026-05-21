// cpu_hooks.cpp - install_cpu_hooks() and the MinHook detours for the
// CPU/topology APIs.
//
// install_cpu_hooks() uses a three-pass install for clean fail
// semantics: (1) MH_CreateHook every spec into a local array without
// publishing the trampoline pointers; (2) publish trampolines to the
// g_real_* globals in one tight loop; (3) MH_EnableHook(MH_ALL_HOOKS).
// Splitting create from publish means a Pass-1 failure can clean up
// without leaving any g_real_* pointing at memory MinHook is about to
// free.
//
// The 6 topology APIs are required all-or-nothing. If any fails, the
// whole install is torn down and FIX STATUS: INACTIVE is logged — a
// half-applied fix would let middleware see "8" from one API and "64"
// from another. SetThreadAffinityMask is opt-in via INI; its install
// failure is non-fatal.

#include "cpu_hooks.h"
#include "topology.h"
#include "log.h"

#include "../third_party/minhook/include/MinHook.h"

#include <string.h>

namespace dtf {

// Each API is hooked at the address returned by GetProcAddress on
// kernel32.dll (with KernelBase.dll as fallback). For some APIs this
// is the kernel32 implementation; for others it's a forwarder/stub
// that chains through apiset to KernelBase. Either way, IAT-resolved
// callers in DXMD.exe, bink2w64.dll, and amd_ags64.dll land on this
// same address. Callers that import directly from KernelBase.dll
// would bypass the hook, but DXMD and its middleware all import from
// KERNEL32.dll.

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

// First-hit flags. Each transitions 0→1 atomically on the first call
// to its hooked API. At LogLevel=1 we log only that first call, then
// stay silent — otherwise high-frequency APIs like GetSystemInfo would
// balloon the log to gigabytes.
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
    // Why this exists: the other 6 hooks tell the game the system has
    // only N logical processors in a single group, but the OS still
    // really has 64+ CPUs across multiple groups. A SetThreadAffinityMask
    // mask applies within the thread's CURRENT group, not globally. If
    // middleware asks for a high-numbered bit (because it saw the real
    // CPU count through a path we didn't hook), the OS would happily
    // pin the thread to that CPU — inconsistent with our lie.
    //
    // When ClampAffinity=1, intersect the requested mask with the
    // first-N-bits mask of the thread's current group. If the result
    // is empty (caller wanted only CPUs we lied about), fall back to
    // the full allowed mask — passing 0 would return failure to a
    // caller that may treat that as fatal.
    //
    // This is opt-in (ClampAffinity=0 by default) because in-game
    // testing on Threadripper showed DXMD never requests affinity
    // outside our fake range. Note: this clamps within the thread's
    // current group only; it does not migrate threads across groups.
    // Best-effort on multi-group systems.
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
// fall back to KernelBase if kernel32 doesn't export the symbol.
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

    // MH_Initialize is idempotent. Track whether WE initialized so the
    // failure path only calls MH_Uninitialize if we own MinHook's
    // lifetime. (Defensive against this function being re-entered;
    // DllMain shouldn't run twice but the bookkeeping is cheap.)
    bool we_initialized_mh = false;
    MH_STATUS s = MH_Initialize();
    if (s == MH_OK) {
        we_initialized_mh = true;
    } else if (s != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("ERROR: MH_Initialize failed: %d", (int)s);
        return -1;
    }

    // Three-pass install (see file header for rationale): create all
    // hooks into a local array, then publish trampolines, then enable.
    // Note: MH_EnableHook(MH_ALL_HOOKS) is not transactional — MinHook
    // enables hooks one at a time and stops on first error. Fine here
    // because we're called from DllMain before any worker threads
    // exist, and on any error we DisableHook/RemoveHook everything.
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

    // Pass 1: create all hooks.
    for (const auto& spec : g_hook_specs) {
        if (spec.always_install) n_required_total++;

        // Skip SetThreadAffinityMask when ClampAffinity=0 — the only
        // optional hook. Installing it would also log affinity calls
        // at LogLevel=2 and actively clamp affinities; both are
        // unwanted in the default configuration.
        if (!spec.always_install && !clamp_affinity &&
            strcmp(spec.api_name, "SetThreadAffinityMask") == 0) {
            continue;
        }

        LPVOID target = resolve_target(spec.api_name);
        if (!target) {
            if (spec.always_install) {
                // Required API missing from kernel32. Shouldn't happen
                // on any supported Windows (these have been kernel32
                // exports since Windows 7).
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

    // Pass 2: publish trampoline pointers. Pure memory writes.
    for (int i = 0; i < n_created; ++i) {
        *created[i].pp_trampoline = created[i].trampoline;
    }

    // Pass 3: enable all hooks.
    {
        MH_STATUS es = MH_EnableHook(MH_ALL_HOOKS);
        if (es != MH_OK) {
            log_line("ERROR: MH_EnableHook(MH_ALL_HOOKS) failed: %d; aborting.", (int)es);
            // Best-effort: disable anything that did get enabled.
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
    for (int i = 0; i < n_created; ++i) {
        MH_RemoveHook(created[i].target);
        *created[i].pp_trampoline = nullptr;
    }
    if (we_initialized_mh) {
        MH_Uninitialize();
    }
    return -1;
}

void remove_cpu_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace dtf
