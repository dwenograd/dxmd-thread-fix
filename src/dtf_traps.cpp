// dtf_traps.cpp - trap functions for the dxgi proxy.
//
// Every pfn_FOO global in dxgi_exports.cpp is initialized at compile
// time to point at one of these traps. This is load-bearing: the
// Windows app-compat shim engine (apphelp.dll's SE_DllLoaded path)
// calls some dxgi exports — notably SetAppCompatStringPointer — BEFORE
// our DllMain runs. If pfn_FOO were nullptr at that point, the asm
// stub's `jmp QWORD PTR [pfn_FOO]` would jump to address 0 and the
// process would die before our log file was even created. Confirmed
// via minidump analysis on real DXMD crash dumps.
//
// **Do not "fix" the pfn_FOO globals to default to nullptr.** Doing so
// breaks every install in a way that's hostile to diagnose (crash
// before any log artifact exists).
//
// Why 5 traps instead of 1 generic "return 0":
//   - For SetAppCompatStringPointer (apphelp's compat-pass target),
//     returning 0 is observed safe.
//   - For CreateDXGIFactory(REFIID, void**), returning 0 means S_OK —
//     and a caller that doesn't check the return would dereference an
//     uninitialized out-pointer. A typed DXGI_ERROR_NOT_FOUND return
//     with the out-pointer zeroed is much safer.
//
// After load_system_dxgi_and_resolve() runs, every slot for which the
// host's Windows actually exports the symbol is overwritten with the
// real address. Traps remain live only for exports the host's Windows
// version doesn't provide.
//
// These traps are safe under the x64 calling convention because RCX/
// RDX/R8/R9 are caller-saved volatile registers — the trap can ignore
// arguments without leaving caller state inconsistent. A 32-bit
// stdcall port would need per-export argument-count matching.

#include <windows.h>

#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND 0x887A0002L
#endif

// `__declspec(noinline)` + `extern "C"` keeps these as real symbols
// under LTCG so the linker doesn't fold or inline them away. Release
// builds may still apply /OPT:ICF and fold trap functions with
// identical bodies (e.g. dtf_trap_HRESULT_void and
// dtf_trap_HRESULT_HANDLE) to the same address — functionally fine,
// just be aware when inspecting the binary.
extern "C" __declspec(noinline) void* WINAPI dtf_trap_pre_resolve(void) {
    return nullptr;
}

// CreateDXGIFactory / CreateDXGIFactory1.
// Zero the out-pointer BEFORE returning failure: a caller that ignores
// the HRESULT will then fault cleanly on `(*ppFactory)->...` rather
// than reading random memory.
extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_CreateDXGIFactory(REFIID /*riid*/, void** ppFactory) {
    if (ppFactory) *ppFactory = nullptr;
    return DXGI_ERROR_NOT_FOUND;
}

// CreateDXGIFactory2 / DXGIGetDebugInterface1.
extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_CreateDXGIFactory2(UINT /*Flags*/, REFIID /*riid*/, void** ppFactory) {
    if (ppFactory) *ppFactory = nullptr;
    return DXGI_ERROR_NOT_FOUND;
}

// DXGIDeclareAdapterRemovalSupport.
extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_HRESULT_void(void) {
    return DXGI_ERROR_NOT_FOUND;
}

// DXGIDisableVBlankVirtualization. HANDLE arg is documentation only;
// under x64 ABI the trap doesn't depend on argument layout.
extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_HRESULT_HANDLE(HANDLE /*hAdapter*/) {
    return DXGI_ERROR_NOT_FOUND;
}
