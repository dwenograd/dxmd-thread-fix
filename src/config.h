#pragma once

#include <windows.h>

namespace dtf {

struct Config {
    int  logical_processors = 8;   // ceiling, clamped to real count at apply time
    int  clamp_affinity     = 0;
    int  log_level          = 1;
};

// Reads dxmd-thread-fix.ini next to the DLL. Missing file = defaults.
Config load_config(HMODULE self);

} // namespace dtf
