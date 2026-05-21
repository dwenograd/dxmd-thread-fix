# install.ps1 - copies dxgi.dll + dxmd-thread-fix.ini into a DXMD install.
#
# Identity model:
#   "Ours" is identified by the VERSIONINFO ProductName field embedded
#   in our dxgi.dll ("dxmd-thread-fix"). This is more robust than a hash
#   match — it correctly identifies any version of this tool, including
#   upgrades and downgrades, without needing a hash manifest.
#
# Behavior:
#   - No existing dxgi.dll        -> install our DLL.
#   - Existing dxgi.dll is ours   -> overwrite silently (upgrade/reinstall).
#   - Existing dxgi.dll is FOREIGN -> refuse unless -Force. With -Force,
#     create a timestamped backup ONLY IF no backup already exists. The
#     first backup is sacred — it's the user's original pre-DTF state
#     (e.g. ReShade) — and we never overwrite it on subsequent upgrades.
#
# Usage:
#   pwsh -File install.ps1
#       (auto-detect Steam install)
#   pwsh -File install.ps1 -Game "X:\Path\To\Deus Ex Mankind Divided"
#       (manual path, for non-Steam or unusual installs)
#   pwsh -File install.ps1 -Force
#       (overwrite a foreign dxgi.dll, backing it up if not already)

param(
    [string]$Game,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath

# The DLL we install can be in one of two layouts:
#   - Source-tree layout:    $root\dist\dxgi.dll
#   - Release-zip layout:    $root\dxgi.dll
# This script is shipped in both, so we accept either.
$dllSourceTree  = Join-Path $root 'dist\dxgi.dll'
$dllReleaseZip  = Join-Path $root 'dxgi.dll'
if (Test-Path -LiteralPath $dllSourceTree) {
    $dll = $dllSourceTree
} elseif (Test-Path -LiteralPath $dllReleaseZip) {
    $dll = $dllReleaseZip
} else {
    throw @"
Built dxgi.dll not found.

Looked in both:
    $dllSourceTree    (source-tree layout, after build.ps1)
    $dllReleaseZip   (release-zip layout, after extracting the release zip)

If you cloned the source repo: run build.ps1 first.
If you downloaded a release zip: make sure dxgi.dll is in the same folder as install.ps1
(i.e. extract the zip with its directory structure intact).
"@
}
$ini  = Join-Path $root 'dxmd-thread-fix.ini'

# -- Identity helper: is a given dxgi.dll "ours"? -----------------------

function Test-IsOurs {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    try {
        $vi = (Get-Item -LiteralPath $Path).VersionInfo
        return $vi.ProductName -eq 'dxmd-thread-fix'
    } catch {
        return $false
    }
}

# -- Auto-detect game install if not specified --------------------------

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

# -- Pre-flight: DXMD not running? --------------------------------------

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

# -- Identify and handle any existing dxgi.dll --------------------------

$ourHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
$existingDxgi = Join-Path $retail 'dxgi.dll'
$conflict = $false
if (Test-Path -LiteralPath $existingDxgi) {
    if (Test-IsOurs -Path $existingDxgi) {
        # Same tool, possibly different version. Safe to overwrite —
        # no backup needed, no -Force needed, no surprises.
        $existingVer = (Get-Item -LiteralPath $existingDxgi).VersionInfo.FileVersion
        Write-Host "Existing dxgi.dll is dxmd-thread-fix v$existingVer; overwriting." -ForegroundColor DarkGray
    } else {
        # Foreign DLL. Almost certainly another mod (ReShade, Special K,
        # ENB). Do NOT overwrite without -Force.
        $existingProduct = ''
        try { $existingProduct = (Get-Item -LiteralPath $existingDxgi).VersionInfo.ProductName } catch { }
        Write-Host "An EXISTING dxgi.dll is present that is NOT dxmd-thread-fix:" -ForegroundColor Yellow
        if ($existingProduct) {
            Write-Host "  ProductName: '$existingProduct'" -ForegroundColor Yellow
        } else {
            Write-Host "  (no VERSIONINFO ProductName embedded)" -ForegroundColor Yellow
        }
        Write-Host "  Most likely this is another mod (Special K, ReShade, ENB, ...)." -ForegroundColor Yellow
        if (-not $Force) {
            throw "Aborting to avoid clobbering the other mod. Re-run with -Force to back it up (as dxgi.dll.bak-<timestamp>) and install ours on top."
        }
        # With -Force: back up the foreign DLL, but ONLY IF we don't
        # already have a backup. The first backup is the user's original
        # pre-DTF state — we must never overwrite it on subsequent runs,
        # because that's the file uninstall.ps1 needs to restore.
        $existingBackups = @(Get-ChildItem -LiteralPath $retail -Filter 'dxgi.dll.bak-*' -File -ErrorAction SilentlyContinue)
        if ($existingBackups.Count -gt 0) {
            Write-Host "  $($existingBackups.Count) prior backup file(s) already present; preserving them." -ForegroundColor Yellow
            Write-Host "  (The oldest backup is the original pre-DTF state.)" -ForegroundColor Yellow
        } else {
            # Use millisecond precision + a short random suffix so that
            # concurrent -Force runs can't collide on identical filenames.
            $stamp  = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
            $suffix = -join ((1..4) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) })
            $backup = "$existingDxgi.bak-$stamp-$suffix"
            # Don't pass -Force here: if (improbably) a file with that
            # exact name already exists, we want a loud failure rather
            # than silently overwriting it.
            Copy-Item -LiteralPath $existingDxgi -Destination $backup
            Write-Host "  Backed up foreign dxgi.dll -> $(Split-Path $backup -Leaf)" -ForegroundColor Yellow
        }
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
$ourVer = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
Write-Host "Our dxgi.dll version: $ourVer" -ForegroundColor DarkGray

if ($conflict) {
    Write-Host ""
    Write-Host "NOTE: another dxgi.dll mod was previously installed and has been backed up." -ForegroundColor Yellow
    Write-Host "      The two mods cannot coexist as both want to be dxgi.dll." -ForegroundColor Yellow
    Write-Host "      Run uninstall.ps1 to remove dxmd-thread-fix and restore the backup."  -ForegroundColor Yellow
}
Write-Host ""
Write-Host "Launch DXMD; verify the fix engaged by checking:" -ForegroundColor Cyan
Write-Host "  $retail\dxmd-thread-fix.log" -ForegroundColor Cyan
Write-Host "Look for a line containing 'FIX STATUS: ACTIVE'." -ForegroundColor Cyan
