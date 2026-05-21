// topology.cpp - captures real CPU counts and stores the clamped fake
// values reported by every hook.
//
// CALL-ORDER REQUIREMENT: set_topology() must run BEFORE
// install_cpu_hooks(). Once detours are installed, calling the kernel32
// topology APIs (even via a freshly-resolved GetProcAddress pointer)
// enters our own detour code, which would feed our lies back into
// "real" topology.

#include "topology.h"

namespace dtf {

static FakeTopology g_topology;

const FakeTopology& topology() { return g_topology; }

DWORD_PTR first_n_bits_mask(WORD n) {
    if (n == 0) return 0;
    if (n >= sizeof(DWORD_PTR) * 8) return static_cast<DWORD_PTR>(-1);
    return (static_cast<DWORD_PTR>(1) << n) - 1;
}

void set_topology(WORD desired) {
    using PFN_GNSI = void  (WINAPI*)(LPSYSTEM_INFO);
    using PFN_GAPC = DWORD (WINAPI*)(WORD);
    using PFN_GAGC = WORD  (WINAPI*)();

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_GNSI real_GetNativeSystemInfo = k32 ? reinterpret_cast<PFN_GNSI>(
        GetProcAddress(k32, "GetNativeSystemInfo")) : nullptr;
    PFN_GAPC real_GetActiveProcessorCount = k32 ? reinterpret_cast<PFN_GAPC>(
        GetProcAddress(k32, "GetActiveProcessorCount")) : nullptr;
    PFN_GAGC real_GetActiveProcessorGroupCount = k32 ? reinterpret_cast<PFN_GAGC>(
        GetProcAddress(k32, "GetActiveProcessorGroupCount")) : nullptr;

    // Use GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) for the system
    // total. GetNativeSystemInfo only reports the calling thread's
    // current processor group, which on >64-CPU machines undercounts.
    WORD real_total = 0;
    if (real_GetActiveProcessorCount) {
        real_total = static_cast<WORD>(real_GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    } else if (real_GetNativeSystemInfo) {
        SYSTEM_INFO si{};
        real_GetNativeSystemInfo(&si);
        real_total = static_cast<WORD>(si.dwNumberOfProcessors);
    } else {
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        real_total = static_cast<WORD>(si.dwNumberOfProcessors);
    }

    g_topology.real_logical_processors = real_total;
    g_topology.real_groups = real_GetActiveProcessorGroupCount
        ? real_GetActiveProcessorGroupCount() : 1;

    WORD clamped = desired;
    if (clamped < 1) clamped = 1;
    if (clamped > g_topology.real_logical_processors) {
        clamped = g_topology.real_logical_processors;
    }
    // Single fake group means dwActiveProcessorMask must fit in 64 bits.
    if (clamped > 64) clamped = 64;
    // Guard against real_total == 0 (Wine/Proton/broken hooks); a
    // 1-CPU fake is degraded but functional, while 0 would produce
    // empty affinity masks that hang the game's thread pools.
    if (clamped < 1) clamped = 1;

    g_topology.logical_processors = clamped;
    g_topology.groups             = 1;
    g_topology.numa_nodes         = 1;
}

} // namespace dtf
