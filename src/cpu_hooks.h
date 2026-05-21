// cpu_hooks.h
//
// Installs inline detours on all CPU-discovery / topology APIs so the
// game and its middleware see a consistent fake topology defined in
// topology.h.
//
// Hook target resolution: for each API, we call GetProcAddress on
// kernel32.dll (falling back to KernelBase.dll if missing). For some
// APIs this returns the kernel32 implementation directly; for others
// it follows a forwarder chain through the apiset DLLs to the actual
// KernelBase implementation. Either way, callers that resolve the
// same symbol via kernel32 (whether by static import or runtime
// GetProcAddress) end up with the same address that we hook, so they
// hit our detour.
//
// LIMITATION: a caller that imports DIRECTLY from `KernelBase.dll`
// (rather than through `kernel32.dll`) and lands on a distinct
// implementation address would bypass this hook. In practice, the
// DXMD game binary and all its bundled middleware (verified via
// dumpbin /imports) only import from KERNEL32.dll, so this limitation
// doesn't apply to the target use case. Future v1.x may add
// dedicated KernelBase coverage if a real-world need surfaces.
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
