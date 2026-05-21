// topology.h - single source of truth for the fake CPU topology
//
// Every hooked CPU/topology API consults this struct. Returning
// inconsistent values across APIs (e.g. GetSystemInfo says 8, but
// GetActiveProcessorCount says 64) is worse than not hooking at all -
// game code may allocate for 8 and schedule for 64. So: one struct,
// one source of truth, every hook agrees.

#pragma once

#include <windows.h>

namespace dtf {

struct FakeTopology {
    // What the game sees:
    WORD logical_processors = 8;   // per group AND total
    WORD groups             = 1;   // always 1 in our fake world
    WORD numa_nodes         = 1;

    // What the system actually has (captured once at init for logging):
    WORD real_logical_processors = 0;
    WORD real_groups             = 0;
};

// Returns the active fake topology. Set at init time, read by every hook.
const FakeTopology& topology();

// Called once during init after config is parsed. Caps `desired` to the
// real logical-processor count and stores the result.
void set_topology(WORD desired);

// Helper: a DWORD_PTR mask covering the first N bits, saturating at the
// machine word width.
DWORD_PTR first_n_bits_mask(WORD n);

} // namespace dtf
