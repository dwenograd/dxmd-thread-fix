# uninstall.ps1 - removes dxgi.dll + dxmd-thread-fix.ini from a DXMD install.
#
# Safety:
#   - Only deletes dxgi.dll if its SHA-256 matches OUR built artifact.
#     If the dxgi.dll in retail/ is a different mod (ReShade, Special K),
#     this script refuses to delete it. Use -Force to override.
#   - If a backup from install.ps1 -Force exists (dxgi.dll.bak-<timestamp>),
#     restores the most recent one and cleans up all the others.
#
# Usage:
#   pwsh -File uninstall.ps1                # auto-detect Steam install
#   pwsh -File uninstall.ps1 -Game "..."    # manual path
#   pwsh -File uninstall.ps1 -Force         # delete dxgi.dll even if it
#                                           # doesn't match our hash

param(
    [string]$Game,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath
$ourDll = Join-Path $root 'dist\dxgi.dll'

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

# -- Identify our dxgi.dll by SHA-256 -----------------------------------

$dll = Join-Path $retail 'dxgi.dll'
$ini = Join-Path $retail 'dxmd-thread-fix.ini'
$log = Join-Path $retail 'dxmd-thread-fix.log'

if (Test-Path -LiteralPath $dll) {
    $theirHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
    $matches_ours = $false
    if (Test-Path -LiteralPath $ourDll) {
        $ourHash = (Get-FileHash -LiteralPath $ourDll -Algorithm SHA256).Hash
        $matches_ours = ($theirHash -eq $ourHash)
    }
    if ($matches_ours) {
        Remove-Item -LiteralPath $dll -Force
        Write-Host "Removed: $dll" -ForegroundColor Green
    } elseif ($Force) {
        Write-Host "WARNING: $dll has SHA-256 $theirHash, which does NOT match our build." -ForegroundColor Yellow
        Write-Host "         You passed -Force; deleting it anyway." -ForegroundColor Yellow
        Remove-Item -LiteralPath $dll -Force
        Write-Host "Removed: $dll" -ForegroundColor Yellow
    } else {
        Write-Host "REFUSING to delete $dll" -ForegroundColor Red
        Write-Host "  Its SHA-256 is $theirHash" -ForegroundColor Red
        Write-Host "  That does NOT match our build's SHA-256 - this is likely a DIFFERENT" -ForegroundColor Red
        Write-Host "  mod (ReShade, Special K, ENB, ...) that also proxies dxgi.dll." -ForegroundColor Red
        Write-Host "  Re-run with -Force to delete it anyway." -ForegroundColor Red
        Write-Host ""
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

# -- Restore prior dxgi.dll from backup, clean all other backups --------

$backups = Get-ChildItem -LiteralPath $retail -Filter 'dxgi.dll.bak-*' -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending
if ($backups.Count -gt 0) {
    $latest = $backups[0]
    Copy-Item -LiteralPath $latest.FullName -Destination $dll -Force
    Write-Host "Restored prior dxgi.dll from $($latest.Name)" -ForegroundColor Yellow
    # Clean up ALL backups including the restored one's source, so they
    # don't accumulate across install cycles.
    foreach ($bak in $backups) {
        Remove-Item -LiteralPath $bak.FullName -Force
    }
    Write-Host "Cleaned $($backups.Count) backup file(s)." -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "Uninstall complete." -ForegroundColor Green
