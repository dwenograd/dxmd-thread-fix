// path_util.h - long-path-safe module-file-name helpers.

#pragma once

#include <windows.h>

namespace dtf {

// Returns the absolute file path of `module` in a fresh heap buffer.
// Caller must free with free_wstr(). Pass nullptr for the host EXE.
// Grows up to 32K wchars for long-path-enabled Windows. Returns nullptr
// on error.
wchar_t* get_module_path(HMODULE module);

// Like get_module_path() but returns just the directory with trailing
// separator. Returns nullptr on error.
wchar_t* get_module_dir(HMODULE module);

// ASCII case-insensitive equality on null-terminated wide strings.
bool wstr_ieq_ascii(const wchar_t* a, const wchar_t* b);

// Free a buffer returned by get_module_path/get_module_dir. Null-safe.
void free_wstr(wchar_t* s);

} // namespace dtf
