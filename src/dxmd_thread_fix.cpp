// dxmd_thread_fix.cpp - single translation unit for the whole fix.
//
// What this DLL does: proxies dxgi.dll for DXMD.exe and installs
// MinHook detours on 6 CPU-topology APIs so the game sees 8 logical
// processors instead of the host's real count (which crashes the game
// on high-core CPUs). See docs/DESIGN.md for the architectural story.
//
// Source layout: the entire fix is in this one file plus three
// non-C++ siblings the toolchain requires separately:
//   - dxgi_stubs.asm - 20 single-instruction tail-jump export stubs
//   - dxgi.def       - exports table (name + ordinal preservation)
//   - version.rc     - VERSIONINFO resource
//
// Build order in this file (top to bottom is also dependency order):
//   SECTION 1  dxgi trap functions
//   SECTION 2  pfn_FOO globals (consumed by dxgi_stubs.asm)
//   SECTION 3  path utilities
//   SECTION 4  logger
//   SECTION 5  fake CPU topology
//   SECTION 6  real-dxgi loader and export resolution
//   SECTION 7  CPU hook detours and installer
//   SECTION 8  DllMain and attach/detach

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "../third_party/minhook/include/MinHook.h"

#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND 0x887A0002L
#endif


// =========================================================================
// SECTION 1: dxgi trap functions
// =========================================================================
//
// Every pfn_FOO global in SECTION 2 is initialized at compile time to
// point at one of these traps. This is load-bearing: Windows'
// app-compat shim engine (apphelp.dll's SE_DllLoaded path) calls some
// dxgi exports - notably SetAppCompatStringPointer - BEFORE our
// DllMain runs. If pfn_FOO were nullptr at that point, the asm stub's
// `jmp QWORD PTR [pfn_FOO]` would jump to address 0 and the process
// would die before our log file was even created. Confirmed via
// minidump analysis on real DXMD crash dumps.
//
// **Do not "fix" the pfn_FOO globals to default to nullptr.**
//
// After load_system_dxgi_and_resolve() (SECTION 6) runs, each slot
// for which the host's Windows actually exports the symbol is
// overwritten with the real address. Traps remain live only for
// exports the host's Windows version doesn't provide.
//
// `__declspec(noinline)` + `extern "C"` keeps these as real symbols
// under LTCG. Release builds may still apply /OPT:ICF and fold trap
// functions with identical bodies (e.g. dtf_trap_HRESULT_void and
// dtf_trap_HRESULT_HANDLE) to the same address - functionally fine,
// just expected when inspecting the binary.

extern "C" __declspec(noinline) void* WINAPI dtf_trap_pre_resolve(void) {
    return nullptr;
}

// CreateDXGIFactory / CreateDXGIFactory1. Zero the out-pointer BEFORE
// returning failure: a caller that ignores the HRESULT will then fault
// cleanly on `(*ppFactory)->...` rather than reading random memory.
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

// DXGIDisableVBlankVirtualization. HANDLE param is for source readability;
// the trap ignores it.
extern "C" __declspec(noinline) HRESULT WINAPI
dtf_trap_HRESULT_HANDLE(HANDLE /*hAdapter*/) {
    return DXGI_ERROR_NOT_FOUND;
}


// =========================================================================
// SECTION 2: pfn_FOO globals (consumed by dxgi_stubs.asm)
// =========================================================================
//
// The asm stubs in dxgi_stubs.asm tail-jump through these pointers. The
// symbol names must match the `EXTERN pfn_FOO:QWORD` declarations in
// the asm. Each is initialized at COMPILE TIME to a trap function from
// SECTION 1 (see that section's header for why).

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

// =========================================================================
// SECTION 3: path utilities
// =========================================================================
//
// Long-path-safe module path helpers. Uses the MSDN double-the-buffer
// retry pattern for GetModuleFileNameW, capped at 32K wchars.

static HANDLE process_heap() {
    static HANDLE h = GetProcessHeap();
    return h;
}

static void free_wstr(wchar_t* s) {
    if (s) HeapFree(process_heap(), 0, s);
}

static wchar_t* get_module_path(HMODULE module) {
    HANDLE heap = process_heap();
    if (!heap) return nullptr;

    // GetModuleFileNameW returns the number of wchars written; equal to
    // the buffer size means truncation (it doesn't report the required
    // size). Grow and retry. A fixed MAX_PATH buffer would silently
    // fail on long-path-enabled installs.
    const DWORD MAX_BUF = 32 * 1024;
    DWORD bufLen = MAX_PATH;
    wchar_t* path = static_cast<wchar_t*>(HeapAlloc(heap, 0, bufLen * sizeof(wchar_t)));
    if (!path) return nullptr;

    DWORD n = 0;
    bool success = false;
    for (;;) {
        n = GetModuleFileNameW(module, path, bufLen);
        if (n == 0) break;                          // API error
        if (n < bufLen) { success = true; break; }  // fits

        if (bufLen >= MAX_BUF) break;
        DWORD newLen = bufLen * 2;
        if (newLen > MAX_BUF) newLen = MAX_BUF;

        // HeapReAlloc on failure leaves the original `path` valid (like
        // realloc). Capture into a temp so we don't lose it on NULL.
        wchar_t* newPath = static_cast<wchar_t*>(
            HeapReAlloc(heap, 0, path, newLen * sizeof(wchar_t)));
        if (!newPath) break;
        path   = newPath;
        bufLen = newLen;
    }
    if (!success) { HeapFree(heap, 0, path); return nullptr; }
    return path;
}

static wchar_t* get_module_dir(HMODULE module) {
    wchar_t* path = get_module_path(module);
    if (!path) return nullptr;
    size_t len = 0;
    while (path[len]) ++len;
    while (len > 0 && path[len - 1] != L'\\' && path[len - 1] != L'/') {
        --len;
    }
    path[len] = 0;
    return path;
}

static bool wstr_ieq_ascii(const wchar_t* a, const wchar_t* b) {
    while (*a && *b) {
        wchar_t ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca + (L'a' - L'A'));
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb + (L'a' - L'A'));
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}


// =========================================================================
// SECTION 4: logger
// =========================================================================
//
// Thread-safe file logger. Each log_line() opens, writes, and closes
// the file under a critical section so writes survive a process crash.
// Log lives next to the DLL as dxmd-thread-fix.log.

static CRITICAL_SECTION g_lock;
static bool             g_lock_inited = false;
static wchar_t*         g_log_path = nullptr;
static int              g_level = 1;
static bool             g_log_inited = false;

static void compute_log_path(HMODULE self) {
    free_wstr(g_log_path);
    g_log_path = nullptr;

    wchar_t* dir = get_module_dir(self);
    if (!dir) return;

    static const wchar_t kLogName[] = L"dxmd-thread-fix.log";
    size_t dirLen = 0; while (dir[dirLen]) ++dirLen;
    size_t pathLen = dirLen + (sizeof(kLogName) / sizeof(wchar_t));  // includes null
    g_log_path = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, pathLen * sizeof(wchar_t)));
    if (!g_log_path) { free_wstr(dir); return; }
    for (size_t i = 0; i < dirLen; ++i) g_log_path[i] = dir[i];
    for (size_t i = 0; ; ++i) {
        g_log_path[dirLen + i] = kLogName[i];
        if (kLogName[i] == 0) break;
    }
    free_wstr(dir);
}

// Truncating open (CREATE_ALWAYS): each game launch starts a fresh log.
// Permissive sharing matches log_line()'s so truncation succeeds even
// when the prior session's file is still open in an editor.
static void log_open() {
    if (!g_log_inited) return;
    if (!g_log_path || g_log_path[0] == 0) return;
    HANDLE h = CreateFileW(g_log_path, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void log_init_deferred(HMODULE self) {
    if (g_log_inited) return;
    InitializeCriticalSection(&g_lock);
    g_lock_inited = true;
    g_level = 0;
    compute_log_path(self);
    g_log_inited = true;
}

static void log_shutdown() {
    if (!g_log_inited) return;
    if (g_lock_inited) {
        DeleteCriticalSection(&g_lock);
        g_lock_inited = false;
    }
    free_wstr(g_log_path);
    g_log_path = nullptr;
    g_log_inited = false;
}

static int log_level() { return g_level; }

static void log_set_level(int level) {
    const int prev = g_level;
    g_level = level;
    if (prev == 0 && level > 0) log_open();
}

static void log_line(const char* fmt, ...) {
    if (!g_log_inited || g_level <= 0) return;
    if (!g_log_path || g_log_path[0] == 0) return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) {
        msg[sizeof(msg) - 1] = '\0';
        n = static_cast<int>(strlen(msg));
    }
    if (n <= 0) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[64];
    int pn = _snprintf_s(prefix, sizeof(prefix), _TRUNCATE,
                         "[%02u:%02u:%02u.%03u] ",
                         st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (pn <= 0) return;

    EnterCriticalSection(&g_lock);
    // Open-write-close per line so each line is committed before the
    // next call - the whole point of this DLL is that DXMD crashes,
    // and we want the last logged lines to survive on disk.
    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, prefix, static_cast<DWORD>(pn), &written, nullptr);
        WriteFile(h, msg,    static_cast<DWORD>(n),  &written, nullptr);
        WriteFile(h, "\r\n", 2, &written, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_lock);
}

// Returns true exactly once per `flag` via atomic CAS. Used to throttle
// LogLevel=1 output so each hooked API logs only on its first call.
static bool first_hit(LONG volatile* flag) {
    return InterlockedCompareExchange(flag, 1, 0) == 0;
}


// =========================================================================
// SECTION 5: fake CPU topology
// =========================================================================
//
// Single source of truth for what we report to the game. Every hook in
// SECTION 7 reads from g_topology so all hooks return consistent values
// (otherwise game code could allocate for one count and schedule for
// another).
//
// CALL-ORDER REQUIREMENT: set_topology() must run BEFORE
// install_cpu_hooks() (SECTION 7). Once detours are installed, calling
// the kernel32 topology APIs (even via a freshly-resolved
// GetProcAddress pointer) enters our own detour code, which would feed
// our lies back into "real" topology.

struct FakeTopology {
    // Values reported to the game:
    WORD logical_processors = 8;
    WORD groups             = 1;
    WORD numa_nodes         = 1;

    // Real system values, captured at init for logging:
    WORD real_logical_processors = 0;
    WORD real_groups             = 0;
};

static FakeTopology g_topology;

static const FakeTopology& topology() { return g_topology; }

static DWORD_PTR first_n_bits_mask(WORD n) {
    if (n == 0) return 0;
    if (n >= sizeof(DWORD_PTR) * 8) return static_cast<DWORD_PTR>(-1);
    return (static_cast<DWORD_PTR>(1) << n) - 1;
}

static void set_topology(WORD desired) {
    using PFN_GNSI = void  (WINAPI*)(LPSYSTEM_INFO);
    using PFN_GAPC = DWORD (WINAPI*)(WORD);
    using PFN_GAGC = WORD  (WINAPI*)();

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_GNSI real_GetNativeSystemInfo = k32 ? reinterpret_cast<PFN_GNSI>(
        GetProcAddress(k32, "GetNativeSystemInfo")) : nullptr;
    PFN_GAPC real_GetActiveProcessorCount = k32 ? reinterpret_cast<PFN_GAPC>(
        GetProcAddress(k32, "GetActiveProcessorCount")) : nullptr;
    PFN_GAGC real_GetActiveProcessorGroupCount = k32 ? reinterpret_cast<PFN_GAGC>(
        GetProcAddress(k32, "GetActiveProcessorGroupCount")) : nullptr;

    // GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) is the system total.
    // GetNativeSystemInfo only reports the calling thread's current
    // processor group, which on >64-CPU machines undercounts.
    WORD real_total = 0;
    if (real_GetActiveProcessorCount) {
        real_total = static_cast<WORD>(real_GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    } else if (real_GetNativeSystemInfo) {
        SYSTEM_INFO si{};
        real_GetNativeSystemInfo(&si);
        real_total = static_cast<WORD>(si.dwNumberOfProcessors);
    } else {
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        real_total = static_cast<WORD>(si.dwNumberOfProcessors);
    }

    g_topology.real_logical_processors = real_total;
    g_topology.real_groups = real_GetActiveProcessorGroupCount
        ? real_GetActiveProcessorGroupCount() : 1;

    WORD clamped = desired;
    if (clamped < 1) clamped = 1;
    if (clamped > g_topology.real_logical_processors) {
        clamped = g_topology.real_logical_processors;
    }
    // Single fake group means dwActiveProcessorMask must fit in 64 bits.
    if (clamped > 64) clamped = 64;
    // Guard against real_total == 0 (Wine/Proton/broken hooks); a 1-CPU
    // fake is degraded but functional, while 0 would produce empty
    // affinity masks that hang the game's thread pools.
    if (clamped < 1) clamped = 1;

    g_topology.logical_processors = clamped;
    g_topology.groups             = 1;
    g_topology.numa_nodes         = 1;
}


// =========================================================================
// SECTION 6: real-dxgi loader and export resolution
// =========================================================================
//
// Loads System32\dxgi.dll and overwrites each pfn_FOO global (SECTION 2)
// with its real address. After this runs, the asm stubs in
// dxgi_stubs.asm tail-jump to the real dxgi.

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

static HMODULE load_system_dxgi_and_resolve() {
    // Build an absolute path to System32\dxgi.dll. Because the path is
    // fully qualified, Windows skips all search-path resolution - we
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

    // LOAD_WITH_ALTERED_SEARCH_PATH: resolve the loaded file's own
    // dependencies starting from System32 rather than from the host
    // process directory. Without it, dxgi's dependencies could resolve
    // out of the game folder if a same-named DLL had been planted
    // there. LOAD_LIBRARY_SEARCH_SYSTEM32 would be stricter but isn't
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

static void free_system_dxgi(HMODULE h) {
    if (h) FreeLibrary(h);
}


// =========================================================================
// SECTION 7: CPU hook detours and installer
// =========================================================================
//
// install_cpu_hooks() uses a three-pass install for clean fail
// semantics: (1) MH_CreateHook every spec into a local array without
// publishing trampolines; (2) publish trampolines to the g_real_*
// globals; (3) MH_EnableHook(MH_ALL_HOOKS). Splitting create from
// publish means a Pass-1 failure can clean up without leaving any
// g_real_* pointing at memory MinHook is about to free.
//
// The 6 topology APIs are required all-or-nothing. If any fails the
// whole install is torn down and FIX STATUS: INACTIVE is logged - a
// half-applied fix would let middleware see "8" from one API and "64"
// from another. SetThreadAffinityMask is opt-in (compile-time false in
// SECTION 8); its install failure is non-fatal.

using PFN_GetSystemInfo                 = void      (WINAPI*)(LPSYSTEM_INFO);
using PFN_GetNativeSystemInfo           = void      (WINAPI*)(LPSYSTEM_INFO);
using PFN_GetActiveProcessorCount       = DWORD     (WINAPI*)(WORD);
using PFN_GetMaximumProcessorCount      = DWORD     (WINAPI*)(WORD);
using PFN_GetActiveProcessorGroupCount  = WORD      (WINAPI*)();
using PFN_GetMaximumProcessorGroupCount = WORD      (WINAPI*)();
using PFN_SetThreadAffinityMask         = DWORD_PTR (WINAPI*)(HANDLE, DWORD_PTR);

static PFN_GetSystemInfo                 g_real_GetSystemInfo                 = nullptr;
static PFN_GetNativeSystemInfo           g_real_GetNativeSystemInfo           = nullptr;
static PFN_GetActiveProcessorCount       g_real_GetActiveProcessorCount       = nullptr;
static PFN_GetMaximumProcessorCount      g_real_GetMaximumProcessorCount      = nullptr;
static PFN_GetActiveProcessorGroupCount  g_real_GetActiveProcessorGroupCount  = nullptr;
static PFN_GetMaximumProcessorGroupCount g_real_GetMaximumProcessorGroupCount = nullptr;
static PFN_SetThreadAffinityMask         g_real_SetThreadAffinityMask         = nullptr;

// First-hit flags. Each transitions 0->1 atomically on the first call
// to its hooked API. At LogLevel=1 we log only that first call, then
// stay silent - otherwise high-frequency APIs like GetSystemInfo would
// balloon the log to gigabytes.
static LONG volatile g_fh_GetSystemInfo                 = 0;
static LONG volatile g_fh_GetNativeSystemInfo           = 0;
static LONG volatile g_fh_GetActiveProcessorCount       = 0;
static LONG volatile g_fh_GetMaximumProcessorCount      = 0;
static LONG volatile g_fh_GetActiveProcessorGroupCount  = 0;
static LONG volatile g_fh_GetMaximumProcessorGroupCount = 0;
static LONG volatile g_fh_SetThreadAffinityMask         = 0;

static bool g_clamp_affinity = false;

static void rewrite_system_info(LPSYSTEM_INFO si) {
    if (!si) return;
    const auto& topo = topology();
    si->dwNumberOfProcessors  = topo.logical_processors;
    si->dwActiveProcessorMask = first_n_bits_mask(topo.logical_processors);
}

static void WINAPI Hooked_GetSystemInfo(LPSYSTEM_INFO si) {
    g_real_GetSystemInfo(si);
    const DWORD original = si ? si->dwNumberOfProcessors : 0;
    rewrite_system_info(si);
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetSystemInfo))) {
        log_line("GetSystemInfo: %lu -> %u", original, topology().logical_processors);
    }
}

static void WINAPI Hooked_GetNativeSystemInfo(LPSYSTEM_INFO si) {
    g_real_GetNativeSystemInfo(si);
    const DWORD original = si ? si->dwNumberOfProcessors : 0;
    rewrite_system_info(si);
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetNativeSystemInfo))) {
        log_line("GetNativeSystemInfo: %lu -> %u", original, topology().logical_processors);
    }
}

static DWORD WINAPI Hooked_GetActiveProcessorCount(WORD group) {
    const auto& topo = topology();
    DWORD real = g_real_GetActiveProcessorCount(group);
    DWORD reported;
    if (group == ALL_PROCESSOR_GROUPS || group == 0) {
        reported = topo.logical_processors;
    } else {
        reported = 0; // we claim only one group
    }
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetActiveProcessorCount))) {
        log_line("GetActiveProcessorCount(group=%u): %lu -> %lu",
                 (unsigned)group, real, reported);
    }
    return reported;
}

static DWORD WINAPI Hooked_GetMaximumProcessorCount(WORD group) {
    const auto& topo = topology();
    DWORD real = g_real_GetMaximumProcessorCount(group);
    DWORD reported;
    if (group == ALL_PROCESSOR_GROUPS || group == 0) {
        reported = topo.logical_processors;
    } else {
        reported = 0;
    }
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetMaximumProcessorCount))) {
        log_line("GetMaximumProcessorCount(group=%u): %lu -> %lu",
                 (unsigned)group, real, reported);
    }
    return reported;
}

static WORD WINAPI Hooked_GetActiveProcessorGroupCount(void) {
    WORD real = g_real_GetActiveProcessorGroupCount();
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetActiveProcessorGroupCount))) {
        log_line("GetActiveProcessorGroupCount: %u -> 1", (unsigned)real);
    }
    return 1;
}

static WORD WINAPI Hooked_GetMaximumProcessorGroupCount(void) {
    WORD real = g_real_GetMaximumProcessorGroupCount();
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_GetMaximumProcessorGroupCount))) {
        log_line("GetMaximumProcessorGroupCount: %u -> 1", (unsigned)real);
    }
    return 1;
}

static DWORD_PTR WINAPI Hooked_SetThreadAffinityMask(HANDLE hThread, DWORD_PTR mask) {
    // The other 6 hooks tell the game the system has only N logical
    // processors in a single group, but the OS still really has 64+
    // CPUs across multiple groups. A SetThreadAffinityMask mask applies
    // within the thread's CURRENT group, not globally. If middleware
    // asks for a high-numbered bit, the OS would pin the thread to
    // that CPU - inconsistent with our lie.
    //
    // When g_clamp_affinity is true, intersect the requested mask with
    // the first-N-bits mask. If the result is empty, fall back to the
    // full allowed mask - passing 0 would return failure to a caller
    // that may treat that as fatal.
    //
    // Opt-in (compile-time false) because in-game testing on Threadripper
    // showed DXMD never requests affinity outside our fake range.
    const auto& topo = topology();
    const DWORD_PTR allowed = first_n_bits_mask(topo.logical_processors);
    DWORD_PTR effective = mask;
    bool modified = false;
    if (g_clamp_affinity) {
        DWORD_PTR intersected = mask & allowed;
        if (intersected == 0) intersected = allowed;
        if (intersected != mask) {
            effective = intersected;
            modified = true;
        }
    }
    DWORD_PTR prev = g_real_SetThreadAffinityMask(hThread, effective);
    if (log_level() >= 2 ||
        (log_level() >= 1 && first_hit(&g_fh_SetThreadAffinityMask))) {
        log_line("SetThreadAffinityMask: 0x%llx -> 0x%llx%s (prev 0x%llx)",
                 (unsigned long long)mask,
                 (unsigned long long)effective,
                 modified ? " [clamped]" : "",
                 (unsigned long long)prev);
    }
    return prev;
}

struct HookSpec {
    const char* api_name;
    LPVOID      detour;
    void**      pp_trampoline;
    bool        always_install;
};

static const HookSpec g_hook_specs[] = {
    { "GetSystemInfo",                 (LPVOID)&Hooked_GetSystemInfo,                 (void**)&g_real_GetSystemInfo,                 true  },
    { "GetNativeSystemInfo",           (LPVOID)&Hooked_GetNativeSystemInfo,           (void**)&g_real_GetNativeSystemInfo,           true  },
    { "GetActiveProcessorCount",       (LPVOID)&Hooked_GetActiveProcessorCount,       (void**)&g_real_GetActiveProcessorCount,       true  },
    { "GetMaximumProcessorCount",      (LPVOID)&Hooked_GetMaximumProcessorCount,      (void**)&g_real_GetMaximumProcessorCount,      true  },
    { "GetActiveProcessorGroupCount",  (LPVOID)&Hooked_GetActiveProcessorGroupCount,  (void**)&g_real_GetActiveProcessorGroupCount,  true  },
    { "GetMaximumProcessorGroupCount", (LPVOID)&Hooked_GetMaximumProcessorGroupCount, (void**)&g_real_GetMaximumProcessorGroupCount, true  },
    { "SetThreadAffinityMask",         (LPVOID)&Hooked_SetThreadAffinityMask,         (void**)&g_real_SetThreadAffinityMask,         false },
};

// Prefer kernel32 (its GetProcAddress follows forwarders to the
// KernelBase implementation); fall back to KernelBase if kernel32
// doesn't export the symbol.
static LPVOID resolve_target(const char* api) {
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
        if (auto p = GetProcAddress(k32, api)) {
            return reinterpret_cast<LPVOID>(p);
        }
    }
    if (HMODULE kb = GetModuleHandleW(L"KernelBase.dll")) {
        if (auto p = GetProcAddress(kb, api)) {
            return reinterpret_cast<LPVOID>(p);
        }
    }
    return nullptr;
}

static int install_cpu_hooks(bool clamp_affinity) {
    g_clamp_affinity = clamp_affinity;

    // MH_Initialize is idempotent. Track whether WE initialized so the
    // failure path only calls MH_Uninitialize if we own MinHook's
    // lifetime.
    bool we_initialized_mh = false;
    MH_STATUS s = MH_Initialize();
    if (s == MH_OK) {
        we_initialized_mh = true;
    } else if (s != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("ERROR: MH_Initialize failed: %d", (int)s);
        return -1;
    }

    // MH_EnableHook(MH_ALL_HOOKS) is not transactional - MinHook
    // enables one at a time and stops on first error. Fine here
    // because we're called from DllMain before any worker threads
    // exist, and on any error we DisableHook/RemoveHook everything.
    struct Created {
        LPVOID      target;
        const char* api_name;
        bool        required;
        void**      pp_trampoline;
        void*       trampoline;
        LPVOID      detour;
    };
    Created created[sizeof(g_hook_specs) / sizeof(g_hook_specs[0])];
    int n_created = 0;
    int n_required_created = 0;
    int n_required_total = 0;

    // Pass 1: create all hooks.
    for (const auto& spec : g_hook_specs) {
        if (spec.always_install) n_required_total++;

        // Skip SetThreadAffinityMask when clamp_affinity is false -
        // the only optional hook. Installing it would log every
        // affinity call at LogLevel=2 and actively clamp affinities.
        if (!spec.always_install && !clamp_affinity &&
            strcmp(spec.api_name, "SetThreadAffinityMask") == 0) {
            continue;
        }

        LPVOID target = resolve_target(spec.api_name);
        if (!target) {
            if (spec.always_install) {
                log_line("ERROR: required API %s not exported on this Windows; aborting hook install.",
                         spec.api_name);
                goto fail;
            }
            log_line("INFO: optional API %s not present; skipped.", spec.api_name);
            continue;
        }

        void* tramp = nullptr;
        MH_STATUS cs = MH_CreateHook(target, spec.detour, &tramp);
        if (cs != MH_OK) {
            if (spec.always_install) {
                log_line("ERROR: MH_CreateHook(%s) failed: %d; aborting.",
                         spec.api_name, (int)cs);
                goto fail;
            }
            log_line("WARN: MH_CreateHook(%s) failed: %d; optional, continuing.",
                     spec.api_name, (int)cs);
            continue;
        }

        created[n_created++] = Created{
            target, spec.api_name, spec.always_install,
            spec.pp_trampoline, tramp, spec.detour
        };
        if (spec.always_install) n_required_created++;
    }

    if (n_required_created != n_required_total) {
        log_line("ERROR: only %d of %d required hooks created; aborting.",
                 n_required_created, n_required_total);
        goto fail;
    }

    // Pass 2: publish trampoline pointers.
    for (int i = 0; i < n_created; ++i) {
        *created[i].pp_trampoline = created[i].trampoline;
    }

    // Pass 3: enable all hooks.
    {
        MH_STATUS es = MH_EnableHook(MH_ALL_HOOKS);
        if (es != MH_OK) {
            log_line("ERROR: MH_EnableHook(MH_ALL_HOOKS) failed: %d; aborting.", (int)es);
            MH_DisableHook(MH_ALL_HOOKS);
            goto fail;
        }
    }

    log_line("CPU hook install complete: %d hooks active (%d required, %d optional).",
             n_created, n_required_created, n_created - n_required_created);
    for (int i = 0; i < n_created; ++i) {
        log_line("  Hooked %s @ %p%s", created[i].api_name, created[i].target,
                 created[i].required ? " [required]" : " [optional]");
    }
    return 0;

fail:
    for (int i = 0; i < n_created; ++i) {
        MH_RemoveHook(created[i].target);
        *created[i].pp_trampoline = nullptr;
    }
    if (we_initialized_mh) {
        MH_Uninitialize();
    }
    return -1;
}

static void remove_cpu_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}


// =========================================================================
// SECTION 8: DllMain and attach/detach
// =========================================================================
//
// Notable departures from generic DllMain advice:
//
// 1. The whole fix runs synchronously in DLL_PROCESS_ATTACH (load real
//    dxgi, install hooks). A dxgi proxy guarantees our DllMain runs
//    before DXMD's entry point, but only BEFORE-our-init guarantees us
//    priority over middleware (ApexFramework, PhysX, bink, amd_ags)
//    that also imports topology APIs. Punting to a worker thread would
//    be too late. MinHook doesn't touch the loader lock, and the
//    recursive LoadLibrary of System32\dxgi.dll is constrained to an
//    absolute path so it's safe here.
//
// 2. We do NOT call DisableThreadLibraryCalls. Combining it with the
//    static CRT (/MT) breaks per-thread CRT state cleanup (ReShade
//    documents the same conclusion).
//
// 3. host_is_dxmd() gates installing the CPU hooks: a proxy DLL that
//    altered any host process would be a living-off-the-land risk.
//    Non-DXMD hosts still get correctly forwarded dxgi.
//
// 4. On real-dxgi load failure we return FALSE from DllMain so the OS
//    reports a clean "failed to load dxgi.dll" instead of a deferred
//    crash when the game calls CreateDXGIFactory.
//
// 5. On DLL_PROCESS_DETACH we skip cleanup when lpReserved != nullptr
//    (process termination): doing MinHook teardown there risks
//    loader-lock deadlocks per MSDN.

static HMODULE g_self = nullptr;
static HMODULE g_real_dxgi = nullptr;
static bool    g_fix_active = false;

// True if the host EXE basename is "DXMD.exe" (case-insensitive).
static bool host_is_dxmd() {
    wchar_t* path = get_module_path(nullptr);
    if (!path) return false;
    const wchar_t* base = path;
    for (size_t i = 0; path[i]; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') base = &path[i + 1];
    }
    bool result = wstr_ieq_ascii(base, L"DXMD.exe");
    free_wstr(path);
    return result;
}

static BOOL attach() {
    log_init_deferred(g_self);

    // Hardcoded defaults. v1.0.0 read these from dxmd-thread-fix.ini;
    // v1.1.0 dropped the INI because the values never need tuning in
    // practice. Edit and recompile if you genuinely need different
    // values.
    constexpr int  kLogicalProcessors = 8;     // DXMD sweet spot
    constexpr bool kClampAffinity     = false; // don't hook SetThreadAffinityMask
    constexpr int  kLogLevel          = 1;     // startup + first-hit per hooked API

    log_set_level(kLogLevel);
    log_line("dxmd-thread-fix attach: loaded at module handle %p", g_self);

    // MAX_PATH buffer is fine here - host_path is only used for the
    // log line below; host_is_dxmd() above did the real basename check
    // via the long-path-safe helper.
    wchar_t host_path[MAX_PATH];
    DWORD host_n = GetModuleFileNameW(nullptr, host_path, MAX_PATH);
    if (host_n > 0 && host_n < MAX_PATH) {
        log_line("Host process: %ls", host_path);
    }
    const bool is_dxmd = host_is_dxmd();
    if (!is_dxmd) {
        log_line("WARNING: host process is NOT DXMD.exe. CPU topology hooks");
        log_line("WARNING: will NOT be installed (they're DXMD-specific). The");
        log_line("WARNING: dxgi proxy will still forward to System32 dxgi.");
    }

    g_real_dxgi = load_system_dxgi_and_resolve();
    if (!g_real_dxgi) {
        log_line("FATAL: real dxgi could not be loaded. Returning FALSE from");
        log_line("FATAL: DllMain so the OS reports a clean DLL load failure.");
        return FALSE;
    }

    if (!is_dxmd) {
        log_line("FIX STATUS: forwarder-only mode (non-DXMD host).");
        g_fix_active = false;
        return TRUE;
    }

    set_topology(static_cast<WORD>(kLogicalProcessors));
    log_line("Fake topology: %u logical processors, %u group(s)  (real: %u in %u group(s))",
             (unsigned)topology().logical_processors,
             (unsigned)topology().groups,
             (unsigned)topology().real_logical_processors,
             (unsigned)topology().real_groups);

    int rc = install_cpu_hooks(kClampAffinity);
    if (rc != 0) {
        log_line("============================================================");
        log_line("FIX STATUS: *** INACTIVE ***  (install_cpu_hooks rc=%d)", rc);
        log_line("The thread-count fix did NOT install. The game will run with");
        log_line("its original CPU detection and may crash on high-core CPUs.");
        log_line("Please file a bug report including this log file.");
        log_line("============================================================");
        g_fix_active = false;
    } else {
        log_line("============================================================");
        log_line("FIX STATUS: ACTIVE  (reporting %u logical processors to game)",
                 (unsigned)topology().logical_processors);
        log_line("============================================================");
        g_fix_active = true;
    }
    return TRUE;
}

static void detach() {
    log_line("dxmd-thread-fix detach (explicit unload).");
    remove_cpu_hooks();
    free_system_dxgi(g_real_dxgi);
    g_real_dxgi = nullptr;
    log_shutdown();
}

} // namespace dtf

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            dtf::g_self = hModule;
            // No DisableThreadLibraryCalls - see SECTION 8 header.
            return dtf::attach();
        case DLL_PROCESS_DETACH:
            // Only do non-trivial cleanup on explicit FreeLibrary
            // (lpReserved == nullptr). At process termination, let
            // Windows reclaim everything; MinHook teardown here would
            // risk loader-lock deadlocks.
            if (lpReserved == nullptr) {
                dtf::detach();
            }
            return TRUE;
        default:
            return TRUE;
    }
}
