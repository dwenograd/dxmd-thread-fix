// config.h - the parsed INI configuration.
//
// The Config struct is the single place where defaults are defined.
// load_config() reads dxmd-thread-fix.ini next to the DLL and returns
// a filled-in Config; missing file or missing keys fall back to the
// defaults below.
//
// See dxmd-thread-fix.ini for the user-facing description of each value.

#pragma once

#include <windows.h>

namespace dtf {

struct Config {
    // Number of logical processors to report to the game.
    // Community-validated sweet spot for DXMD is 8. Effective value is
    // clamped to min(real_count, 64) in topology.cpp.
    int  logical_processors = 8;

    // 0 = leave SetThreadAffinityMask alone (default; only the 6
    //     required topology hooks install).
    // 1 = also hook SetThreadAffinityMask and clamp affinity masks
    //     into the first LogicalProcessors bits. Only enable when
    //     troubleshooting affinity-related crashes.
    int  clamp_affinity     = 0;

    // 0 = silent (no log file written at all).
    // 1 = startup + first-hit per hooked API (default; ~10 lines per run).
    // 2 = verbose (every hooked call logs; useful only for bug reports).
    int  log_level          = 1;
};

// Reads dxmd-thread-fix.ini next to the DLL. Missing file = defaults.
Config load_config(HMODULE self);

} // namespace dtf
