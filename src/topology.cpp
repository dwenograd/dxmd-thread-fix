// topology.cpp - implementation of the fake CPU topology described in
// topology.h.
//
// set_topology() captures the REAL CPU counts (by calling the kernel32
// APIs directly via GetProcAddress, bypassing any IAT) and stores both
// the real values (for logging) and the clamped fake values that every
// hooked API will report.
//
// CALL-ORDER REQUIREMENT (important — read this before refactoring):
// set_topology() MUST be called BEFORE install_cpu_hooks(). Once our
// inline detours are installed, calling kernel32's exported topology
// APIs (whether via the IAT or via a freshly-resolved GetProcAddress
// pointer) enters our detour code, because MinHook has patched the
// function prologue in place. GetProcAddress doesn't return a
// "different" pointer post-hook — it returns the same exported
// address as before, but the bytes at that address now jump to our
// detour. Calling that from set_topology() would feed our own lies
// back into our "real" topology, defeating the entire mechanism.
//
// The current attach() sequence in dllmain.cpp satisfies this order;
// any future refactor that wants to call set_topology() after
// install_cpu_hooks() would need to save the unhooked function
// pointers separately, BEFORE the hooks are installed.
//
// Clamp chain: configured value -> min(configured, real_count) ->
// min(that, 64). The 64-cap exists because we report a single
// processor group, whose dwActiveProcessorMask is DWORD_PTR — 64 bits
// on x64. A larger reported count would exceed the mask width.

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
    // Resolve the real CPU-count APIs directly via GetProcAddress.
    //
    // Call-order requirement: set_topology() MUST be called BEFORE
    // install_cpu_hooks(). Once our inline detours are installed,
    // calling these APIs (via any pointer to them) enters our detour
    // code, because MinHook has patched the function prologue in
    // place. GetProcAddress returns the same exported address pre-
    // and post-hook — what changes is the machine code at that
    // address. Calling the patched code from set_topology would feed
    // our own lies back into our "real" topology, defeating the
    // purpose. The current attach() in dllmain.cpp satisfies this
    // order; any future refactor that wants to call set_topology()
    // after install_cpu_hooks() would need to save the original
    // function bytes / unpatched call path here BEFORE hook install.
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
    // Re-apply the at-least-1 floor AFTER the real-count cap. On a
    // healthy supported Windows, real_logical_processors is always
    // >= 1, so this is unreachable. But under Wine, Proton, custom
    // emulation layers, or broken/hostile GetActiveProcessorCount
    // hooks, real_total could come back as 0. Without this guard,
    // clamped = min(desired, 0) = 0 → first_n_bits_mask(0) returns 0
    // → every reported affinity mask is zero, and the game's thread
    // pools spin forever waiting for "any CPU" to be available. A
    // 1-CPU fake topology is degraded but functional; 0 is broken.
    if (clamped < 1) clamped = 1;

    g_topology.logical_processors = clamped;
    g_topology.groups             = 1;
    g_topology.numa_nodes         = 1;
}

} // namespace dtf
