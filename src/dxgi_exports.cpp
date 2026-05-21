#include "dxgi_exports.h"
#include "log.h"

// These pointers are referenced by the tail-jump stubs in
// dxgi_stubs.asm. Names MUST be `pfn_<ExportName>`.
extern "C" {
    void* pfn_ApplyCompatResolutionQuirking    = nullptr;
    void* pfn_CompatString                     = nullptr;
    void* pfn_CompatValue                      = nullptr;
    void* pfn_DXGIDumpJournal                  = nullptr;
    void* pfn_PIXBeginCapture                  = nullptr;
    void* pfn_PIXEndCapture                    = nullptr;
    void* pfn_PIXGetCaptureState               = nullptr;
    void* pfn_SetAppCompatStringPointer        = nullptr;
    void* pfn_UpdateHMDEmulationStatus         = nullptr;
    void* pfn_CreateDXGIFactory                = nullptr;
    void* pfn_CreateDXGIFactory1               = nullptr;
    void* pfn_CreateDXGIFactory2               = nullptr;
    void* pfn_DXGID3D10CreateDevice            = nullptr;
    void* pfn_DXGID3D10CreateLayeredDevice     = nullptr;
    void* pfn_DXGID3D10GetLayeredDeviceSize    = nullptr;
    void* pfn_DXGID3D10RegisterLayers          = nullptr;
    void* pfn_DXGIDeclareAdapterRemovalSupport = nullptr;
    void* pfn_DXGIDisableVBlankVirtualization  = nullptr;
    void* pfn_DXGIGetDebugInterface1           = nullptr;
    void* pfn_DXGIReportAdapterConfiguration   = nullptr;
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
            // Not present on this Windows version. The export's stub
            // will jmp to nullptr if invoked, but the game typically
            // only calls exports it knows exist on its target OS.
            missing++;
            log_line("WARN: real dxgi missing export %s (this Windows: %lu)",
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
