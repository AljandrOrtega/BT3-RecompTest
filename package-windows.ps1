#!/usr/bin/env pwsh
# Package a Windows release stage into a portable zip (mirrors tools/release/package.sh).
#
#   .\package-windows.ps1                                    # uses defaults
#   .\package-windows.ps1 -StageDir "C:\...\stage" -OutDir "C:\out"
param(
    [string]$StageDir = "",
    [string]$OutDir = "",
    [switch]$NoDesktopCopy
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ROOT = $PSScriptRoot
$RELEASE_BASE = Join-Path $ROOT "build\release-windows"
$OUT = if ($OutDir) { $OutDir } else { Join-Path $RELEASE_BASE "out" }
$STAGE = if ($StageDir) { $StageDir } else { Join-Path $OUT "stage" }
$TREE_NAME = "Dragon Ball Budokai Tenkaichi 3 Recompiled"

function Fail($msg) { Write-Error $msg; exit 1 }
function Log($msg) { Write-Host "  $msg" }
function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }

if (-not (Test-Path $STAGE)) { Fail "Stage dir not found: $STAGE (run build-windows.ps1 first)" }
if (-not ((Test-Path (Join-Path $STAGE "Launcher.exe")) -and
          (Test-Path (Join-Path $STAGE "bt3-runner.exe")) -and
          (Test-Path (Join-Path $STAGE "qt.conf")) -and
          (Test-Path (Join-Path $STAGE "lib")))) {
    Fail "Stage incomplete (Launcher.exe, bt3-runner.exe, qt.conf, lib/ required)"
}

# ─── Assemble portable tree ────────────────────────────────────────────────────
Step "Assembling portable tree"
$tmpTree = Join-Path $OUT $TREE_NAME
Remove-Item -Recurse -Force $tmpTree -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $tmpTree | Out-Null

# The stage is already a flat, self-contained tree: Launcher.exe, bt3-runner.exe,
# Qt6 + VC runtime DLLs and qt.conf next to the executables, Qt plugins under
# lib\qt6\plugins, assets flattened. Mirror it verbatim and add the empty save
# directory the launcher expects.
Get-ChildItem -Force $STAGE | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $tmpTree $_.Name) -Recurse -Force
}
New-Item -ItemType Directory -Force -Path (Join-Path $tmpTree "savedata\BASLUS-21678DBZT3") | Out-Null
Log "Tree assembled: $tmpTree"

# ─── Create zip ────────────────────────────────────────────────────────────────
Step "Creating zip archive"
Push-Location $OUT
$zipFile = "BT3-Recomp-x86_64.zip"
Remove-Item $zipFile -Force -ErrorAction SilentlyContinue

# Prefer bsdtar (ships with Git for Windows / Windows 10+)
$useTar = $false
$tarExe = Get-Command tar.exe -ErrorAction SilentlyContinue
if ($tarExe) {
    $tarVer = & tar.exe --version
    if ($tarVer -match "bsdtar|libarchive") { $useTar = $true }
}

if ($useTar) {
    Log "Using bsdtar (libarchive) to create zip"
    & tar.exe -a -cf $zipFile $TREE_NAME
} else {
    Log "Using Compress-Archive to create zip"
    Compress-Archive -Path (Join-Path $OUT $TREE_NAME) -DestinationPath (Join-Path $OUT $zipFile) -CompressionLevel Optimal
}
Pop-Location
Log "$zipFile created"

# ─── SHA-256 checksum ──────────────────────────────────────────────────────────
Step "Computing SHA-256"
$zipPath = Join-Path $OUT $zipFile
$hash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash
$shaContent = "$hash  $zipFile"
$shaPath = Join-Path $OUT "BT3-Recomp-x86_64.sha256"
$shaContent | Out-File -FilePath $shaPath -Encoding ASCII
Log "SHA-256 written: $shaPath"

# ─── Cleanup temp tree ─────────────────────────────────────────────────────────
Remove-Item -Recurse -Force $tmpTree -ErrorAction SilentlyContinue

# ─── Copy to Desktop ────────────────────────────────────────────────────────────
$desktopPath = ""
if (-not $NoDesktopCopy) {
    Step "Copying release to Desktop"
    $desktopPath = [Environment]::GetFolderPath("Desktop")
    if (-not $desktopPath) { $desktopPath = Join-Path $env:USERPROFILE "Desktop" }
    Copy-Item $zipPath $desktopPath -Force
    Copy-Item $shaPath $desktopPath -Force
    Log "Copied to: $desktopPath"
}

# ─── Summary ────────────────────────────────────────────────────────────────────
$zipInfo = Get-Item $zipPath
Step "Release artifact"
Write-Host "  Zip:  $zipPath  ($([math]::Round($zipInfo.Length / 1MB, 1)) MB)"
Write-Host "  Sha:  $shaPath"
Write-Host "  Stage: $STAGE"
Write-Host ""
Write-Host "  Deploy ready: $zipPath" -ForegroundColor Green
if ($desktopPath) {
    Write-Host "  Desktop copy: $desktopPath\$zipFile" -ForegroundColor Green
    Write-Host "    (unzip and run Launcher.exe)"
}
