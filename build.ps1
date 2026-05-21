# build.ps1 - one-shot MSVC build for dxmd-thread-fix
#
# Produces dist\dxgi.dll. Requires Visual Studio 2022 (Community is fine)
# with the "Desktop development with C++" workload installed.
#
# Usage:
#   pwsh -File build.ps1            # release build (default)
#   pwsh -File build.ps1 -Config Debug

param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath
Set-Location $root

# -- Locate Visual Studio and import vcvars64 env into this PowerShell ---

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found. Install Visual Studio 2022." }
$vs = & $vswhere -latest -property installationPath
if (-not $vs) { throw "No Visual Studio installation found by vswhere." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Spawn a child cmd that calls vcvars64.bat, then echoes env vars; parse
# them back into this PowerShell session.
$envDump = & cmd.exe /s /c "`"$vcvars`" >nul && set" 2>&1
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
    }
}
# Sanity check
$cl    = (Get-Command cl.exe   -ErrorAction SilentlyContinue).Source
$ml64  = (Get-Command ml64.exe -ErrorAction SilentlyContinue).Source
$link  = (Get-Command link.exe -ErrorAction SilentlyContinue).Source
if (-not $cl -or -not $ml64 -or -not $link) {
    throw "After importing vcvars64, missing one of: cl/ml64/link"
}
Write-Host "Toolchain ready:"
Write-Host "  cl    = $cl"
Write-Host "  ml64  = $ml64"
Write-Host "  link  = $link"

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

# -- Flags ---------------------------------------------------------------

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
    $relFlags = @('/O2', '/Oi', '/GL', '/MT', '/DNDEBUG', '/GS-', '/Gy')
    $cppFlags += $relFlags
    $cFlags   += $relFlags
} else {
    $dbgFlags = @('/Od', '/Zi', '/MTd', '/RTC1')
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

$outDll = Join-Path $dist 'dxgi.dll'
$outPdb = Join-Path $dist 'dxgi.pdb'
$implib = Join-Path $build 'dxgi.lib'
$defFile = Join-Path $root 'src\dxgi.def'

$linkFlags = @(
    '/nologo',
    '/DLL',
    '/MACHINE:X64',
    "/DEF:$defFile",
    "/OUT:$outDll",
    "/IMPLIB:$implib",
    "/PDB:$outPdb",
    'kernel32.lib', 'user32.lib', 'advapi32.lib'
)
if ($Config -eq 'Release') {
    $linkFlags += @('/LTCG', '/OPT:REF', '/OPT:ICF', '/RELEASE')
} else {
    $linkFlags += @('/DEBUG')
}

Write-Host "Linking..." -ForegroundColor Cyan
Invoke-Tool $link ($linkFlags + $asmObjs + $cObjs + $cppObjs) "link -> dxgi.dll"

Write-Host ""
Write-Host "BUILD OK -> $outDll" -ForegroundColor Green
Get-Item $outDll | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
