#include "cpu_hooks.h"
#include "topology.h"
#include "log.h"

#include "../third_party/minhook/include/MinHook.h"

#include <string.h>

namespace dtf {

// --- Trampolines (set by MinHook on successful hook install) -----------
//
// We hook each API at exactly one address: the one returned by
// GetProcAddress, which follows the kernel32 -> apiset -> KernelBase
// forwarder chain and lands at the real implementation. IAT-resolved
// callers in DXMD.exe, bink2w64.dll, and amd_ags64.dll all wind up with
// pointers to that same address in their IAT slots (the loader follows
// forwarders during import resolution), so hooking that one body catches
// every caller regardless of which module they statically imported from.

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

// First-hit flags
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

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("ERROR: MH_Initialize failed: %d", (int)s);
        return -1;
    }

    // Two-pass install:
    //   Pass 1: CreateHook for every spec; collect (target, tramp) pairs.
    //           Required hooks must all succeed. Optional hooks may fail.
    //   Pass 2: EnableHook for everything created in pass 1, atomically.
    //
    // Two-pass is required because we treat required hooks as
    // all-or-nothing: if any required hook fails to CREATE, we must
    // tear down anything we already created (else the game sees mixed
    // real/fake topology).
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

    for (const auto& spec : g_hook_specs) {
        if (spec.always_install) n_required_total++;

        // Opt-out for SetThreadAffinityMask when not requested.
        if (!spec.always_install && !clamp_affinity &&
            strcmp(spec.api_name, "SetThreadAffinityMask") == 0) {
            continue;
        }

        LPVOID target = resolve_target(spec.api_name);
        if (!target) {
            if (spec.always_install) {
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

        // Publish the trampoline BEFORE EnableHook. The detour can't run
        // yet (hook isn't enabled), but the order is documented as
        // safer and the cost is zero.
        *spec.pp_trampoline = tramp;
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

    // Pass 2: enable everything in one go. MH_EnableHook(MH_ALL_HOOKS)
    // is atomic w.r.t. enabling - all hooks become live together, so
    // there's no window where some are live and others aren't.
    {
        MH_STATUS es = MH_EnableHook(MH_ALL_HOOKS);
        if (es != MH_OK) {
            log_line("ERROR: MH_EnableHook(MH_ALL_HOOKS) failed: %d; aborting.", (int)es);
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
    // Tear down anything we created.
    for (int i = 0; i < n_created; ++i) {
        MH_RemoveHook(created[i].target);
        *created[i].pp_trampoline = nullptr;
    }
    return -1;
}

void remove_cpu_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace dtf
