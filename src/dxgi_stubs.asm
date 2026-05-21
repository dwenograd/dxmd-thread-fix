; dxgi_stubs.asm - tail-jump forwarders for every exported dxgi function.
;
; Each stub is two instructions:
;   jmp QWORD PTR [pfn_FOO]
; where pfn_FOO is a 64-bit global initialized at COMPILE TIME (in
; dxgi_exports.cpp) to one of the trap functions in dtf_traps.cpp.
; After our DllMain runs, the loader overwrites each pfn_FOO with the
; real System32 dxgi function pointer. Exports missing on the host's
; Windows version keep pointing at their trap.
;
; Tail-jumping (vs. C++ thunking) preserves the original calling
; convention, register state, parameter passing, and return value
; without us having to know each function's signature. This is critical
; for the dxgi private exports (DXGID3D10*, PIX*, SetAppCompatStringPointer,
; UpdateHMDEmulationStatus, etc.) whose prototypes are not publicly
; documented.
;
; pfn_FOO is GUARANTEED non-null by the compile-time initializers in
; dxgi_exports.cpp. The traps return:
;   - generic exports: 0 (S_OK / nullptr / FALSE - safe for compat-pass)
;   - CreateDXGIFactory{,1,2} / DXGIGetDebugInterface1: zero out-pointer
;     and return DXGI_ERROR_NOT_FOUND
;   - DXGIDeclareAdapterRemovalSupport: DXGI_ERROR_NOT_FOUND
;   - DXGIDisableVBlankVirtualization: DXGI_ERROR_NOT_FOUND (typed HANDLE trap)
;
; This means: even if our DLL loads but real System32 dxgi can't be
; resolved, the game receives clean error codes from factory APIs
; rather than crashing on a NULL dereference.

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
