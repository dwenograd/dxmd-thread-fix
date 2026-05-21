# install.ps1 - copies dxgi.dll + dxmd-thread-fix.ini into a DXMD install.
#
# Usage:
#   pwsh -File install.ps1 -Game "N:\Steam\steamapps\common\Deus Ex Mankind Divided"
#
# If -Game is omitted, the script tries to auto-detect from Steam's
# libraryfolders.vdf.

param(
    [string]$Game,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath
$dll  = Join-Path $root 'dist\dxgi.dll'
$ini  = Join-Path $root 'dxmd-thread-fix.ini'

if (-not (Test-Path $dll)) {
    throw "Built dxgi.dll not found. Run build.ps1 first."
}
if (-not (Test-Path $ini)) {
    throw "dxmd-thread-fix.ini not found in project root."
}

# -- Auto-detect game install if not specified ---------------------------

function Find-DXMD {
    # Probe common Steam library locations via the registry.
    $steamPath = $null
    try {
        $reg = Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction Stop
        $steamPath = $reg.SteamPath
    } catch {
        try {
            $reg = Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction Stop
            $steamPath = $reg.InstallPath
        } catch { }
    }
    if (-not $steamPath) { return $null }
    $vdf = Join-Path $steamPath 'steamapps\libraryfolders.vdf'
    if (-not (Test-Path $vdf)) { return $null }

    $content = Get-Content $vdf -Raw
    $libs = @()
    foreach ($m in [regex]::Matches($content, '"path"\s*"([^"]+)"')) {
        $libs += $m.Groups[1].Value -replace '\\\\', '\'
    }
    foreach ($lib in $libs) {
        $candidate = Join-Path $lib 'steamapps\common\Deus Ex Mankind Divided'
        if (Test-Path (Join-Path $candidate 'retail\DXMD.exe')) {
            return $candidate
        }
    }
    return $null
}

if (-not $Game) {
    Write-Host "Auto-detecting DXMD install..." -ForegroundColor Cyan
    $Game = Find-DXMD
    if (-not $Game) {
        throw "Could not auto-detect DXMD. Pass -Game ""<path>""."
    }
    Write-Host "Found: $Game" -ForegroundColor Green
}

$retail = Join-Path $Game 'retail'
$exe    = Join-Path $retail 'DXMD.exe'
if (-not (Test-Path $exe)) {
    throw "DXMD.exe not at $exe -- is -Game correct?"
}

# -- Pre-flight conflict checks ------------------------------------------

$existingDxgi = Join-Path $retail 'dxgi.dll'
$conflict = $false
if (Test-Path $existingDxgi) {
    $sig = [System.IO.File]::ReadAllBytes($existingDxgi)
    # If it's ours (built from this project), the very first PE export
    # ordinal sequence will match. Cheaper check: compare file size to
    # our build.
    $ourSize  = (Get-Item $dll).Length
    $theirSize = $sig.Length
    if ($ourSize -eq $theirSize) {
        Write-Host "Existing dxgi.dll is same size as ours; assuming previous install. Overwriting." -ForegroundColor Yellow
    } else {
        Write-Host "An existing dxgi.dll is present (size $theirSize bytes)." -ForegroundColor Yellow
        Write-Host "  Most likely this is another mod (Special K, ReShade, ENB)." -ForegroundColor Yellow
        if (-not $Force) {
            throw "Aborting to avoid clobbering. Re-run with -Force to back it up and proceed."
        }
        $backup = "$existingDxgi.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
        Copy-Item $existingDxgi $backup -Force
        Write-Host "Backed up to: $backup" -ForegroundColor Yellow
        $conflict = $true
    }
}

# -- Install -------------------------------------------------------------

Copy-Item -Path $dll -Destination (Join-Path $retail 'dxgi.dll') -Force
Copy-Item -Path $ini -Destination (Join-Path $retail 'dxmd-thread-fix.ini') -Force

Write-Host "Installed:" -ForegroundColor Green
Get-ChildItem -Path $retail -Filter dxgi.dll | Select-Object FullName, Length, LastWriteTime | Format-Table -AutoSize
Get-ChildItem -Path $retail -Filter dxmd-thread-fix.ini | Select-Object FullName, Length, LastWriteTime | Format-Table -AutoSize
if ($conflict) {
    Write-Host "NOTE: another dxgi.dll mod was previously installed. Edit dxmd-thread-fix.ini if you want to change settings; uninstall via uninstall.ps1." -ForegroundColor Yellow
}
Write-Host "Launch DXMD; check $retail\dxmd-thread-fix.log to confirm the hook engaged." -ForegroundColor Cyan
