// dxgi_exports.h
//
// Loads System32\dxgi.dll and resolves the function pointer for every
// proxied export. The pointers live in dxgi_exports.cpp as `extern "C"`
// globals named `pfn_<ExportName>`; the assembly stubs in dxgi_stubs.asm
// tail-jump through these pointers.
//
// Each pfn_FOO is initialized at COMPILE TIME (in dxgi_exports.cpp) to
// a no-op trap function (see dtf_traps.cpp), so the asm stubs are safe
// to invoke at any time — including BEFORE this function runs. The
// Windows loader's apphelp compat pass calls some dxgi exports
// (notably SetAppCompatStringPointer) before our DllMain runs, and
// trapping those harmlessly is the whole reason the trap design exists.
//
// load_system_dxgi_and_resolve() is called from DllMain after our log
// and config are initialized; it overwrites every successfully-resolved
// pfn_FOO with the real System32 address. After that, normal forwarded
// calls reach the real dxgi.

#pragma once

#include <windows.h>

namespace dtf {

// Returns the handle to the loaded System32 dxgi (so DllMain can free it
// on detach), or nullptr on failure. Logs warnings for unresolved exports.
HMODULE load_system_dxgi_and_resolve();

void free_system_dxgi(HMODULE h);

} // namespace dtf
