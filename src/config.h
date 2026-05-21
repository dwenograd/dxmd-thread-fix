// config.h - parsed INI configuration.
//
// Defaults below take effect if dxmd-thread-fix.ini, the [ThreadFix]
// section, or individual keys are missing. See dxmd-thread-fix.ini for
// the user-facing description of each value.

#pragma once

#include <windows.h>

namespace dtf {

struct Config {
    // Logical processors to report to the game. Community-validated
    // sweet spot for DXMD is 8. Clamped to min(real_count, 64) in
    // topology.cpp.
    int  logical_processors = 8;

    // 0 = don't hook SetThreadAffinityMask (default).
    // 1 = hook it and clamp affinity masks into the first
    //     LogicalProcessors bits. Only useful when troubleshooting
    //     affinity-related crashes.
    int  clamp_affinity     = 0;

    // 0 = silent (no log file created).
    // 1 = startup + first-hit per hooked API (default).
    // 2 = verbose; every hooked call logs.
    int  log_level          = 1;
};

Config load_config(HMODULE self);

} // namespace dtf
