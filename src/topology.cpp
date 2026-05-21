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
    // Resolve the real CPU-count APIs directly via GetProcAddress so we
    // read TRUTH even if (hypothetically) hooks are already installed.
    // This is defense-in-depth: today set_topology() is called BEFORE
    // install_cpu_hooks() so the calls would be unhooked anyway, but
    // future refactors could reorder that. Reading via direct function
    // pointers from kernel32 makes this code robust to call order.
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
    // total, NOT GetNativeSystemInfo. The latter reports processors in
    // the calling thread's current processor group, which on a >64-CPU
    // machine (e.g. Threadripper PRO 7995WX at 96C/192T, 3 groups) only
    // sees part of the system.
    WORD real_total = 0;
    if (real_GetActiveProcessorCount) {
        real_total = static_cast<WORD>(real_GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    } else if (real_GetNativeSystemInfo) {
        SYSTEM_INFO si{};
        real_GetNativeSystemInfo(&si);
        real_total = static_cast<WORD>(si.dwNumberOfProcessors);
    } else {
        // Last resort - shouldn't happen on any supported Windows.
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

    g_topology.logical_processors = clamped;
    g_topology.groups             = 1;
    g_topology.numa_nodes         = 1;
}

} // namespace dtf
