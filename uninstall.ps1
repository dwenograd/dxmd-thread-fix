# uninstall.ps1 - removes dxgi.dll + dxmd-thread-fix.ini from a DXMD install.
#
# If a backup of a prior dxgi.dll (created by install.ps1 -Force) exists,
# it is restored.
#
# Usage:
#   pwsh -File uninstall.ps1 -Game "N:\Steam\steamapps\common\Deus Ex Mankind Divided"

param(
    [string]$Game
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath

# Re-use the auto-detect from install.ps1 (duplicated for standalone use).

function Find-DXMD-Local {
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
    $Game = Find-DXMD-Local
    if (-not $Game) { throw "Could not auto-detect DXMD. Pass -Game ""<path>""." }
}
$retail = Join-Path $Game 'retail'
if (-not (Test-Path (Join-Path $retail 'DXMD.exe'))) {
    throw "DXMD.exe not found at $retail"
}

$dll = Join-Path $retail 'dxgi.dll'
$ini = Join-Path $retail 'dxmd-thread-fix.ini'
$log = Join-Path $retail 'dxmd-thread-fix.log'

if (Test-Path $dll) {
    Remove-Item $dll -Force
    Write-Host "Removed: $dll" -ForegroundColor Green
}
if (Test-Path $ini) {
    Remove-Item $ini -Force
    Write-Host "Removed: $ini" -ForegroundColor Green
}
if (Test-Path $log) {
    Remove-Item $log -Force
    Write-Host "Removed: $log" -ForegroundColor Green
}

# Restore the most-recent backup, if any.
$backups = Get-ChildItem -Path $retail -Filter 'dxgi.dll.bak-*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
if ($backups.Count -gt 0) {
    $latest = $backups[0]
    Copy-Item $latest.FullName $dll -Force
    Remove-Item $latest.FullName -Force
    Write-Host "Restored prior dxgi.dll from $($latest.Name)" -ForegroundColor Yellow
}

Write-Host "Uninstall complete." -ForegroundColor Green
