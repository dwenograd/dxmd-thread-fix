#include "dxgi_exports.h"
#include "log.h"

// Trap functions live in dtf_traps.cpp. Every pfn_FOO slot is
// initialized at compile time to one of these traps so that the asm
// stubs in dxgi_stubs.asm never jump through a null pointer — necessary
// because the Windows loader's app-compat shim layer (apphelp.dll) calls
// some dxgi exports (notably SetAppCompatStringPointer) BEFORE our
// DllMain runs. See dtf_traps.cpp's header comment for the full story.
//
// Three categories of trap:
//   - dtf_trap_pre_resolve: generic, returns 0. Safe for compat-pass
//     exports and any undocumented private export. Most exports use this.
//   - dtf_trap_CreateDXGIFactory: zeros the out-pointer and returns
//     DXGI_ERROR_NOT_FOUND. Used for CreateDXGIFactory, CreateDXGIFactory1.
//   - dtf_trap_CreateDXGIFactory2: same shape but with the extra Flags
//     parameter. Used for CreateDXGIFactory2 and DXGIGetDebugInterface1.
//   - dtf_trap_HRESULT_void: zero-argument, returns DXGI_ERROR_NOT_FOUND.
//     Used for DXGIDeclareAdapterRemovalSupport and DXGIDisableVBlankVirtualization.
//
// After DllMain runs, every pfn_FOO that the host's System32 dxgi
// actually exports is overwritten with the real address; only exports
// missing on the host's Windows version stay pointing at their trap.
extern "C" void* WINAPI dtf_trap_pre_resolve(void);
extern "C" HRESULT WINAPI dtf_trap_CreateDXGIFactory(REFIID, void**);
extern "C" HRESULT WINAPI dtf_trap_CreateDXGIFactory2(UINT, REFIID, void**);
extern "C" HRESULT WINAPI dtf_trap_HRESULT_void(void);

// These pointers are referenced by the tail-jump stubs in dxgi_stubs.asm.
// Names MUST be `pfn_<ExportName>`.
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
    void* pfn_DXGIDisableVBlankVirtualization  = (void*)&dtf_trap_HRESULT_void;
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
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return nullptr;
    if (path[n - 1] != L'\\') {
        path[n] = L'\\';
        path[n + 1] = 0;
    }
    wcscat_s(path, MAX_PATH, L"dxgi.dll");

    // LOAD_LIBRARY_SEARCH_SYSTEM32 would also do it on Win8+, but a full
    // absolute path is the most portable.
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
