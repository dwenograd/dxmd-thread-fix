// cpu_hooks.h
//
// Installs inline detours on all CPU-discovery / topology APIs so the
// game and its middleware see a consistent fake topology defined in
// topology.h.
//
// Hook target resolution: for each API, we call GetProcAddress on
// kernel32.dll (falling back to KernelBase.dll if missing). On modern
// Windows, kernel32's GetProcAddress follows the apiset/forwarder chain
// and returns the address of the actual implementation in KernelBase.
// Static-import callers in DXMD/bink/amd_ags get the same forwarder-
// followed address written into their IAT slots by the loader. So
// hooking that one address catches every caller regardless of which
// module they imported from.
//
// The 6 topology APIs (GetSystemInfo, GetNativeSystemInfo,
// GetActive/MaxProcessorCount, GetActive/MaxProcessorGroupCount) are
// treated as a REQUIRED all-or-nothing set: if any of them fails to
// hook, install_cpu_hooks() returns failure so the caller can flag the
// fix as INACTIVE rather than letting middleware see inconsistent
// topology. SetThreadAffinityMask is opt-in via the INI.

#pragma once

#include <windows.h>

namespace dtf {

// Returns 0 on success (all required hooks installed),
// non-zero on failure (at least one required hook failed).
int install_cpu_hooks(bool clamp_affinity);

// Disables and removes all hooks. Safe to call from DllMain detach,
// but only do so when the DLL is being explicitly unloaded
// (lpReserved == nullptr in DllMain). At process termination, let
// Windows reclaim everything.
void remove_cpu_hooks();

} // namespace dtf
