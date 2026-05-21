// log.h - append-only, thread-safe logger writing next to the DLL.
//
// Log levels:
//   0 = silent (errors still written if level >= 0; effectively disabled)
//   1 = startup + first-hit per API
//   2 = verbose (every hooked call)
//
// First-hit logging uses one atomic flag per API; verbose logging takes
// the critical section per line.

#pragma once

#include <windows.h>

namespace dtf {

void log_init(HMODULE self, int level);
void log_shutdown();

int  log_level();

// printf-style; null-terminated UTF-8, newline added.
void log_line(const char* fmt, ...);

// Returns true exactly once per `flag` (atomic). Use to gate first-hit logs.
bool first_hit(LONG volatile* flag);

} // namespace dtf
