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
$hasSourceTree  = Test-Path -LiteralPath $dllSourceTree
$hasReleaseZip  = Test-Path -LiteralPath $dllReleaseZip
if ($hasSourceTree -and $hasReleaseZip) {
    # Ambiguous: both layouts present. Refuse rather than guess.
    $sthash = (Get-FileHash -LiteralPath $dllSourceTree -Algorithm SHA256).Hash
    $rzhash = (Get-FileHash -LiteralPath $dllReleaseZip -Algorithm SHA256).Hash
    if ($sthash -eq $rzhash) {
        # Same file in both locations - harmless, prefer the source-tree one
        $dll = $dllSourceTree
    } else {
        throw @"
Ambiguous install layout: BOTH of these files exist and they differ:
    $dllSourceTree    (source-tree layout, from build.ps1)
    $dllReleaseZip   (release-zip layout, from extracted release zip)

This happens if you extracted a release zip INTO a source checkout that
also has a built dist\dxgi.dll. To resolve, delete whichever one you
don't want to install, then re-run this script.
"@
    }
} elseif ($hasSourceTree) {
    $dll = $dllSourceTree
} elseif ($hasReleaseZip) {
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
#
# Why we do this and not just "try the install and see if it fails":
# the install touches dxgi.dll, which is the file the game depends on
# to launch. A failed half-install (e.g. created the backup but
# couldn't write the new DLL because the user wasn't an admin) would
# leave the game broken until they figured out how to manually restore.
# So instead we attempt to write a tiny temp file FIRST, fail with a
# clear "you need admin / move the game out of Program Files" message
# BEFORE touching any real files.
#
# The temp file name is `._dtf_writetest_<guid>.tmp` (GUID-random to
# avoid colliding with anything else in retail/) and is deleted in the
# same try block. A code-curious user will see this transient file
# appear if they're watching retail/ during install and we want them
# to be able to find this comment to understand what it was.
#
# NOTE: this preflight is NOT a transactional safety guarantee. A
# later Copy-Item -Force can still fail mid-operation (antivirus
# scan racing the write, sharing violations, disk full, transient
# I/O errors, etc.). The catch is the BACKUP file we create before
# overwriting the active dxgi.dll — if the copy fails or the user
# notices something wrong, the .bak file in retail/ is the recovery
# path. Uninstall.ps1 knows how to find and restore from it.
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
        # With -Force: back up the CURRENT foreign DLL, unless an
        # existing backup with the same content is already present.
        #
        # The original (subtler) logic here was "if any backup exists,
        # skip creating one." That preserves the user's pre-DTF
        # original (the oldest backup) but loses a different foreign
        # DLL if the user installed one between DTF -Force runs. For
        # example:
        #   1. User has ReShade installed. DTF -Force backs it up
        #      as dxgi.dll.bak-X and installs ours.
        #   2. User manually replaces our dxgi.dll with ENBSeries
        #      (DTF not uninstalled; ReShade backup still present).
        #   3. User re-runs DTF install -Force.
        #   4. Old logic: "backups exist, skip." ENB lost.
        # New logic: compute SHA-256 of the current foreign DLL,
        # compare to each existing backup. If no backup already
        # contains this exact file, create a NEW timestamped backup.
        # The oldest backup is still the user's original pre-DTF
        # state (we never delete backups in install.ps1; uninstall
        # restores the oldest specifically). Any intermediate foreign
        # DLLs the user installed between DTF runs end up in extra
        # backups — possibly more than necessary, but never less than
        # safe.
        $existingBackups = @(Get-ChildItem -LiteralPath $retail -Filter 'dxgi.dll.bak-*' -File -ErrorAction SilentlyContinue)
        $currentHash = (Get-FileHash -LiteralPath $existingDxgi -Algorithm SHA256).Hash
        $alreadyBackedUp = $false
        foreach ($eb in $existingBackups) {
            try {
                $ebHash = (Get-FileHash -LiteralPath $eb.FullName -Algorithm SHA256).Hash
                if ($ebHash -eq $currentHash) {
                    $alreadyBackedUp = $true
                    break
                }
            } catch {
                # Backup unreadable for some reason; treat as "not the
                # same content" so we err on the side of creating a
                # new backup rather than skipping.
            }
        }
        if ($alreadyBackedUp) {
            Write-Host "  Current foreign dxgi.dll matches an existing backup; not re-backing-up." -ForegroundColor Yellow
            Write-Host "  $($existingBackups.Count) prior backup file(s) preserved." -ForegroundColor Yellow
            Write-Host "  (The oldest backup is the original pre-DTF state.)" -ForegroundColor Yellow
        } else {
            if ($existingBackups.Count -gt 0) {
                Write-Host "  $($existingBackups.Count) prior backup file(s) present, but current foreign DLL differs from all of them." -ForegroundColor Yellow
                Write-Host "  Creating an additional backup so the current DLL can be restored later if needed." -ForegroundColor Yellow
            }
            # Use millisecond precision + a short random suffix so that
            # concurrent -Force runs can't collide on identical filenames.
            $stamp  = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
            $suffix = -join ((1..4) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) })
            $backup = "$existingDxgi.bak-$stamp-$suffix"
            # Pass -Force:$false explicitly so the user's
            # $PSDefaultParameterValues can't silently add -Force and
            # overwrite a pre-existing backup file with the same name.
            # (Belt-and-suspenders: the timestamp+random suffix makes
            # collision astronomically unlikely already.)
            Copy-Item -LiteralPath $existingDxgi -Destination $backup -Force:$false
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
