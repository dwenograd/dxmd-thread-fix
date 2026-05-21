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
# Manifest is strict coreutils-format: pure `<sha256><two spaces><path>`
# lines, LF endings, ASCII, no comments. So `sha256sum -c SHA256SUMS.txt`
# works on Linux/macOS/Git-for-Windows without warnings. Explanatory
# notes (including "this manifest excludes its own hash") live in the
# README and the GitHub release page, NOT in the manifest itself.
$manifestLines = @()
Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel  = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifestLines += "$hash  $rel"
    Write-Host "  $hash  $rel" -ForegroundColor DarkGray
}
$manifestText = ($manifestLines -join "`n") + "`n"

# Write with LF endings, no BOM. .NET WriteAllBytes lets us control
# encoding precisely; Set-Content / Out-File default to CRLF on Windows.
$shaFile = Join-Path $release "dxmd-thread-fix-v$Version-SHA256SUMS.txt"
[System.IO.File]::WriteAllBytes($shaFile, [System.Text.Encoding]::ASCII.GetBytes($manifestText))

# Also copy the manifest INTO the staging folder so it ships in the zip.
# Same strict format. The manifest does not include its own hash; that
# fact is documented in README, not in the manifest (so consumers can
# pipe it directly into `sha256sum -c`).
$inZipSha = Join-Path $stage 'SHA256SUMS.txt'
[System.IO.File]::WriteAllBytes($inZipSha, [System.Text.Encoding]::ASCII.GetBytes($manifestText))

# -- Generate dumpbin artifacts (alongside the zip, not inside) ---------
#
# README's trust section promises these so suspicious users can inspect
# the DLL's PE headers, exports, and imports without having dumpbin
# installed. We:
#   1. cd into dist/ so the output paths are bare "dxgi.dll", not
#      the absolute K:\... build path (a leak that prior reviewers
#      flagged for the public release).
#   2. Capture stdout and write it as ASCII (no UTF-16 BOM that
#      PowerShell `>` would otherwise produce on 5.1).

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $vs = & $vswhere -latest -property installationPath
    $dumpbin = Get-ChildItem -LiteralPath "$vs\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
    if ($dumpbin) {
        Write-Host "=== Generating dumpbin artifacts ===" -ForegroundColor Cyan
        $headersFile = Join-Path $release "dxmd-thread-fix-v$Version-headers.txt"
        $exportsFile = Join-Path $release "dxmd-thread-fix-v$Version-exports.txt"
        $importsFile = Join-Path $release "dxmd-thread-fix-v$Version-imports.txt"
        Push-Location -LiteralPath $dist
        try {
            $headersText = & $dumpbin.FullName /headers 'dxgi.dll' | Out-String
            $exportsText = & $dumpbin.FullName /exports 'dxgi.dll' | Out-String
            $importsText = & $dumpbin.FullName /imports 'dxgi.dll' | Out-String
        } finally { Pop-Location }
        [System.IO.File]::WriteAllBytes($headersFile, [System.Text.Encoding]::ASCII.GetBytes($headersText))
        [System.IO.File]::WriteAllBytes($exportsFile, [System.Text.Encoding]::ASCII.GetBytes($exportsText))
        [System.IO.File]::WriteAllBytes($importsFile, [System.Text.Encoding]::ASCII.GetBytes($importsText))
        Write-Host "  $headersFile" -ForegroundColor DarkGray
        Write-Host "  $exportsFile" -ForegroundColor DarkGray
        Write-Host "  $importsFile" -ForegroundColor DarkGray
    } else {
        Write-Host "WARN: dumpbin not found; skipping headers/exports/imports text generation." -ForegroundColor Yellow
    }
} else {
    Write-Host "WARN: vswhere not found; skipping headers/exports/imports text generation." -ForegroundColor Yellow
}

# -- Build the zip -----------------------------------------------------

$zip = Join-Path $release "dxmd-thread-fix-v$Version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }

Write-Host "=== Creating $zip ===" -ForegroundColor Cyan
# Build the zip manually so we control:
#  - entry name normalization (forward slashes for nested paths, per
#    standard zip practice and what `tar -tf` etc. expect)
#  - compression level
#  - file ordering (sorted by relative path for reproducibility)
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipStream = [System.IO.File]::Create($zip)
$archive   = [System.IO.Compression.ZipArchive]::new($zipStream, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem -LiteralPath $stage -Recurse -File |
        Sort-Object { $_.FullName.Substring($stage.Length + 1).Replace('\', '/') } |
        ForEach-Object {
            $rel   = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
            $entry = $archive.CreateEntry($rel, [System.IO.Compression.CompressionLevel]::Optimal)
            $entryStream = $entry.Open()
            $srcStream   = [System.IO.File]::OpenRead($_.FullName)
            try   { $srcStream.CopyTo($entryStream) }
            finally {
                $srcStream.Close()
                $entryStream.Close()
            }
        }
} finally {
    $archive.Dispose()
    $zipStream.Dispose()
}

$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
$dllHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash

Write-Host ""
Write-Host "BUILD OK" -ForegroundColor Green
Write-Host "  Zip:         $zip"
Write-Host "  Zip SHA-256: $zipHash"
Write-Host "  DLL SHA-256: $dllHash"
Write-Host "  Manifest:    $shaFile"
Write-Host ""
Write-Host "Release-page text:" -ForegroundColor Cyan
Write-Host "---"
Write-Host "**dxmd-thread-fix v$Version**"
Write-Host ""
Write-Host "Download: ``dxmd-thread-fix-v$Version.zip``"
Write-Host "  zip SHA-256:    ``$zipHash``"
Write-Host "  dxgi.dll SHA-256: ``$dllHash``"
Write-Host ""
Write-Host "Provenance artifacts (alongside the zip on this release page):"
Write-Host "  - ``dxmd-thread-fix-v$Version-SHA256SUMS.txt`` (per-file manifest, coreutils-compatible)"
Write-Host "  - ``dxmd-thread-fix-v$Version-headers.txt`` (dumpbin /headers output)"
Write-Host "  - ``dxmd-thread-fix-v$Version-exports.txt`` (dumpbin /exports output)"
Write-Host "  - ``dxmd-thread-fix-v$Version-imports.txt`` (dumpbin /imports output)"
Write-Host ""
Write-Host "Verify locally:"
Write-Host '  PowerShell:  (Get-FileHash .\dxgi.dll -Algorithm SHA256).Hash'
Write-Host '  Linux/macOS: sha256sum -c SHA256SUMS.txt   (manifest is in the zip)'
Write-Host "---"
