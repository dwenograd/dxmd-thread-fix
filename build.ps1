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
#   - Optional debug symbols go to dist\dxgi.pdb.
#   - Self-builds are NOT expected to match the SHA-256 of official
#     release binaries byte-for-byte; MSVC embeds timestamps, GUIDs,
#     and absolute paths that differ between machines. Verify the
#     EXPORT TABLE matches the official release (dumpbin /exports),
#     and that the source you built came from a tagged commit.

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

# -- Sources -------------------------------------------------------------

$cppSources = @(
    'src\dllmain.cpp',
    'src\config.cpp',
    'src\log.cpp',
    'src\topology.cpp',
    'src\dxgi_exports.cpp',
    'src\dtf_traps.cpp',
    'src\cpu_hooks.cpp'
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
    # Hardening: /GS (stack cookies on), /sdl (additional checks), explicit
    # security mitigations. NOT /guard:cf because MinHook's runtime code
    # patching is fundamentally at odds with CFG validation.
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
    # Security mitigations (explicit, so PE characteristics reflect them):
    '/DYNAMICBASE',          # ASLR
    '/HIGHENTROPYVA',        # 64-bit ASLR
    '/NXCOMPAT',             # DEP
    "/DEF:$defFile",
    "/OUT:$outDll",
    "/IMPLIB:$implib",
    "/PDB:$outPdb",
    # Minimal system import:
    'kernel32.lib'
)
if ($Config -eq 'Release') {
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
