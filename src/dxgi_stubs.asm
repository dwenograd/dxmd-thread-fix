; dxgi_stubs.asm - tail-jump forwarders for every exported dxgi function.
;
; Each stub is two instructions:
;   jmp QWORD PTR [pfn_FOO]
; where pfn_FOO is a 64-bit global initialized at DLL load time by the
; resolver in dxgi_exports.cpp.
;
; Tail-jumping (vs. C++ thunking) preserves the original calling
; convention, register state, parameter passing, and return value
; without us having to know each function's signature. This is critical
; for the dxgi private exports (DXGID3D10*, PIX*, SetAppCompatStringPointer,
; UpdateHMDEmulationStatus, etc.) whose prototypes are not publicly
; documented.
;
; If pfn_FOO is null (export not present on this Windows version), the
; jump will crash. dxgi_exports.cpp logs a warning in that case; if the
; game actually invokes such an export, that's a Windows version
; incompatibility, not our bug.

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
