// dxgi_exports.h - loads System32\dxgi.dll and overwrites each pfn_FOO
// global (defined in dxgi_exports.cpp) with its real address. The asm
// stubs in dxgi_stubs.asm tail-jump through those pointers.
//
// Each pfn_FOO is initialized at compile time to a trap function (see
// dtf_traps.cpp), so the asm stubs are safe to invoke before this
// function runs — necessary because Windows' apphelp compat pass calls
// some dxgi exports before DllMain.

#pragma once

#include <windows.h>

namespace dtf {

// Returns the loaded module handle, or nullptr on failure.
HMODULE load_system_dxgi_and_resolve();

void free_system_dxgi(HMODULE h);

} // namespace dtf
