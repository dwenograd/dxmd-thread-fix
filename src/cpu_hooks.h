// cpu_hooks.h - inline detours on CPU/topology APIs so the game and
// its middleware see a consistent fake topology (see topology.h).
//
// Each API is resolved via GetProcAddress on kernel32.dll (falling
// back to KernelBase.dll). Callers that import these APIs from
// kernel32 — which is everything in DXMD and its middleware — land on
// the same address and hit our detour. Direct imports from KernelBase
// would bypass the hook; not relevant in practice.
//
// The 6 topology APIs are a REQUIRED all-or-nothing set: if any fails,
// install_cpu_hooks() returns failure so the caller logs FIX STATUS:
// INACTIVE. Half-installed hooks would let middleware see inconsistent
// values. SetThreadAffinityMask is opt-in via the INI.

#pragma once

#include <windows.h>

namespace dtf {

// Returns 0 on success, non-zero on failure.
int install_cpu_hooks(bool clamp_affinity);

// Disables and removes all hooks. Only call on explicit FreeLibrary;
// at process termination, let Windows reclaim everything.
void remove_cpu_hooks();

} // namespace dtf
