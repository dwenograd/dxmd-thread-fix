# build.ps1 - one-shot MSVC build for dxmd-thread-fix
#
# Produces dist\dxgi.dll. Requires Visual Studio 2022 (Community is
# fine) with the "Desktop development with C++" workload installed,
# which provides cl.exe, ml64.exe, link.exe, and rc.exe.
#
# Usage:
#   pwsh -File build.ps1            # release build (default)
#   pwsh -File build.ps1 -Config Debug
#
# Notes for users building from source:
#   - This script handles paths with spaces (we quote everything).
#   - Output goes to dist\dxgi.dll (a single ~150 KB file).
#   - Debug builds also emit dist\dxgi.pdb; Release builds do not
#     (we don't pass /DEBUG to the linker in Release mode).
#   - Self-builds are NOT expected to match the SHA-256 of official
#     release binaries byte-for-byte; MSVC embeds timestamps and
#     other linker/toolchain metadata into the PE that differ between
#     machines and toolchain versions. Verify the EXPORT TABLE
#     matches the official release (dumpbin /exports), and that the
#     source you built came from a tagged commit.

param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath
Set-Location -LiteralPath $root

# -- Locate Visual Studio and import vcvars64 env into this PowerShell ---

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 (Community is fine) with the 'Desktop development with C++' workload."
}
$vs = & $vswhere -latest -property installationPath
if (-not $vs) { throw "No Visual Studio installation found by vswhere." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Spawn a child cmd that calls vcvars64.bat, then echoes env vars; parse
# them back into this PowerShell session. We quote $vcvars to handle
# paths with spaces in 'Program Files (x86)'.
$envDump = & cmd.exe /s /c "`"$vcvars`" >nul && set" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host ($envDump | Out-String) -ForegroundColor Red
    throw "vcvars64.bat failed (exit $LASTEXITCODE). Visual Studio 2022's C++ workload may be incomplete or damaged."
}
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
    }
}
$cl    = (Get-Command cl.exe   -ErrorAction SilentlyContinue).Source
$ml64  = (Get-Command ml64.exe -ErrorAction SilentlyContinue).Source
$link  = (Get-Command link.exe -ErrorAction SilentlyContinue).Source
$rc    = (Get-Command rc.exe   -ErrorAction SilentlyContinue).Source
if (-not $cl -or -not $ml64 -or -not $link -or -not $rc) {
    throw "After importing vcvars64, missing one of: cl/ml64/link/rc"
}
Write-Host "Toolchain ready:"
Write-Host "  cl    = $cl"
Write-Host "  ml64  = $ml64"
Write-Host "  link  = $link"
Write-Host "  rc    = $rc"

# -- Build / dist dirs ---------------------------------------------------

$build = Join-Path $root 'build'
$dist  = Join-Path $root 'dist'
New-Item -ItemType Directory -Path $build -Force | Out-Null
New-Item -ItemType Directory -Path $dist  -Force | Out-Null
# Don't leave stale logs from prior smoke tests in dist/ - they'd ship
# in any release zip made by zipping dist/.
Remove-Item -LiteralPath (Join-Path $dist 'dxmd-thread-fix.log') -Force -ErrorAction SilentlyContinue

# -- Sources -------------------------------------------------------------

$cppSources = @(
    'src\dxmd_thread_fix.cpp'
)
$cSources = @(
    'third_party\minhook\src\buffer.c',
    'third_party\minhook\src\hook.c',
    'third_party\minhook\src\trampoline.c',
    'third_party\minhook\src\hde\hde64.c'
)
$asmSources = @(
    'src\dxgi_stubs.asm'
)
$rcSources = @(
    'src\version.rc'
)

# -- Flags --------------------------------------------------------------
#
# Path-quoting note: cl/link parse `/Ifoo bar` as TWO arguments. PowerShell
# splits at spaces unless the WHOLE argument value is one string. We pass
# each flag as a single PS string (no embedded quoting needed for cl - the
# child-process invocation will deliver each array element as one arg).

$commonFlags = @(
    '/nologo',
    '/D_WIN32_WINNT=0x0601',     # Windows 7 SP1 baseline (game minimum)
    '/DWIN32_LEAN_AND_MEAN',
    '/DNOMINMAX',
    "/I$root\src",
    "/I$root\third_party\minhook\include",
    "/I$root\third_party\minhook\src"
)
$cppFlags = @('/W4', '/EHsc', '/std:c++17') + $commonFlags
$cFlags   = @('/W3')                          + $commonFlags
if ($Config -eq 'Release') {
    # =================================================================
    # Why these specific compiler flags (matters for anyone reviewing
    # this source — these are deliberate, not accidental):
    # =================================================================
    #
    # /O2 /Oi /GL  — standard release optimization. /GL enables whole-
    #                program optimization (LTCG). The traps in
    #                SECTION 1 of dxmd_thread_fix.cpp use __declspec(noinline) to survive
    #                this (we need their symbols intact for the pfn_FOO
    #                initializers).
    #
    # /MT          — static CRT linkage. CRITICAL design choice.
    #                /MT links the CRT statically into our DLL so it
    #                has no external VC runtime dependency. /MD
    #                would require an appropriate VC++ runtime
    #                redistributable to be installed and healthy on
    #                the user's machine; if absent or broken, the
    #                DLL would fail to load with an obscure
    #                "msvcp140.dll not found" or similar error,
    #                defeating the "drop in and play" value
    #                proposition. Static CRT makes our ~150 KB
    #                dxgi.dll completely self-contained.
    #
    #                Cost: ~80 KB of CRT statically baked in. Fine.
    #
    # /DNDEBUG     — strips asserts.
    #
    # /GS          — stack-cookie checks. Inserts canary values to
    #                detect stack-buffer overflow. Cheap and good.
    #
    # /sdl         — Security Development Lifecycle extra checks.
    #                Turns specific warnings into errors and enables
    #                additional buffer-security checks.
    #
    # /Gy          — function-level linking; lets the linker /OPT:REF
    #                drop unused functions (cleaner final binary).
    #
    # ------ Flags we deliberately DO NOT use ------
    #
    # /guard:cf    — Control Flow Guard. Instruments indirect calls
    #                in our compiled code with a runtime check
    #                against a bitmap of legal call targets.
    #
    #                Why we don't enable it: SECTION 7 of dxmd_thread_fix.cpp calls the
    #                original (pre-hook) API implementations through
    #                MinHook-provided trampolines (function pointers
    #                stored in `g_real_GetSystemInfo`, etc.). Those
    #                trampolines live in runtime-allocated executable
    #                memory that MinHook obtains via VirtualAlloc.
    #
    #                Whether such runtime-allocated targets are valid
    #                under CFG depends on the Windows version and on
    #                whether MinHook explicitly registers them via
    #                SetProcessValidCallTargets. The v1.3.3 release
    #                we vendor does NOT register its trampolines. We
    #                have NOT validated the combination of /guard:cf
    #                + our MinHook vintage across the Windows 7 SP1
    #                through Windows 11 range we support. Enabling
    #                CFG without that validation could turn a working
    #                install into "DLL loads but first hooked call
    #                crashes the process" on some unknown subset of
    #                users' machines.
    #
    #                That validation is a separate compatibility
    #                project. For v1.0.0 we conservatively leave CFG
    #                off; the other mitigations (DEP, ASLR, stack
    #                cookies, /sdl, static CRT) are still on.
    #
    #                NOTE: Some CFG-related load-config metadata may
    #                still appear in the final binary because MSVC's
    #                static CRT objects are CFG-instrumented upstream.
    #                That metadata is inert — the OS only enforces
    #                CFG when IMAGE_DLLCHARACTERISTICS_GUARD_CF is
    #                set in the PE header, which we don't set. A
    #                code-curious reader running `dumpbin /loadconfig`
    #                will see the CFG fields populated and might
    #                think we're enabling it; we're not. Verify with
    #                `dumpbin /headers dxgi.dll` — the DLL
    #                characteristics line will not include "Guard CF".
    #
    # /MD          — see /MT above. Would force a VC++ redist dep.
    #
    # /Wall        — too noisy; includes warnings for system headers.
    #                We use /W4 instead (still strict, signal-only).
    $relFlags = @('/O2', '/Oi', '/GL', '/MT', '/DNDEBUG', '/GS', '/sdl', '/Gy')
    $cppFlags += $relFlags
    $cFlags   += $relFlags
} else {
    $dbgFlags = @('/Od', '/Zi', '/MTd', '/RTC1', '/GS', '/sdl')
    $cppFlags += $dbgFlags
    $cFlags   += $dbgFlags
}

function Invoke-Tool {
    param([string]$Tool, [string[]]$ToolArgs, [string]$Label)
    Write-Host "  $Label" -ForegroundColor DarkCyan
    & $Tool @ToolArgs
    if ($LASTEXITCODE -ne 0) { throw "$Label failed (exit $LASTEXITCODE)" }
}

# -- Assemble ------------------------------------------------------------

$asmObjs = @()
Write-Host "Assembling..." -ForegroundColor Cyan
foreach ($a in $asmSources) {
    $src = Join-Path $root $a
    $obj = Join-Path $build ([System.IO.Path]::GetFileNameWithoutExtension($a) + '.obj')
    Invoke-Tool $ml64 @('/nologo', '/c', "/Fo$obj", $src) "ml64 $a"
    $asmObjs += $obj
}

# -- Compile resources --------------------------------------------------

$resObjs = @()
Write-Host "Compiling resources..." -ForegroundColor Cyan
foreach ($r in $rcSources) {
    $src = Join-Path $root $r
    $res = Join-Path $build ([System.IO.Path]::GetFileNameWithoutExtension($r) + '.res')
    Invoke-Tool $rc @('/nologo', '/fo', $res, $src) "rc $r"
    $resObjs += $res
}

# -- Compile C (MinHook) -------------------------------------------------

$cObjs = @()
Write-Host "Compiling C..." -ForegroundColor Cyan
foreach ($c in $cSources) {
    $src = Join-Path $root $c
    $obj = Join-Path $build ([System.IO.Path]::GetFileNameWithoutExtension($c) + '.obj')
    Invoke-Tool $cl ($cFlags + @('/c', "/Fo$obj", $src)) "cl $c"
    $cObjs += $obj
}

# -- Compile C++ ---------------------------------------------------------

$cppObjs = @()
Write-Host "Compiling C++..." -ForegroundColor Cyan
foreach ($c in $cppSources) {
    $src = Join-Path $root $c
    $obj = Join-Path $build ([System.IO.Path]::GetFileNameWithoutExtension($c) + '.obj')
    Invoke-Tool $cl ($cppFlags + @('/c', "/Fo$obj", $src)) "cl $c"
    $cppObjs += $obj
}

# -- Link ----------------------------------------------------------------

$outDll  = Join-Path $dist 'dxgi.dll'
$outPdb  = Join-Path $dist 'dxgi.pdb'
$implib  = Join-Path $build 'dxgi.lib'
$defFile = Join-Path $root 'src\dxgi.def'

$linkFlags = @(
    '/nologo',
    '/DLL',
    '/MACHINE:X64',
    '/SUBSYSTEM:WINDOWS',
    # =================================================================
    # PE security characteristics. We set these explicitly so the
    # final binary's IMAGE_DLLCHARACTERISTICS field reflects them and
    # the OS loader enforces them. (Defaults vary by toolchain; being
    # explicit makes the security posture self-documenting.)
    # =================================================================
    '/DYNAMICBASE',          # ASLR — DLL is randomly relocated at load
    '/HIGHENTROPYVA',        # Requests high-entropy 64-bit ASLR on
                             # OS versions that support it (Win 8+
                             # honors it fully; on Win7 SP1 it's
                             # accepted but the effective entropy is
                             # less). /DYNAMICBASE above is the
                             # baseline ASLR guarantee.
    '/NXCOMPAT',             # DEP — non-executable data pages
    # /DEF specifies the module-definition file that lists our 20 dxgi
    # exports by exact name AND ordinal. We use a .def file (not
    # __declspec(dllexport)) because:
    #   1. The dxgi exports include reserved names like CreateDXGIFactory
    #      that we want exported without C++ name mangling, AND
    #   2. Some callers (notably amd_ags and older D3D10-era code
    #      paths) resolve dxgi exports by ordinal in addition to or
    #      instead of by name. The .def file lets us pin both the
    #      name AND the ordinal to match System32 dxgi exactly, so
    #      ordinal-based resolution keeps working through our proxy.
    "/DEF:$defFile",
    "/OUT:$outDll",
    "/IMPLIB:$implib",
    "/PDB:$outPdb",
    # Only system import is kernel32 (for LoadLibrary, GetProcAddress,
    # CreateFile, the topology APIs, etc.). MinHook is statically
    # linked from the vendored source. Keeping the import list minimal
    # means a security auditor running `dumpbin /imports dxgi.dll` sees
    # a one-line answer: "yep, just kernel32, nothing fishy".
    'kernel32.lib'
)
if ($Config -eq 'Release') {
    # /LTCG: pairs with /GL on the compile side for whole-program opt.
    # /OPT:REF: drop unused functions/data.
    # /OPT:ICF: fold identical-COMDAT functions to one address.
    #          (See the SECTION 1 trap note in dxmd_thread_fix.cpp — this can fold two trap
    #          functions with identical machine code to one address,
    #          which is safe but worth knowing when debugging.)
    # /RELEASE: writes a PE image checksum into the optional header
    #          (mostly cosmetic for DLLs — the OS doesn't enforce
    #          it — but conventional for shipping binaries and good
    #          hygiene). There is NO IMAGE_FILE_RELEASE bit despite
    #          the flag's name; it just affects the checksum field.
    $linkFlags += @('/LTCG', '/OPT:REF', '/OPT:ICF', '/RELEASE')
} else {
    $linkFlags += @('/DEBUG')
}

Write-Host "Linking..." -ForegroundColor Cyan
Invoke-Tool $link ($linkFlags + $asmObjs + $resObjs + $cObjs + $cppObjs) "link -> dxgi.dll"

Write-Host ""
Write-Host "BUILD OK -> $outDll" -ForegroundColor Green
Get-Item -LiteralPath $outDll | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
# Print SHA-256 so the user / release process can record it
$hash = (Get-FileHash -LiteralPath $outDll -Algorithm SHA256).Hash
Write-Host "SHA-256: $hash" -ForegroundColor Cyan
