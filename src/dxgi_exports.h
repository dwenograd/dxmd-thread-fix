// dxgi_exports.h
//
// Loads System32\dxgi.dll and resolves the function pointer for every
// proxied export. The pointers live in dxgi_exports.cpp as `extern "C"`
// globals named `pfn_<ExportName>`; the assembly stubs in dxgi_stubs.asm
// tail-jump through these pointers.
//
// Must be called BEFORE any exported stub is invoked. We call it from
// DllMain(DLL_PROCESS_ATTACH) (loading a system DLL from DllMain is the
// one form of recursive load that's actually safe in practice, and
// system dxgi is sometimes already loaded by the time we're attached).

#pragma once

#include <windows.h>

namespace dtf {

// Returns the handle to the loaded System32 dxgi (so DllMain can free it
// on detach), or nullptr on failure. Logs warnings for unresolved exports.
HMODULE load_system_dxgi_and_resolve();

void free_system_dxgi(HMODULE h);

} // namespace dtf
