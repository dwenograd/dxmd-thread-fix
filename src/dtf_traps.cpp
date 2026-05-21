// dtf_traps.cpp - per-export trap functions for the dxgi proxy.
//
// WHY THIS EXISTS
// ===============
// Every dxgi export in `dxgi_stubs.asm` is a single tail-jump:
//
//     jmp QWORD PTR [pfn_FOO]
//
// `pfn_FOO` is a 64-bit slot in `.data` that's overwritten in DllMain
// with the address of the real System32 dxgi function. Originally
// every slot defaulted to nullptr, and that crashed apphelp's compat
// pass (which runs BEFORE DllMain). Fix: initialize every pfn_FOO at
// compile time to a trap function in this file.
//
// WHY MULTIPLE TRAPS, NOT JUST ONE
// ================================
// The single generic trap (which just returns 0) is fine for the
// compat-pass exports — apphelp calls SetAppCompatStringPointer /
// ApplyCompatResolutionQuirking with a string argument, doesn't care
// about the return value beyond "this exists, ok", and a zero RAX is
// interpreted as success.
//
// But returning 0 from CreateDXGIFactory* means S_OK with an
// uninitialized out-parameter — the caller thinks a factory was
// returned and immediately crashes dereferencing the null/garbage
// pointer they were "given". That's a strictly-worse failure mode than
// a clean error code. So:
//
//   - Compat-pass exports (SetAppCompatStringPointer,
//     ApplyCompatResolutionQuirking, CompatString, CompatValue) get
//     the generic `dtf_trap_pre_resolve` returning 0.
//
//   - Documented HRESULT exports with output parameters get typed
//     traps that zero the out-pointer and return DXGI_ERROR_NOT_FOUND.
//     If the game ever ends up calling these (which only happens if
//     real System32 dxgi failed to load AND the host process kept
//     running anyway), the caller gets a clean, debuggable HRESULT
//     failure instead of a NULL-deref crash.
//
//   - Other undocumented exports keep the generic trap.
//
// AFTER DLLMAIN COMPLETES
// =======================
// Every pfn_FOO has been overwritten with the real System32 dxgi
// address by `load_system_dxgi_and_resolve()`. The traps are then
// effectively dead code until/unless a System32 dxgi export is
// missing on the user's Windows version (in which case that export's
// slot keeps pointing at its trap).

#include <windows.h>

// dxgi headers aren't pulled in to keep our DLL minimal; define the
// constants we need.
#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND 0x887A0002L
#endif

// --- Generic trap: used for compat-pass exports + undocumented ones --

extern "C" __declspec(noinline) void* WINAPI dtf_trap_pre_resolve(void) {
    return nullptr;
}

// --- Typed traps for documented DXGI exports -------------------------

// CreateDXGIFactory(REFIID riid, void **ppFactory)
// CreateDXGIFactory1(REFIID riid, void **ppFactory)
// CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory)
// DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **pDebug)
//
// All four follow the same pattern: last out-parameter is `void**`.
// Zero it and return failure.

extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_CreateDXGIFactory(REFIID /*riid*/, void** ppFactory) {
    if (ppFactory) *ppFactory = nullptr;
    return DXGI_ERROR_NOT_FOUND;
}

extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_CreateDXGIFactory2(UINT /*Flags*/, REFIID /*riid*/, void** ppFactory) {
    if (ppFactory) *ppFactory = nullptr;
    return DXGI_ERROR_NOT_FOUND;
}

// DXGIDeclareAdapterRemovalSupport(void) -> HRESULT
// DXGIDisableVBlankVirtualization(void) -> HRESULT

extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_HRESULT_void(void) {
    return DXGI_ERROR_NOT_FOUND;
}
