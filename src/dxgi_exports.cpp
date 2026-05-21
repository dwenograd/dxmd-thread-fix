// dxgi_exports.cpp - the pfn_FOO globals tail-jumped through by the
// asm stubs in dxgi_stubs.asm, plus the resolver that overwrites
// them with real System32 dxgi function pointers at DllMain time.
//
// WHY THE COMPILE-TIME TRAP INITIALIZATION
// =========================================
// Every pfn_FOO MUST be a valid function-pointer-shaped value at all
// times, including BEFORE our DllMain runs. The Windows loader's
// app-compat shim layer (apphelp.dll) calls some dxgi exports
// (notably SetAppCompatStringPointer) as part of its compat-fixup
// pass, and that pass runs BEFORE our entry point.
//
// If pfn_FOO were nullptr at that point, the asm stub's
// `jmp QWORD PTR [pfn_FOO]` would jump to NULL and crash the process
// before our DllMain ever ran. This is exactly what the first version
// of this shim did, and it killed every install.
//
// Fix: every pfn_FOO is initialized at COMPILE TIME to point at one
// of the trap functions defined in dtf_traps.cpp. The asm stubs always
// land on valid code. After our DllMain runs and resolves real System32
// dxgi addresses, the trap pointers get overwritten with the real ones.
//
// See dtf_traps.cpp for the full story and src/DESIGN.md for the
// architectural narrative.

#include "dxgi_exports.h"
#include "log.h"

// Trap functions live in dtf_traps.cpp. Every pfn_FOO slot is
// initialized at compile time to one of these traps so that the asm
// stubs in dxgi_stubs.asm never jump through a null pointer — necessary
// because the Windows loader's app-compat shim layer (apphelp.dll) calls
// some dxgi exports (notably SetAppCompatStringPointer) BEFORE our
// DllMain runs. See dtf_traps.cpp's header comment for the full story.
//
// Five categories of trap:
//   - dtf_trap_pre_resolve: generic, returns 0. Safe for compat-pass
//     exports and any undocumented private export. Most exports use this.
//   - dtf_trap_CreateDXGIFactory: zeros the (REFIID, void**) out-pointer
//     and returns DXGI_ERROR_NOT_FOUND. Used for CreateDXGIFactory,
//     CreateDXGIFactory1.
//   - dtf_trap_CreateDXGIFactory2: same shape but with the extra Flags
//     parameter. Used for CreateDXGIFactory2 and DXGIGetDebugInterface1.
//   - dtf_trap_HRESULT_void: zero-argument, returns DXGI_ERROR_NOT_FOUND.
//     Used for DXGIDeclareAdapterRemovalSupport.
//   - dtf_trap_HRESULT_HANDLE: takes a HANDLE adapter argument (signature
//     community-reverse-engineered; see dtf_traps.cpp), returns
//     DXGI_ERROR_NOT_FOUND. Used for DXGIDisableVBlankVirtualization.
//
// After DllMain runs, every pfn_FOO that the host's System32 dxgi
// actually exports is overwritten with the real address; only exports
// missing on the host's Windows version stay pointing at their trap.
extern "C" void* WINAPI dtf_trap_pre_resolve(void);
extern "C" HRESULT WINAPI dtf_trap_CreateDXGIFactory(REFIID, void**);
extern "C" HRESULT WINAPI dtf_trap_CreateDXGIFactory2(UINT, REFIID, void**);
extern "C" HRESULT WINAPI dtf_trap_HRESULT_void(void);
extern "C" HRESULT WINAPI dtf_trap_HRESULT_HANDLE(HANDLE);

// =====================================================================
// THE pfn_FOO TABLE (the most important block of code in this DLL)
// =====================================================================
//
// These 20 pointers are the .data slots that the asm tail-jump stubs
// in dxgi_stubs.asm dereference. Each `jmp QWORD PTR [pfn_FOO]` reads
// 8 bytes from one of these slots and jumps to that address.
//
// They are INITIALIZED AT COMPILE TIME to point at trap functions in
// dtf_traps.cpp.
//
// A normal C++ programmer would write:
//     void* pfn_CreateDXGIFactory = nullptr;
// and resolve them at runtime in DllMain. **DO NOT change this code to
// that.** The first version of this DLL did exactly that, and it
// crashed every install on every machine. The crash was:
//
//   - Process starts, OS loader maps our dxgi.dll.
//   - OS loader's app-compat shim layer (apphelp.dll) runs its
//     compat-fixup pass on the just-loaded DLL. For dxgi, that pass
//     calls SetAppCompatStringPointer to apply per-app compat data.
//   - apphelp resolves SetAppCompatStringPointer via our IAT → gets
//     our exported stub address.
//   - apphelp calls our stub → jmp QWORD PTR [pfn_SetAppCompatStringPointer]
//   - pfn_SetAppCompatStringPointer is still nullptr (DllMain hasn't
//     run yet — apphelp runs BEFORE DllMain).
//   - Jump to address 0 → fault at 0 → process death.
//   - User sees: DXMD crashes instantly on launch, no log file
//     created (because our log_init never got to run either).
//
// The fix below is to initialize every slot at compile time to a
// non-null trap. When apphelp does its pre-DllMain calls, the stubs
// land on the traps, the traps return a safe value, apphelp moves on,
// then the loader actually runs our DllMain and we overwrite these
// pointers with real System32 dxgi addresses.
//
// load_system_dxgi_and_resolve() (below in this file) does that
// overwrite. After it runs, "trap" pointers only remain in slots
// where the host's Windows version doesn't actually export that
// function (rare; mostly Windows-10+-only exports on older Windows).
//
// Why different traps for different exports? Because returning 0 means
// different things to different APIs. For SetAppCompatStringPointer
// (called by apphelp), 0 means "no shim applied, continue" — safe.
// For CreateDXGIFactory, returning 0 (S_OK) would mean "factory created
// successfully" and the caller would dereference the uninitialized
// output pointer and crash. So:
//
//   - dtf_trap_pre_resolve: generic, returns 0 in RAX. Used for the
//     compat-namespace exports apphelp actually calls, plus
//     undocumented private exports where 0 happens to be a safe
//     default.
//   - dtf_trap_CreateDXGIFactory (REFIID, void**): zeros the out-pointer
//     and returns DXGI_ERROR_NOT_FOUND. Used for CreateDXGIFactory and
//     CreateDXGIFactory1.
//   - dtf_trap_CreateDXGIFactory2 (UINT, REFIID, void**): same shape with
//     the extra Flags param. Used for CreateDXGIFactory2 and
//     DXGIGetDebugInterface1.
//   - dtf_trap_HRESULT_void: zero-arg, returns DXGI_ERROR_NOT_FOUND.
//     Used for DXGIDeclareAdapterRemovalSupport.
//   - dtf_trap_HRESULT_HANDLE: HANDLE arg, returns DXGI_ERROR_NOT_FOUND.
//     Used for DXGIDisableVBlankVirtualization.
//
// These pointers MUST be named `pfn_<ExportName>` exactly — the asm
// stubs in dxgi_stubs.asm reference them by those names via MASM's
// `EXTERN pfn_FOO:QWORD` declarations.
extern "C" {
    void* pfn_ApplyCompatResolutionQuirking    = (void*)&dtf_trap_pre_resolve;
    void* pfn_CompatString                     = (void*)&dtf_trap_pre_resolve;
    void* pfn_CompatValue                      = (void*)&dtf_trap_pre_resolve;
    void* pfn_DXGIDumpJournal                  = (void*)&dtf_trap_pre_resolve;
    void* pfn_PIXBeginCapture                  = (void*)&dtf_trap_pre_resolve;
    void* pfn_PIXEndCapture                    = (void*)&dtf_trap_pre_resolve;
    void* pfn_PIXGetCaptureState               = (void*)&dtf_trap_pre_resolve;
    void* pfn_SetAppCompatStringPointer        = (void*)&dtf_trap_pre_resolve;
    void* pfn_UpdateHMDEmulationStatus         = (void*)&dtf_trap_pre_resolve;
    void* pfn_CreateDXGIFactory                = (void*)&dtf_trap_CreateDXGIFactory;
    void* pfn_CreateDXGIFactory1               = (void*)&dtf_trap_CreateDXGIFactory;
    void* pfn_CreateDXGIFactory2               = (void*)&dtf_trap_CreateDXGIFactory2;
    void* pfn_DXGID3D10CreateDevice            = (void*)&dtf_trap_pre_resolve;
    void* pfn_DXGID3D10CreateLayeredDevice     = (void*)&dtf_trap_pre_resolve;
    void* pfn_DXGID3D10GetLayeredDeviceSize    = (void*)&dtf_trap_pre_resolve;
    void* pfn_DXGID3D10RegisterLayers          = (void*)&dtf_trap_pre_resolve;
    void* pfn_DXGIDeclareAdapterRemovalSupport = (void*)&dtf_trap_HRESULT_void;
    void* pfn_DXGIDisableVBlankVirtualization  = (void*)&dtf_trap_HRESULT_HANDLE;
    void* pfn_DXGIGetDebugInterface1           = (void*)&dtf_trap_CreateDXGIFactory2;
    void* pfn_DXGIReportAdapterConfiguration   = (void*)&dtf_trap_pre_resolve;
}

namespace dtf {

struct ExportSpec {
    const char* name;
    void**      slot;
};

static const ExportSpec g_exports[] = {
    { "ApplyCompatResolutionQuirking",    &pfn_ApplyCompatResolutionQuirking    },
    { "CompatString",                     &pfn_CompatString                     },
    { "CompatValue",                      &pfn_CompatValue                      },
    { "DXGIDumpJournal",                  &pfn_DXGIDumpJournal                  },
    { "PIXBeginCapture",                  &pfn_PIXBeginCapture                  },
    { "PIXEndCapture",                    &pfn_PIXEndCapture                    },
    { "PIXGetCaptureState",               &pfn_PIXGetCaptureState               },
    { "SetAppCompatStringPointer",        &pfn_SetAppCompatStringPointer        },
    { "UpdateHMDEmulationStatus",         &pfn_UpdateHMDEmulationStatus         },
    { "CreateDXGIFactory",                &pfn_CreateDXGIFactory                },
    { "CreateDXGIFactory1",               &pfn_CreateDXGIFactory1               },
    { "CreateDXGIFactory2",               &pfn_CreateDXGIFactory2               },
    { "DXGID3D10CreateDevice",            &pfn_DXGID3D10CreateDevice            },
    { "DXGID3D10CreateLayeredDevice",     &pfn_DXGID3D10CreateLayeredDevice     },
    { "DXGID3D10GetLayeredDeviceSize",    &pfn_DXGID3D10GetLayeredDeviceSize    },
    { "DXGID3D10RegisterLayers",          &pfn_DXGID3D10RegisterLayers          },
    { "DXGIDeclareAdapterRemovalSupport", &pfn_DXGIDeclareAdapterRemovalSupport },
    { "DXGIDisableVBlankVirtualization",  &pfn_DXGIDisableVBlankVirtualization  },
    { "DXGIGetDebugInterface1",           &pfn_DXGIGetDebugInterface1           },
    { "DXGIReportAdapterConfiguration",   &pfn_DXGIReportAdapterConfiguration   },
};

HMODULE load_system_dxgi_and_resolve() {
    // =================================================================
    // UNUSUAL THING: we're proxying dxgi.dll, but ALSO we're loading
    // the real dxgi.dll. How do we get the real one when our shim
    // has the same name?
    // =================================================================
    //
    // We build the path absolutely from GetSystemDirectoryW (which on
    // every supported Windows version resolves to C:\Windows\System32,
    // or whatever the user's actual system directory is). The result
    // is something like:
    //
    //     C:\Windows\System32\dxgi.dll
    //
    // We then LoadLibraryExW that ABSOLUTE PATH. Because the path is
    // fully qualified, Windows does no search-path lookup at all — it
    // opens that exact file. This cannot accidentally load our own
    // proxy (which is at <game>\retail\dxgi.dll, not in System32) or
    // any other dxgi.dll the user might have lying around.
    //
    // SECURITY-CURIOUS READERS: yes, an attacker who could already
    // overwrite C:\Windows\System32\dxgi.dll already owns your machine
    // far worse than this DLL can make it. That's outside our threat
    // model. The build-an-absolute-path-from-System32 pattern is the
    // standard MSDN-recommended way to do this.
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return nullptr;
    if (path[n - 1] != L'\\') {
        path[n] = L'\\';
        path[n + 1] = 0;
    }
    wcscat_s(path, MAX_PATH, L"dxgi.dll");

    // About the LOAD_WITH_ALTERED_SEARCH_PATH flag: it sounds scary
    // ("altered search path" → "attacker controlling DLL search?")
    // but it's the OPPOSITE here. The flag tells the loader: "for any
    // DEPENDENT DLLs that the file I'm loading needs, search starting
    // from THAT file's directory instead of mine."
    //
    // Concretely: when the loader maps System32\dxgi.dll, dxgi has
    // its own dependencies (d3d11.dll, etc.) which should be resolved
    // from System32, NOT from <game>\retail\ (which is our load
    // directory). Without this flag, dxgi's dependencies would search
    // <game>\retail\ first — and if a malicious actor had planted
    // d3d11.dll there, dxgi would load THAT. The flag prevents
    // exactly that attack by pinning the search to System32.
    //
    // Could also use LOAD_LIBRARY_SEARCH_SYSTEM32 (Win8+) for the
    // same effect with stricter semantics, but the absolute-path +
    // ALTERED_SEARCH_PATH combo works on Win7 SP1 (our minimum
    // supported OS, matching DXMD's minimum).
    HMODULE h = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        log_line("FATAL: could not load real dxgi: %ls (err %lu)", path, GetLastError());
        return nullptr;
    }
    log_line("Real dxgi loaded: %ls @ %p", path, h);

    int resolved = 0;
    int missing  = 0;
    for (const auto& e : g_exports) {
        FARPROC p = GetProcAddress(h, e.name);
        if (p) {
            *e.slot = reinterpret_cast<void*>(p);
            resolved++;
        } else {
            // Not present on this Windows version. The slot keeps
            // pointing at its compile-time trap (see dtf_traps.cpp).
            // For HRESULT-returning APIs that's a typed failure stub
            // (DXGI_ERROR_NOT_FOUND); for others it's the generic
            // dtf_trap_pre_resolve. Either way: NOT a null pointer.
            missing++;
            log_line("WARN: real dxgi missing export %s (this Windows: err %lu)",
                     e.name, GetLastError());
        }
    }
    log_line("dxgi exports resolved: %d ok, %d missing.", resolved, missing);
    return h;
}

void free_system_dxgi(HMODULE h) {
    if (h) FreeLibrary(h);
}

} // namespace dtf
