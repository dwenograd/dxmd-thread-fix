// path_util.h
//
// Long-path-safe module-file-name retrieval. Returns the directory
// portion of a given module's file path (DLL or EXE), with trailing
// separator included so the caller can concatenate a leaf filename.
//
// Round 11 unified three independent copies of this logic — in
// dllmain.cpp's host_is_dxmd(), config.cpp's INI path lookup, and
// log.cpp's log file path lookup — that had drifted out of sync and
// only one of which (host_is_dxmd) was long-path-safe.

#pragma once

#include <windows.h>

namespace dtf {

// Retrieve the absolute file path of the given module into a fresh
// heap-allocated wchar buffer. Caller must free with free_wstr().
// Pass nullptr for `module` to get the host process EXE path; pass our
// own HMODULE to get the path of our DLL.
//
// Buffer grows dynamically up to 32K wchars to handle long-path-enabled
// Windows configurations. Returns nullptr on any error.
wchar_t* get_module_path(HMODULE module);

// Same as get_module_path() but strips the filename, leaving just the
// directory with trailing separator. Returns nullptr on any error.
wchar_t* get_module_dir(HMODULE module);

// Pure-string helper: returns true if `a` and `b` are ASCII-equal
// case-insensitively. Both must be null-terminated. Used by the
// host_is_dxmd() basename comparison.
bool wstr_ieq_ascii(const wchar_t* a, const wchar_t* b);

// Free a buffer returned by get_module_path() or get_module_dir().
// Safe to call with nullptr (no-op).
void free_wstr(wchar_t* s);

} // namespace dtf
