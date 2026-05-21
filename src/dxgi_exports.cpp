// dxgi_exports.cpp - the pfn_FOO globals that the asm stubs in
// dxgi_stubs.asm tail-jump through, plus the resolver that overwrites
// them with real System32 dxgi addresses.
//
// Each pfn_FOO is initialized at COMPILE TIME to a trap function (see
// dtf_traps.cpp for the full rationale). This is essential because
// Windows' apphelp compat pass calls some dxgi exports before our
// DllMain runs; if pfn_FOO were nullptr, the asm stub would jump to
// address 0 and the process would die pre-DllMain.

#include "dxgi_exports.h"
#include "log.h"

// Traps live in dtf_traps.cpp. See that file for the full story.
extern "C" void* WINAPI dtf_trap_pre_resolve(void);
extern "C" HRESULT WINAPI dtf_trap_CreateDXGIFactory(REFIID, void**);
extern "C" HRESULT WINAPI dtf_trap_CreateDXGIFactory2(UINT, REFIID, void**);
extern "C" HRESULT WINAPI dtf_trap_HRESULT_void(void);
extern "C" HRESULT WINAPI dtf_trap_HRESULT_HANDLE(HANDLE);

// The pfn_FOO table. Each slot is the .data location that the matching
// asm tail-jump stub in dxgi_stubs.asm reads. Symbol names must match
// the `EXTERN pfn_FOO:QWORD` declarations in the asm.
//
// **Do not change these initializers to nullptr.** See dtf_traps.cpp.
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
    // Build an absolute path to System32\dxgi.dll. Because the path is
    // fully qualified, Windows skips all search-path resolution — we
    // can't accidentally load our own proxy (in <game>\retail\) or any
    // other dxgi.dll on the search path.
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return nullptr;
    if (path[n - 1] != L'\\') {
        path[n] = L'\\';
        path[n + 1] = 0;
    }
    wcscat_s(path, MAX_PATH, L"dxgi.dll");

    // LOAD_WITH_ALTERED_SEARCH_PATH tells the loader to resolve the
    // loaded file's own dependencies starting from that file's
    // directory (i.e. System32) rather than from the host process
    // directory. Without it, dxgi's dependencies could resolve out of
    // the game folder if a same-named DLL had been planted there.
    // LOAD_LIBRARY_SEARCH_SYSTEM32 would be stricter but isn't
    // available on our Win7 SP1 baseline.
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
            // Export not present on this Windows version. Slot keeps
            // its compile-time trap (a typed failure stub, not null).
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
