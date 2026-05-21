// cpu_hooks.h
//
// Installs inline detours on all CPU-discovery / topology APIs so the
// game and its middleware see a consistent fake topology defined in
// topology.h.
//
// We hook BOTH kernel32 and KernelBase addresses (deduped) because on
// modern Windows, kernel32 exports may be forwarders into KernelBase,
// and middleware may resolve through either module via api-set DLLs.

#pragma once

#include <windows.h>

namespace dtf {

// Returns 0 on success, or a MinHook MH_STATUS code on failure.
int install_cpu_hooks(bool clamp_affinity);

// Disables and removes all hooks. Safe to call from DllMain detach.
void remove_cpu_hooks();

} // namespace dtf
