// log.h - thread-safe file logger writing next to the DLL.
//
// Each log_line() opens, writes, and closes the file under a critical
// section, so writes survive a process crash.
//
// Log levels:
//   0 = silent. No log file is created.
//   1 = startup + first-hit per API (default).
//   2 = verbose. Every hooked call writes a line.
//
// Lifecycle: log_init_deferred() records the file path without opening
// it. log_set_level() opens (and truncates) the file the first time
// level transitions from 0 to nonzero. log_line() may then be called
// from any thread.

#pragma once

#include <windows.h>

namespace dtf {

void log_init(HMODULE self, int level);
void log_init_deferred(HMODULE self);   // record path, don't open file
void log_shutdown();

int  log_level();
void log_set_level(int level);

// Open and truncate the log file. Called by log_set_level when level
// transitions from 0; silent mode leaves no artifact on disk.
void log_open();

// printf-style; UTF-8, newline added.
void log_line(const char* fmt, ...);

// Atomic; returns true exactly once per `flag`. Used to gate first-hit logs.
bool first_hit(LONG volatile* flag);

} // namespace dtf
