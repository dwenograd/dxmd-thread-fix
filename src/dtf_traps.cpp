// dtf_traps.cpp - per-export trap functions for the dxgi proxy.
//
// =====================================================================
// UNUSUAL THING: every dxgi function pointer points HERE before the
// real System32 dxgi is resolved.
// =====================================================================
//
// What's weird: This file defines 5 functions whose only purpose is to
// be the initial target of our exported tail-jump stubs. They look
// useless. They are not — they are load-bearing.
//
// What specifically breaks without them: the first version of this DLL
// initialized the pfn_FOO pointers to nullptr, planning to fill them
// in during DllMain. Every install crashed instantly on game launch
// with a fault at address 0. The crash sequence we eventually traced
// (via Python minidump-library analysis of the user's crash dump):
//
//   1. Process starts. OS loader maps DXMD.exe's import dependencies
//      including our dxgi.dll. Our DLL is on disk and now in memory.
//
//   2. **Before** running our DllMain, the OS loader invokes
//      apphelp.dll — the app-compat shim layer. For executables
//      flagged for compat fixups (DXMD is, like most pre-Win10 AAA
//      games), apphelp scans the loaded DLLs for known compat-
//      relevant exports and pokes them with per-app strings.
//
//   3. For dxgi specifically, apphelp resolves and calls
//      `SetAppCompatStringPointer` on the dxgi module that's about to
//      satisfy the import. Because OS load order picked our dxgi.dll
//      (sitting next to DXMD.exe) over System32\dxgi.dll, apphelp
//      gets a pointer to OUR exported stub.
//
//   4. Our stub is the tail-jump `jmp QWORD PTR [pfn_SetAppCompatStringPointer]`.
//      The pointer is still nullptr (DllMain hasn't run — we're
//      pre-DllMain). The CPU jumps to address 0.
//
//   5. Process death. Fault offset 0 in BEX64. No log file (log_init
//      never ran). User just sees "DXMD crashed".
//
// We could not deduce step 2-3 from documentation; apphelp's compat
// pass is largely undocumented. We confirmed it by:
//   - Extracting the crash dump's exception context: RIP=0, RAX held
//     the address of our `SetAppCompatStringPointer` exported stub.
//   - Setting a debugger breakpoint on the stub and inspecting the
//     call stack: top frame was `apphelp!SE_DllLoaded` calling into
//     dxgi compat-fixup code, all of this BEFORE our DllMain entry.
//   - Verifying the same export wasn't called by our PowerShell
//     smoke-load test (PowerShell's LoadLibraryW doesn't carry the
//     same app-compat manifest, so smoke tests missed it entirely —
//     don't trust them for proxy-DLL validation).
//
// The fix is this file: every pfn_FOO slot is initialized AT COMPILE
// TIME (see the initializer block in dxgi_exports.cpp) to point at
// one of these trap functions. When apphelp does its pre-DllMain call
// the stub jumps to a real function that returns a safe value, apphelp
// is satisfied, the loader proceeds, and our DllMain runs and
// overwrites every slot with the real System32 dxgi address.
//
// =====================================================================
// UNUSUAL THING: 5 traps with 5 different signatures, not 1 generic
// trap.
// =====================================================================
//
// What's weird: a single "return 0 in RAX, ret" trap would satisfy the
// CPU. We have FIVE separate trap functions instead.
//
// What specifically breaks with a single generic trap: returning 0
// means different things to different callers.
//
//   - For SetAppCompatStringPointer (called by apphelp), the return
//     value is treated as "shim status applied" / void-ish. 0 is fine.
//
//   - For CreateDXGIFactory(REFIID, void** ppFactory), returning 0
//     means `S_OK` ("factory created successfully — see ppFactory").
//     The caller then dereferences *ppFactory, which is uninitialized
//     stack/register memory because we never wrote to it. That's a
//     strictly-WORSE failure than a clean error code: the user gets a
//     random-looking crash inside d3d code rather than a clear
//     "DXGI_ERROR_NOT_FOUND" return that any sensible caller will
//     check and propagate.
//
// So the traps are split by signature:
//
//   - `dtf_trap_pre_resolve`         (returns 0): apphelp compat-pass
//     exports, undocumented PIX/DXGID3D10 internals where 0 is safe
//     (these aren't called pre-DllMain in practice; the trap is just
//     belt-and-suspenders).
//
//   - `dtf_trap_CreateDXGIFactory`   (REFIID, void**): zeros the out-
//     pointer, returns DXGI_ERROR_NOT_FOUND. Used for
//     CreateDXGIFactory and CreateDXGIFactory1.
//
//   - `dtf_trap_CreateDXGIFactory2`  (UINT, REFIID, void**): same with
//     the Flags param. Used for CreateDXGIFactory2 and
//     DXGIGetDebugInterface1.
//
//   - `dtf_trap_HRESULT_void`        (no args): returns
//     DXGI_ERROR_NOT_FOUND. Used for DXGIDeclareAdapterRemovalSupport.
//
//   - `dtf_trap_HRESULT_HANDLE`      (HANDLE): returns
//     DXGI_ERROR_NOT_FOUND. Used for DXGIDisableVBlankVirtualization.
//
// After DllMain runs, load_system_dxgi_and_resolve() overwrites every
// pfn_FOO with the real System32 dxgi address. The traps then only
// stay live for exports the host's Windows version doesn't provide
// (rare; mostly Win10+ exports on Win7/8). In that case, the caller
// gets the typed failure return — debuggable, expected, not a crash.

#include <windows.h>

// dxgi headers aren't pulled in to keep our DLL minimal; define the
// constants we need.
#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND 0x887A0002L
#endif

// --- Generic trap: used for compat-pass exports + undocumented ones --
//
// `__declspec(noinline)` + `extern "C"` keeps the symbol intact under
// LTCG (whole-program optimization) so the linker doesn't inline this
// into some other unrelated function and leave the pfn_FOO initializer
// pointing at half a function.
//
// Note that the Release link's /OPT:ICF (identical COMDAT folding)
// may still fold trap functions with identical machine-code bodies to
// the same address — e.g. dtf_trap_HRESULT_void and dtf_trap_HRESULT_HANDLE
// both compile to `mov eax, 0x887A0002; ret` and may share one address
// in the final binary. That's functionally safe: both return the same
// HRESULT and neither reads caller-supplied state. Be aware when
// inspecting the binary in a debugger — a single address may show up
// under two symbol names.
extern "C" __declspec(noinline) void* WINAPI dtf_trap_pre_resolve(void) {
    return nullptr;
}

// --- Typed traps for documented DXGI exports -------------------------

// CreateDXGIFactory(REFIID riid, void **ppFactory)
// CreateDXGIFactory1(REFIID riid, void **ppFactory)
//
// CRITICAL: zero the out-pointer BEFORE returning failure. If we just
// returned the HRESULT without touching ppFactory, a caller that
// ignored the return value (yes, some do) would dereference an
// uninitialized pointer. Zeroing it ensures any such caller faults
// cleanly on `(*ppFactory)->...` rather than reading random memory.
extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_CreateDXGIFactory(REFIID /*riid*/, void** ppFactory) {
    if (ppFactory) *ppFactory = nullptr;
    return DXGI_ERROR_NOT_FOUND;
}

// CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory)
// DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **pDebug)
//
// Same shape with the extra Flags param. Same zero-the-out-pointer
// guarantee.
extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_CreateDXGIFactory2(UINT /*Flags*/, REFIID /*riid*/, void** ppFactory) {
    if (ppFactory) *ppFactory = nullptr;
    return DXGI_ERROR_NOT_FOUND;
}

// DXGIDeclareAdapterRemovalSupport(void) -> HRESULT
//
// No out-parameter; just an HRESULT. Returning failure means "this
// process didn't declare adapter-removal support" which is the safe
// default behavior.
extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_HRESULT_void(void) {
    return DXGI_ERROR_NOT_FOUND;
}

// DXGIDisableVBlankVirtualization(HANDLE hAdapter) -> HRESULT
//
// This signature is undocumented; the parameter is community-reverse-
// engineered as a DXGI adapter handle. We declare the param to
// document the intended export shape (so anyone comparing this file
// against MSDN or Wine's headers can match it up). The trap never
// forwards the call so it doesn't actually matter whether RCX is
// preserved across this function — but writing the signature out
// makes the source match the apparent contract.
extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_HRESULT_HANDLE(HANDLE /*hAdapter*/) {
    return DXGI_ERROR_NOT_FOUND;
}
