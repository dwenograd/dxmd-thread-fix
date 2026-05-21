# install.ps1 - copies dxgi.dll + dxmd-thread-fix.ini into a DXMD install.
#
# Usage:
#   pwsh -File install.ps1
#       (auto-detect Steam install)
#   pwsh -File install.ps1 -Game "X:\Path\To\Deus Ex Mankind Divided"
#       (manual path, for non-Steam or unusual installs)
#   pwsh -File install.ps1 -Force
#       (back up any existing dxgi.dll, even if it's not ours)
#
# What it does:
#   1. Locate the game's retail\ folder.
#   2. Verify it contains DXMD.exe and isn't running.
#   3. Verify we can write to retail\.
#   4. If a dxgi.dll is already there, compare its SHA-256 to ours:
#        - same hash    -> ours, harmlessly overwrite
#        - different    -> NOT ours (probably ReShade/Special K/ENB);
#                          require -Force, back it up with a timestamp
#   5. Copy dxgi.dll and (if missing) dxmd-thread-fix.ini.
#   6. Print the next-step instructions.

param(
    [string]$Game,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath
$dll  = Join-Path $root 'dist\dxgi.dll'
$ini  = Join-Path $root 'dxmd-thread-fix.ini'

if (-not (Test-Path -LiteralPath $dll)) {
    throw "Built dxgi.dll not found at $dll. Run build.ps1 first, or extract the release zip first."
}
if (-not (Test-Path -LiteralPath $ini)) {
    throw "dxmd-thread-fix.ini not found at $ini."
}

# -- Auto-detect game install if not specified ---------------------------

function Find-DXMD {
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
    if (-not $steamPath) {
        Write-Host "Note: Steam not detected in registry." -ForegroundColor Yellow
        return $null
    }
    $vdf = Join-Path $steamPath 'steamapps\libraryfolders.vdf'
    if (-not (Test-Path -LiteralPath $vdf)) {
        Write-Host "Note: Steam libraryfolders.vdf not found at $vdf." -ForegroundColor Yellow
        return $null
    }
    $content = Get-Content -LiteralPath $vdf -Raw
    $libs = @()
    foreach ($m in [regex]::Matches($content, '"path"\s*"([^"]+)"')) {
        $libs += $m.Groups[1].Value -replace '\\\\', '\'
    }
    foreach ($lib in $libs) {
        $candidate = Join-Path $lib 'steamapps\common\Deus Ex Mankind Divided'
        if (Test-Path -LiteralPath (Join-Path $candidate 'retail\DXMD.exe')) {
            return $candidate
        }
    }
    Write-Host "Note: DXMD not found in any Steam library." -ForegroundColor Yellow
    return $null
}

if (-not $Game) {
    Write-Host "Auto-detecting DXMD install in Steam libraries..." -ForegroundColor Cyan
    $Game = Find-DXMD
    if (-not $Game) {
        throw @"
Could not auto-detect DXMD in Steam.

If you have a non-Steam install (Epic, GOG, manual), re-run with -Game pointing at the game folder, e.g.:
    pwsh -File install.ps1 -Game "C:\Games\Deus Ex Mankind Divided"

The game folder is the one CONTAINING the 'retail' subdirectory (i.e. retail\DXMD.exe is one level inside it).

Microsoft Store / Xbox Game Pass installs of DXMD are NOT supported - their app sandbox blocks dxgi.dll proxying.
"@
    }
    Write-Host "Found: $Game" -ForegroundColor Green
}

$retail = Join-Path $Game 'retail'
$exe    = Join-Path $retail 'DXMD.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    throw "DXMD.exe not at $exe. Pass -Game pointing at the folder that CONTAINS the 'retail' subdirectory."
}

# -- Pre-flight: is DXMD currently running? -----------------------------

$running = Get-Process -Name 'DXMD' -ErrorAction SilentlyContinue
if ($running) {
    throw "DXMD.exe is currently running (PID $($running.Id)). Close the game and try again."
}

# -- Pre-flight: can we write to retail/? -------------------------------

$testFile = Join-Path $retail "._dtf_writetest_$([guid]::NewGuid().ToString('N')).tmp"
try {
    [System.IO.File]::WriteAllText($testFile, "test")
    Remove-Item -LiteralPath $testFile -Force
} catch {
    throw @"
Cannot write to $retail.

This usually means the game is installed under C:\Program Files (or another protected location) and you're running PowerShell as a normal user.

Either:
  - Re-run this script as Administrator (right-click PowerShell -> 'Run as administrator'), or
  - Move/reinstall the game to a folder you can write to (e.g. C:\Games\ or D:\Steam\).

Underlying error: $($_.Exception.Message)
"@
}

# -- SHA-256 identity check on any existing dxgi.dll --------------------

$ourHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
$existingDxgi = Join-Path $retail 'dxgi.dll'
$conflict = $false
if (Test-Path -LiteralPath $existingDxgi) {
    $theirHash = (Get-FileHash -LiteralPath $existingDxgi -Algorithm SHA256).Hash
    if ($theirHash -eq $ourHash) {
        Write-Host "Existing dxgi.dll matches our SHA-256; harmless reinstall, overwriting." -ForegroundColor DarkGray
    } else {
        Write-Host "An EXISTING dxgi.dll is present but its SHA-256 does not match ours:" -ForegroundColor Yellow
        Write-Host "  existing: $theirHash"
        Write-Host "  ours    : $ourHash"
        Write-Host "  Most likely this is another mod (Special K, ReShade, ENB) that also proxies dxgi.dll." -ForegroundColor Yellow
        if (-not $Force) {
            throw "Aborting to avoid clobbering the other mod. Re-run with -Force to back it up (as dxgi.dll.bak-<timestamp>) and proceed."
        }
        # Before making a new backup, clean any pre-existing dtf-style
        # backups so they don't accumulate across install cycles.
        Get-ChildItem -LiteralPath $retail -Filter 'dxgi.dll.bak-*' -File -ErrorAction SilentlyContinue | ForEach-Object {
            Remove-Item -LiteralPath $_.FullName -Force
            Write-Host "  Cleaned old backup: $($_.Name)" -ForegroundColor DarkGray
        }
        $backup = "$existingDxgi.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
        Copy-Item -LiteralPath $existingDxgi -Destination $backup -Force
        Write-Host "  Backed up existing dxgi.dll -> $backup" -ForegroundColor Yellow
        $conflict = $true
    }
}

# -- Install ------------------------------------------------------------

Copy-Item -LiteralPath $dll -Destination (Join-Path $retail 'dxgi.dll') -Force
# Don't clobber an existing INI - user may have customized LogicalProcessors.
$dstIni = Join-Path $retail 'dxmd-thread-fix.ini'
if (-not (Test-Path -LiteralPath $dstIni)) {
    Copy-Item -LiteralPath $ini -Destination $dstIni
}

Write-Host ""
Write-Host "Installed:" -ForegroundColor Green
Get-Item -LiteralPath (Join-Path $retail 'dxgi.dll') | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
Get-Item -LiteralPath $dstIni | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
Write-Host "Our dxgi.dll SHA-256: $ourHash" -ForegroundColor DarkGray

if ($conflict) {
    Write-Host ""
    Write-Host "NOTE: another dxgi.dll mod was previously installed and has been backed up." -ForegroundColor Yellow
    Write-Host "      The two mods cannot coexist as both want to be dxgi.dll." -ForegroundColor Yellow
    Write-Host "      Run uninstall.ps1 to restore the backup."  -ForegroundColor Yellow
}
Write-Host ""
Write-Host "Launch DXMD; verify the fix engaged by checking:" -ForegroundColor Cyan
Write-Host "  $retail\dxmd-thread-fix.log" -ForegroundColor Cyan
Write-Host "Look for a line containing 'FIX STATUS: ACTIVE'." -ForegroundColor Cyan
