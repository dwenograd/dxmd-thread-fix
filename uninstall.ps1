# uninstall.ps1 - removes dxgi.dll + dxmd-thread-fix.ini from a DXMD install.
#
# Identity model: same as install.ps1 — "ours" is identified by the
# VERSIONINFO ProductName field embedded in the DLL ("dxmd-thread-fix").
# This robustly recognizes any version of this tool, including upgrades
# and downgrades.
#
# Force-flag semantics (designed to prevent accidental destruction of
# other mods):
#   no flags                -> safe default. Deletes the dxgi.dll ONLY IF
#                              it's identifiably ours (VERSIONINFO check).
#                              Refuses with a clear message otherwise.
#   -DeleteForeignDxgi      -> EXPLICIT override. Deletes the dxgi.dll
#                              even if it doesn't look like ours. Use
#                              this if you're sure the foreign DLL is
#                              dtf-without-VERSIONINFO or something
#                              equivalent. ALWAYS prefer running this
#                              from a fresh dtf checkout.
#
# Backup handling: if any dxgi.dll.bak-* files exist (from install.ps1
# -Force backing up a foreign mod), restores the OLDEST backup. The
# oldest is the original pre-DTF state — what the user wants back.
# Then deletes ALL backups to keep retail\ tidy.
#
# Usage:
#   pwsh -File uninstall.ps1
#   pwsh -File uninstall.ps1 -Game "X:\Path\To\Deus Ex Mankind Divided"
#   pwsh -File uninstall.ps1 -DeleteForeignDxgi

param(
    [string]$Game,
    [switch]$DeleteForeignDxgi
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath

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
    if (-not $steamPath) { return $null }
    $vdf = Join-Path $steamPath 'steamapps\libraryfolders.vdf'
    if (-not (Test-Path -LiteralPath $vdf)) { return $null }
    # Include $steamPath itself as the first library candidate (it's
    # not always listed in libraryfolders.vdf), parse new-style "path"
    # entries (Steam ~2019+), and old-style "<num>" entries (pre-2019).
    # See install.ps1's equivalent logic for full rationale.
    $libs = @($steamPath)
    $content = Get-Content -LiteralPath $vdf -Raw
    foreach ($m in [regex]::Matches($content, '"path"\s*"([^"]+)"')) {
        $libs += $m.Groups[1].Value -replace '\\\\', '\'
    }
    foreach ($m in [regex]::Matches($content, '"\d+"\s*"([A-Za-z]:[^"]+)"')) {
        $libs += $m.Groups[1].Value -replace '\\\\', '\'
    }
    $libs = $libs | Select-Object -Unique
    foreach ($lib in $libs) {
        $candidate = Join-Path $lib 'steamapps\common\Deus Ex Mankind Divided'
        if (Test-Path -LiteralPath (Join-Path $candidate 'retail\DXMD.exe')) {
            return $candidate
        }
    }
    return $null
}

if (-not $Game) {
    $Game = Find-DXMD
    if (-not $Game) {
        throw "Could not auto-detect DXMD. Pass -Game pointing at the game folder."
    }
}
$retail = Join-Path $Game 'retail'
if (-not (Test-Path -LiteralPath (Join-Path $retail 'DXMD.exe'))) {
    throw "DXMD.exe not at $retail. Pass -Game correctly."
}

# -- Pre-flight: DXMD not running? --------------------------------------

$running = Get-Process -Name 'DXMD' -ErrorAction SilentlyContinue
if ($running) {
    throw "DXMD.exe is currently running (PID $($running.Id)). Close the game and try again."
}

# -- Plan the work (validate everything BEFORE deleting anything) -------
#
# We don't want a half-done uninstall that deletes the active dxgi.dll
# and then fails to restore a backup. So: gather all the inputs,
# validate them, then execute every step. Refuse early if anything is
# wrong rather than committing partial changes.

$dll = Join-Path $retail 'dxgi.dll'
$ini = Join-Path $retail 'dxmd-thread-fix.ini'
$log = Join-Path $retail 'dxmd-thread-fix.log'

# 1. Decide whether we'll delete dxgi.dll.
$dllAction = 'none'
$dllReasonForRefusal = $null
$dllVer = $null
if (Test-Path -LiteralPath $dll) {
    if (Test-IsOurs -Path $dll) {
        $dllAction = 'delete-ours'
        $dllVer = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
    } elseif ($DeleteForeignDxgi) {
        $dllAction = 'delete-foreign-forced'
    } else {
        $dllAction = 'refuse'
        $product = ''
        try { $product = (Get-Item -LiteralPath $dll).VersionInfo.ProductName } catch { }
        $dllReasonForRefusal = $product
    }
}

# 2. Enumerate backups and parse their filename timestamps. We use
#    TryParseExact in invariant culture so a malformed name doesn't
#    throw mid-operation. Backups that don't match our exact naming
#    scheme are reported but excluded from the "restore the oldest"
#    decision — we won't trust their ordering.
$allBackups = @(Get-ChildItem -LiteralPath $retail -Filter 'dxgi.dll.bak-*' -File -ErrorAction SilentlyContinue)
$parsedBackups = @()
$unparseableBackups = @()
$ci = [System.Globalization.CultureInfo]::InvariantCulture
foreach ($bak in $allBackups) {
    # Match both the short form (yyyyMMdd-HHmmss) created by older DTF
    # versions AND the long form (yyyyMMdd-HHmmss-fff[-hex]) used now.
    if ($bak.Name -match '^dxgi\.dll\.bak-(\d{8}-\d{6}(?:-\d{3})?)(?:-[0-9a-f]+)?$') {
        $stampStr = $matches[1]
        $stamp = [datetime]::MinValue
        $formats = @('yyyyMMdd-HHmmss-fff', 'yyyyMMdd-HHmmss')
        $ok = [datetime]::TryParseExact($stampStr, $formats, $ci, [System.Globalization.DateTimeStyles]::None, [ref]$stamp)
        if ($ok) {
            $parsedBackups += [pscustomobject]@{
                File = $bak; Stamp = $stamp
            }
            continue
        }
    }
    $unparseableBackups += $bak
}
$parsedBackups = $parsedBackups | Sort-Object Stamp
$restoreFrom   = $null
if ($parsedBackups.Count -gt 0) {
    $restoreFrom = $parsedBackups[0]
}

# 3. Validate plan. Refuse if anything is off and we'd produce a
#    partially-uninstalled state.
if ($dllAction -eq 'refuse') {
    Write-Host "REFUSING to delete $dll" -ForegroundColor Red
    Write-Host "  Its VERSIONINFO ProductName is: '$dllReasonForRefusal'" -ForegroundColor Red
    Write-Host "  That is NOT 'dxmd-thread-fix' - this is likely a DIFFERENT" -ForegroundColor Red
    Write-Host "  mod (ReShade, Special K, ENB, ...) that also proxies dxgi.dll." -ForegroundColor Red
    Write-Host "" -ForegroundColor Red
    Write-Host "  If you are SURE this is meant to be dxmd-thread-fix (e.g. a build" -ForegroundColor Red
    Write-Host "  too old to have VERSIONINFO), re-run with -DeleteForeignDxgi." -ForegroundColor Red
    Write-Host "" -ForegroundColor Red
    Write-Host "  (Leaving dxmd-thread-fix.ini and dxmd-thread-fix.log alone too)" -ForegroundColor Red
    exit 1
}
if ($unparseableBackups.Count -gt 0) {
    Write-Host "REFUSING to proceed: $($unparseableBackups.Count) backup file(s) have" -ForegroundColor Red
    Write-Host "names that don't match the expected 'dxgi.dll.bak-YYYYMMDD-HHMMSS[-fff[-hex]]' format:" -ForegroundColor Red
    foreach ($u in $unparseableBackups) {
        Write-Host "  $($u.Name)" -ForegroundColor Red
    }
    Write-Host "" -ForegroundColor Red
    Write-Host "Rather than guess which one is your pre-DTF original, please:" -ForegroundColor Red
    Write-Host "  1. Inspect each backup manually," -ForegroundColor Red
    Write-Host "  2. Rename or delete the unrecognized one(s)," -ForegroundColor Red
    Write-Host "  3. Re-run this script." -ForegroundColor Red
    exit 1
}

# 4. Execute. Order:
#    - If there's a backup to restore: overwrite the active dxgi.dll
#      with the oldest backup using Copy-Item -Force, then delete
#      every backup file. We deliberately do NOT do "delete the
#      active DLL, then copy the backup in its place" because that
#      sequence has an intermediate state where no dxgi.dll exists
#      in retail/. If the script crashed there, the user would have
#      no DLL at all. The Copy-Item -Force keeps a file at $dll for
#      the entire operation. (Copy-Item is NOT a transactional
#      atomic-replace primitive — an interrupted copy can still
#      leave a partially-written file at $dll — but it's strictly
#      better than the delete-then-copy alternative.)
#    - If there's no backup: delete the active DLL only if we
#      agreed to above (dllAction == delete-ours or
#      delete-foreign-forced).
#    - Then delete the INI and the log file regardless.

if ($restoreFrom) {
    # Copy the oldest backup over the active DLL. -Force ensures we
    # overwrite (the file is the dxmd-thread-fix DLL we just verified
    # by VERSIONINFO identity check above; this is what we want).
    #
    # Same caveat as install.ps1: Copy-Item is not atomic. If the
    # restore is interrupted, dxgi.dll could be left partially
    # written. The user's recovery path in that case is to keep the
    # remaining .bak files (we delete them BELOW only after a
    # successful restore call) and manually copy one over dxgi.dll
    # by hand. (We don't implement atomic-rename for v1.0 — see the
    # note in install.ps1.)
    Copy-Item -LiteralPath $restoreFrom.File.FullName -Destination $dll -Force
    Write-Host "Restored pre-dxmd-thread-fix dxgi.dll from oldest backup:" -ForegroundColor Yellow
    Write-Host "  $($restoreFrom.File.Name)  (stamp $($restoreFrom.Stamp.ToString('s')))" -ForegroundColor Yellow
    foreach ($pb in $parsedBackups) {
        Remove-Item -LiteralPath $pb.File.FullName -Force
    }
    if ($parsedBackups.Count -gt 1) {
        Write-Host "  Cleaned $($parsedBackups.Count) backup file(s)." -ForegroundColor DarkGray
    }
} else {
    # No backup. Just delete the active DLL (if there is one and we agreed to).
    if ($dllAction -eq 'delete-ours') {
        Remove-Item -LiteralPath $dll -Force
        Write-Host "Removed: $dll (was dxmd-thread-fix v$dllVer)" -ForegroundColor Green
    } elseif ($dllAction -eq 'delete-foreign-forced') {
        Remove-Item -LiteralPath $dll -Force
        Write-Host "Removed: $dll (forced delete; was not dxmd-thread-fix)" -ForegroundColor Yellow
    }
}

if (Test-Path -LiteralPath $ini) {
    Remove-Item -LiteralPath $ini -Force
    Write-Host "Removed: $ini" -ForegroundColor Green
}
if (Test-Path -LiteralPath $log) {
    Remove-Item -LiteralPath $log -Force
    Write-Host "Removed: $log" -ForegroundColor Green
}

Write-Host ""
Write-Host "Uninstall complete." -ForegroundColor Green
