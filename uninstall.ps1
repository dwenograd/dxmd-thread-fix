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

# -- Identify the installed dxgi.dll ------------------------------------

$dll = Join-Path $retail 'dxgi.dll'
$ini = Join-Path $retail 'dxmd-thread-fix.ini'
$log = Join-Path $retail 'dxmd-thread-fix.log'

if (Test-Path -LiteralPath $dll) {
    $isOurs = Test-IsOurs -Path $dll
    if ($isOurs) {
        $ver = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
        Remove-Item -LiteralPath $dll -Force
        Write-Host "Removed: $dll (was dxmd-thread-fix v$ver)" -ForegroundColor Green
    } elseif ($DeleteForeignDxgi) {
        $product = ''
        try { $product = (Get-Item -LiteralPath $dll).VersionInfo.ProductName } catch { }
        Write-Host "WARNING: $dll does NOT identify as dxmd-thread-fix" -ForegroundColor Yellow
        Write-Host "  VERSIONINFO ProductName: '$product'" -ForegroundColor Yellow
        Write-Host "  You passed -DeleteForeignDxgi; deleting anyway." -ForegroundColor Yellow
        Remove-Item -LiteralPath $dll -Force
        Write-Host "Removed: $dll" -ForegroundColor Yellow
    } else {
        $product = ''
        try { $product = (Get-Item -LiteralPath $dll).VersionInfo.ProductName } catch { }
        Write-Host "REFUSING to delete $dll" -ForegroundColor Red
        Write-Host "  Its VERSIONINFO ProductName is: '$product'" -ForegroundColor Red
        Write-Host "  That is NOT 'dxmd-thread-fix' - this is likely a DIFFERENT" -ForegroundColor Red
        Write-Host "  mod (ReShade, Special K, ENB, ...) that also proxies dxgi.dll." -ForegroundColor Red
        Write-Host "" -ForegroundColor Red
        Write-Host "  If you are SURE this is meant to be dxmd-thread-fix (e.g. a build" -ForegroundColor Red
        Write-Host "  too old to have VERSIONINFO), re-run with -DeleteForeignDxgi." -ForegroundColor Red
        Write-Host "" -ForegroundColor Red
        Write-Host "  (Leaving dxmd-thread-fix.ini and dxmd-thread-fix.log alone too)" -ForegroundColor Red
        exit 1
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

# -- Restore the OLDEST backup (= pre-DTF original) and clean up --------

$backups = @(Get-ChildItem -LiteralPath $retail -Filter 'dxgi.dll.bak-*' -File -ErrorAction SilentlyContinue)
# Sort by the timestamp embedded in the filename (dxgi.dll.bak-YYYYMMDD-HHMMSS),
# NOT by LastWriteTime. Copy-Item preserves the source DLL's modification
# timestamp, so the file's LastWriteTime reflects when the *original* DLL
# was built, not when WE made the backup. The filename's timestamp suffix
# is authoritative for "which backup was made first."
$backups = $backups | Sort-Object -Property @{
    Expression = {
        if ($_.Name -match 'dxgi\.dll\.bak-(\d{8}-\d{6})$') {
            [datetime]::ParseExact($matches[1], 'yyyyMMdd-HHmmss', $null)
        } else {
            $_.CreationTime  # fallback for unparseable names
        }
    }
}
if ($backups.Count -gt 0) {
    $oldest = $backups[0]
    Copy-Item -LiteralPath $oldest.FullName -Destination $dll -Force
    Write-Host "Restored pre-dxmd-thread-fix dxgi.dll from oldest backup:" -ForegroundColor Yellow
    Write-Host "  $($oldest.Name)" -ForegroundColor Yellow
    # Clean up ALL backups (including the one we just restored from).
    foreach ($bak in $backups) {
        Remove-Item -LiteralPath $bak.FullName -Force
    }
    if ($backups.Count -gt 1) {
        Write-Host "  Cleaned $($backups.Count) backup file(s)." -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "Uninstall complete." -ForegroundColor Green
