// topology.h - single source of truth for the fake CPU topology.
//
// Every hooked CPU API reads from this struct so all hooks report
// consistent values (otherwise game code could allocate for one count
// and schedule for another).

#pragma once

#include <windows.h>

namespace dtf {

struct FakeTopology {
    // Values reported to the game:
    WORD logical_processors = 8;
    WORD groups             = 1;
    WORD numa_nodes         = 1;

    // Real system values, captured at init for logging:
    WORD real_logical_processors = 0;
    WORD real_groups             = 0;
};

const FakeTopology& topology();

// Caps `desired` to the real logical-processor count and stores it.
void set_topology(WORD desired);

// DWORD_PTR mask covering the first N bits, saturating at word width.
DWORD_PTR first_n_bits_mask(WORD n);

} // namespace dtf
