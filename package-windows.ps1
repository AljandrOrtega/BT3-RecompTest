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
          (Test-Path (Join-Path $STAGE "Launcher.bat")) -and
          (Test-Path (Join-Path $STAGE "lib")))) {
    Fail "Stage incomplete (Launcher.exe, bt3-runner.exe, Launcher.bat, lib/ required)"
}

# ─── Assemble portable tree ────────────────────────────────────────────────────
Step "Assembling portable tree"
$tmpTree = Join-Path $OUT $TREE_NAME
Remove-Item -Recurse -Force $tmpTree -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $tmpTree | Out-Null

Copy-Item (Join-Path $STAGE "Launcher.exe") $tmpTree -Force
Copy-Item (Join-Path $STAGE "bt3-runner.exe") $tmpTree -Force
Copy-Item (Join-Path $STAGE "Launcher.bat") $tmpTree -Force
Copy-Item (Join-Path $STAGE "lib") (Join-Path $tmpTree "lib") -Recurse -Force
if (Test-Path (Join-Path $STAGE "assets")) {
    Copy-Item (Join-Path $STAGE "assets") (Join-Path $tmpTree "assets") -Recurse -Force
}
Copy-Item (Join-Path $STAGE "LICENSE") $tmpTree -Force
if (Test-Path (Join-Path $STAGE "COPYING.LGPLv3")) {
    Copy-Item (Join-Path $STAGE "COPYING.LGPLv3") $tmpTree -Force
}
$saveDst = Join-Path $tmpTree "savedata\BASLUS-21678DBZT3"
New-Item -ItemType Directory -Force -Path $saveDst | Out-Null
if (Test-Path (Join-Path $STAGE "savedata\fps60_sites.txt")) {
    Copy-Item (Join-Path $STAGE "savedata\fps60_sites.txt") (Join-Path $tmpTree "savedata") -Force
}
if (Test-Path (Join-Path $STAGE "savedata\settings.toml")) {
    Copy-Item (Join-Path $STAGE "savedata\settings.toml") (Join-Path $tmpTree "savedata") -Force
}
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
    $tarVer = & tar.exe --version 2>$null
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
