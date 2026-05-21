; dxgi_stubs.asm - tail-jump forwarders for every exported dxgi function.
;
; ============================================================
; UNUSUAL THING: this is hand-written x64 assembly, one tiny PROC
; per dxgi export, each just a single jmp instruction.
; ============================================================
;
; A normal proxy DLL writes a C++ thunk per export, e.g.:
;
;     extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID r, void** p) {
;         return real_CreateDXGIFactory(r, p);
;     }
;
; That works for the well-documented exports. We can't use it for the
; entire dxgi export set because:
;
;   - dxgi exports 20 functions, and 10+ of them are UNDOCUMENTED
;     internal/private APIs (DXGID3D10*, PIX*, SetAppCompatStringPointer,
;     UpdateHMDEmulationStatus, ApplyCompatResolutionQuirking,
;     CompatString, CompatValue, DXGIDumpJournal).
;   - For undocumented exports, we don't know the parameter list or
;     return type. A C++ thunk forces us to guess.
;   - Guessing wrong corrupts the ABI: if we declare a function as
;     HRESULT(void) but it actually takes HRESULT(HANDLE), the real
;     function reads RCX as the HANDLE arg - and gets garbage.
;
; A tail-jump (jmp QWORD PTR [pfn_FOO]) preserves the entire calling
; state: rcx/rdx/r8/r9 stay where the caller put them, xmm0-3 stay
; where the caller put them, the stack stays untouched, the return
; address is the original caller's. The real function sees exactly
; what the caller intended, regardless of what the signature is.
;
; Each stub compiles to exactly 6 bytes (FF 25 disp32 - RIP-relative
; indirect jump). The disp32 is fixed up by the linker to point at
; the pfn_FOO global in our .data section.
;
; ============================================================
; pfn_FOO is NEVER NULL (this is critical - see dtf_traps.cpp)
; ============================================================
;
; The pfn_FOO globals are initialized AT COMPILE TIME (in
; dxgi_exports.cpp) to point at trap functions in dtf_traps.cpp.
; That's not the obvious thing to do - a normal C++ programmer would
; default them to nullptr and resolve them at runtime in DllMain.
;
; We can't do that because the Windows loader's app-compatibility
; pass (apphelp.dll) calls some of dxgi's exports BEFORE our DllMain
; runs. If pfn_FOO were nullptr at that point, this asm stub would
; jmp to address 0 and the process would die. We confirmed this the
; hard way: the first attempt at this DLL crashed every install
; with a fault at address 0, inside apphelp's call to
; SetAppCompatStringPointer, before our DllMain ever ran.
;
; By the time anything other than apphelp calls our stubs, our
; DllMain has run and overwritten the trap pointers with real
; System32 dxgi function addresses. Exports missing on the host's
; Windows version (rare; mostly Windows-10+-only) keep pointing at
; their trap, which returns a clean failure code.
;
; The trap return values are documented in dtf_traps.cpp. Summary:
;   - generic exports: 0 (apphelp treats as "no shim handled")
;   - CreateDXGIFactory{,1,2} / DXGIGetDebugInterface1: zero out-pointer
;     and return DXGI_ERROR_NOT_FOUND (safe failure for the caller)
;   - DXGIDeclareAdapterRemovalSupport: DXGI_ERROR_NOT_FOUND
;   - DXGIDisableVBlankVirtualization: DXGI_ERROR_NOT_FOUND (typed HANDLE)
;
; ============================================================
; Why we can't use static .def-file forwarders
; ============================================================
;
; The linker supports a forwarder syntax:
;   CreateDXGIFactory = real_dxgi.CreateDXGIFactory
; That generates an export-table entry that says "to call this, find
; real_dxgi.dll and call its CreateDXGIFactory". Looks like exactly
; what we want.
;
; It doesn't work for us because our file is also named dxgi.dll, so
; the forwarder name `dxgi.CreateDXGIFactory` would loop back to
; ourselves. We'd need to either:
;   - rename our DLL to something other than dxgi (then DXMD wouldn't
;     find us via its static import), or
;   - copy System32\dxgi.dll into the install folder under a different
;     name like dxgi_orig.dll and forward there (fragile, and the OS
;     may treat the copy as suspicious).
;
; Tail-jumps through runtime-resolved function pointers in .data
; sidestep all of that.

PUBLIC ApplyCompatResolutionQuirking
PUBLIC CompatString
PUBLIC CompatValue
PUBLIC DXGIDumpJournal
PUBLIC PIXBeginCapture
PUBLIC PIXEndCapture
PUBLIC PIXGetCaptureState
PUBLIC SetAppCompatStringPointer
PUBLIC UpdateHMDEmulationStatus
PUBLIC CreateDXGIFactory
PUBLIC CreateDXGIFactory1
PUBLIC CreateDXGIFactory2
PUBLIC DXGID3D10CreateDevice
PUBLIC DXGID3D10CreateLayeredDevice
PUBLIC DXGID3D10GetLayeredDeviceSize
PUBLIC DXGID3D10RegisterLayers
PUBLIC DXGIDeclareAdapterRemovalSupport
PUBLIC DXGIDisableVBlankVirtualization
PUBLIC DXGIGetDebugInterface1
PUBLIC DXGIReportAdapterConfiguration

EXTERN pfn_ApplyCompatResolutionQuirking:QWORD
EXTERN pfn_CompatString:QWORD
EXTERN pfn_CompatValue:QWORD
EXTERN pfn_DXGIDumpJournal:QWORD
EXTERN pfn_PIXBeginCapture:QWORD
EXTERN pfn_PIXEndCapture:QWORD
EXTERN pfn_PIXGetCaptureState:QWORD
EXTERN pfn_SetAppCompatStringPointer:QWORD
EXTERN pfn_UpdateHMDEmulationStatus:QWORD
EXTERN pfn_CreateDXGIFactory:QWORD
EXTERN pfn_CreateDXGIFactory1:QWORD
EXTERN pfn_CreateDXGIFactory2:QWORD
EXTERN pfn_DXGID3D10CreateDevice:QWORD
EXTERN pfn_DXGID3D10CreateLayeredDevice:QWORD
EXTERN pfn_DXGID3D10GetLayeredDeviceSize:QWORD
EXTERN pfn_DXGID3D10RegisterLayers:QWORD
EXTERN pfn_DXGIDeclareAdapterRemovalSupport:QWORD
EXTERN pfn_DXGIDisableVBlankVirtualization:QWORD
EXTERN pfn_DXGIGetDebugInterface1:QWORD
EXTERN pfn_DXGIReportAdapterConfiguration:QWORD

.CODE

ApplyCompatResolutionQuirking PROC
    jmp QWORD PTR [pfn_ApplyCompatResolutionQuirking]
ApplyCompatResolutionQuirking ENDP

CompatString PROC
    jmp QWORD PTR [pfn_CompatString]
CompatString ENDP

CompatValue PROC
    jmp QWORD PTR [pfn_CompatValue]
CompatValue ENDP

DXGIDumpJournal PROC
    jmp QWORD PTR [pfn_DXGIDumpJournal]
DXGIDumpJournal ENDP

PIXBeginCapture PROC
    jmp QWORD PTR [pfn_PIXBeginCapture]
PIXBeginCapture ENDP

PIXEndCapture PROC
    jmp QWORD PTR [pfn_PIXEndCapture]
PIXEndCapture ENDP

PIXGetCaptureState PROC
    jmp QWORD PTR [pfn_PIXGetCaptureState]
PIXGetCaptureState ENDP

SetAppCompatStringPointer PROC
    jmp QWORD PTR [pfn_SetAppCompatStringPointer]
SetAppCompatStringPointer ENDP

UpdateHMDEmulationStatus PROC
    jmp QWORD PTR [pfn_UpdateHMDEmulationStatus]
UpdateHMDEmulationStatus ENDP

CreateDXGIFactory PROC
    jmp QWORD PTR [pfn_CreateDXGIFactory]
CreateDXGIFactory ENDP

CreateDXGIFactory1 PROC
    jmp QWORD PTR [pfn_CreateDXGIFactory1]
CreateDXGIFactory1 ENDP

CreateDXGIFactory2 PROC
    jmp QWORD PTR [pfn_CreateDXGIFactory2]
CreateDXGIFactory2 ENDP

DXGID3D10CreateDevice PROC
    jmp QWORD PTR [pfn_DXGID3D10CreateDevice]
DXGID3D10CreateDevice ENDP

DXGID3D10CreateLayeredDevice PROC
    jmp QWORD PTR [pfn_DXGID3D10CreateLayeredDevice]
DXGID3D10CreateLayeredDevice ENDP

DXGID3D10GetLayeredDeviceSize PROC
    jmp QWORD PTR [pfn_DXGID3D10GetLayeredDeviceSize]
DXGID3D10GetLayeredDeviceSize ENDP

DXGID3D10RegisterLayers PROC
    jmp QWORD PTR [pfn_DXGID3D10RegisterLayers]
DXGID3D10RegisterLayers ENDP

DXGIDeclareAdapterRemovalSupport PROC
    jmp QWORD PTR [pfn_DXGIDeclareAdapterRemovalSupport]
DXGIDeclareAdapterRemovalSupport ENDP

DXGIDisableVBlankVirtualization PROC
    jmp QWORD PTR [pfn_DXGIDisableVBlankVirtualization]
DXGIDisableVBlankVirtualization ENDP

DXGIGetDebugInterface1 PROC
    jmp QWORD PTR [pfn_DXGIGetDebugInterface1]
DXGIGetDebugInterface1 ENDP

DXGIReportAdapterConfiguration PROC
    jmp QWORD PTR [pfn_DXGIReportAdapterConfiguration]
DXGIReportAdapterConfiguration ENDP

END
