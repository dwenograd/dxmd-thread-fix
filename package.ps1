# package.ps1 - build and assemble a release zip.
#
# Produces (in release\):
#   dxmd-thread-fix-v<VERSION>.zip
#   dxmd-thread-fix-v<VERSION>-SHA256SUMS.txt   (per-file manifest, also embedded in zip)
#   dxmd-thread-fix-v<VERSION>-headers.txt      (dumpbin /headers output)
#   dxmd-thread-fix-v<VERSION>-exports.txt      (dumpbin /exports output)
#
# The zip contains everything an end-user needs:
#   dxgi.dll
#   dxmd-thread-fix.ini
#   README.md
#   LICENSE
#   CHANGELOG.md
#   SHA256SUMS.txt
#   install.ps1, uninstall.ps1 (optional, for users who want them)
#   third_party/minhook/LICENSE.txt
#   third_party/minhook/PROVENANCE.md
#
# Does NOT include source code (that's on GitHub).
#
# Usage:
#   pwsh -File package.ps1                  # build then package
#   pwsh -File package.ps1 -SkipBuild       # use existing dist/dxgi.dll
#   pwsh -File package.ps1 -Version 1.0.0   # override version string

param(
    [string]$Version = '1.0.0',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath
Set-Location -LiteralPath $root

# Build first, unless told to skip.
if (-not $SkipBuild) {
    Write-Host "=== Building Release ===" -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root\build.ps1" -Config Release
    if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed with exit code $LASTEXITCODE" }
}

$dist  = Join-Path $root 'dist'
$dll   = Join-Path $dist 'dxgi.dll'
if (-not (Test-Path -LiteralPath $dll)) {
    throw "dist\dxgi.dll not found. Run build.ps1 first or omit -SkipBuild."
}

# -- Stage the release content -----------------------------------------

$release = Join-Path $root 'release'
$stage   = Join-Path $release "dxmd-thread-fix-v$Version"
New-Item -ItemType Directory -Path $release -Force | Out-Null
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Write-Host "=== Staging release into $stage ===" -ForegroundColor Cyan

# Top-level files
Copy-Item -LiteralPath $dll                                          -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'dxmd-thread-fix.ini')       -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'README.md')                 -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'LICENSE')                   -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'CHANGELOG.md')              -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'install.ps1')               -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'uninstall.ps1')             -Destination $stage

# MinHook attribution (BSD-2-Clause requires preserving the license)
$mhDest = Join-Path $stage 'third_party\minhook'
New-Item -ItemType Directory -Path $mhDest -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'third_party\minhook\LICENSE.txt')    -Destination $mhDest
Copy-Item -LiteralPath (Join-Path $root 'third_party\minhook\PROVENANCE.md')  -Destination $mhDest

# -- Compute hashes for the manifest -----------------------------------

Write-Host "=== Hashing release contents ===" -ForegroundColor Cyan
$manifest = @()
$manifest += "# SHA-256 hashes for dxmd-thread-fix v$Version"
$manifest += "# Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
$manifest += "#"
$manifest += "# This manifest does NOT include its own hash (would be circular)."
$manifest += "# The hash of SHA256SUMS.txt itself is published on the GitHub release page."
$manifest += ""
Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel  = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    $manifest += "$hash  $rel"
    Write-Host "  $hash  $rel" -ForegroundColor DarkGray
}

$shaFile = Join-Path $release "dxmd-thread-fix-v$Version-SHA256SUMS.txt"
Set-Content -LiteralPath $shaFile -Value ($manifest -join "`r`n") -Encoding ASCII

# Also copy the manifest INTO the staging folder so it ships in the zip.
# Users get a single zip with everything they need to verify integrity
# locally, not "DLL here, hashes on a separate web page somewhere."
$inZipSha = Join-Path $stage 'SHA256SUMS.txt'
Set-Content -LiteralPath $inZipSha -Value ($manifest -join "`r`n") -Encoding ASCII

# -- Generate dumpbin artifacts (alongside the zip, not inside) ---------
#
# README's trust section promises these so suspicious users can inspect
# the DLL's PE headers and exports without having dumpbin installed.
# We locate dumpbin via vswhere, same pattern as build.ps1.

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $vs = & $vswhere -latest -property installationPath
    $dumpbin = Get-ChildItem -LiteralPath "$vs\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
    if ($dumpbin) {
        Write-Host "=== Generating dumpbin artifacts ===" -ForegroundColor Cyan
        $headersFile = Join-Path $release "dxmd-thread-fix-v$Version-headers.txt"
        $exportsFile = Join-Path $release "dxmd-thread-fix-v$Version-exports.txt"
        & $dumpbin.FullName /headers $dll > $headersFile
        & $dumpbin.FullName /exports $dll > $exportsFile
        Write-Host "  $headersFile" -ForegroundColor DarkGray
        Write-Host "  $exportsFile" -ForegroundColor DarkGray
    } else {
        Write-Host "WARN: dumpbin not found; skipping headers/exports text generation." -ForegroundColor Yellow
    }
} else {
    Write-Host "WARN: vswhere not found; skipping headers/exports text generation." -ForegroundColor Yellow
}

# -- Build the zip -----------------------------------------------------

$zip = Join-Path $release "dxmd-thread-fix-v$Version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }

Write-Host "=== Creating $zip ===" -ForegroundColor Cyan
# Compress-Archive with -LiteralPath + a mixed array of files and a
# directory has quirky behavior in PS 5.1 (silently drops top-level
# files in some cases). Use the .NET API directly for predictable results.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory(
    $stage, $zip,
    [IO.Compression.CompressionLevel]::Optimal,
    $false)   # do NOT include the staging folder name as a top-level entry

$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash

Write-Host ""
Write-Host "BUILD OK" -ForegroundColor Green
Write-Host "  Zip:       $zip"
Write-Host "  Zip SHA-256: $zipHash"
Write-Host "  Manifest:  $shaFile"
Write-Host ""
Write-Host "Release-page text:" -ForegroundColor Cyan
Write-Host "---"
Write-Host "**dxmd-thread-fix v$Version**"
Write-Host ""
Write-Host "Download: ``dxmd-thread-fix-v$Version.zip``"
Write-Host "SHA-256:   ``$zipHash``"
Write-Host ""
Write-Host "Per-file hashes: ``dxmd-thread-fix-v$Version-SHA256SUMS.txt``"
Write-Host "---"
