// log.h - thread-safe file logger writing next to the DLL.
//
// Behavior: at process startup, log_open() truncates the log file (if
// LogLevel > 0); each subsequent log_line() appends one line under a
// critical section. Each line opens the file, writes, and closes it,
// so writes survive crashes.
//
// Log levels:
//   0 = silent. No log file is created on disk. log_line() is a no-op.
//   1 = startup + first-hit per API. Default. Tells you the fix engaged
//       and which hooks installed; quiet thereafter.
//   2 = verbose. Every hooked call writes a line. Noisy; useful only
//       when filing a bug report.
//
// Log lifecycle:
//   - log_init_deferred(self) is called at the very top of DllMain
//     attach so we know WHERE the log file would go, but the file is
//     NOT opened yet.
//   - log_set_level(level) is called after config is parsed. If level
//     > 0, this opens (and truncates) the log file for the first time.
//   - log_line() takes the critical section, opens-appends-closes the
//     file for each call (so writes survive a process crash — the
//     line is committed to the OS file handle before the next call).
//
// Thread safety: log_line() takes a critical section per call.
// log_init_*, log_set_level, log_open, log_shutdown are intended to be
// called from DllMain and assume single-threaded execution there.

#pragma once

#include <windows.h>

namespace dtf {

void log_init(HMODULE self, int level);
void log_init_deferred(HMODULE self);   // record path but don't open file
void log_shutdown();

int  log_level();
void log_set_level(int level);

// Open and truncate the log file. Called after config is loaded if
// LogLevel >= 1; deferred if LogLevel == 0 (so silent mode leaves no
// artifact on disk).
void log_open();

// printf-style; null-terminated UTF-8, newline added.
void log_line(const char* fmt, ...);

// Returns true exactly once per `flag` (atomic). Use to gate first-hit logs.
bool first_hit(LONG volatile* flag);

} // namespace dtf
