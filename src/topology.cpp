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
    SYSTEM_INFO si{};
    // Use the raw kernel32 entry directly so we read truth, not our hook
    // (this runs before hooks are installed, but be defensive).
    GetNativeSystemInfo(&si);

    g_topology.real_logical_processors = static_cast<WORD>(si.dwNumberOfProcessors);
    g_topology.real_groups             = static_cast<WORD>(GetActiveProcessorGroupCount());

    WORD clamped = desired;
    if (clamped < 1) clamped = 1;
    if (clamped > g_topology.real_logical_processors) {
        clamped = g_topology.real_logical_processors;
    }

    g_topology.logical_processors = clamped;
    g_topology.groups             = 1;
    g_topology.numa_nodes         = 1;
}

} // namespace dtf
