#!/usr/bin/env pwsh
# Install all dependencies needed to build BT3-Recomp natively on Windows
# (no Docker/WSL). Safe to re-run: skips anything already installed.
#
#   .\install-deps-windows.ps1             # check only
#   .\install-deps-windows.ps1 -Install   # install what's missing (winget/pip)
#
# Installs:
#   - VS Build Tools 2022 (ClangCL + Win11 SDK)          [winget]
#   - CMake >= 3.21                                       [winget]
#   - Ninja                                               [winget]
#   - Python 3                                            [winget]
#   - aqtinstall + pefile (pip)  + Qt 6.5.3 via aqtinstall
param(
    [switch]$Install
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$BUILD = Join-Path $PSScriptRoot "build"
# Actualizamos a una versión de Qt compatible con Clang-CL y C++ moderno
$QT_VERSION = "6.6.3" 
$QT_HOST = "windows"
$QT_TARGET = "win64_msvc2019_64"   
$QT_INSTALL_DIR = "msvc2019_64"    
$QT_BASE = Join-Path $BUILD "qt"
$QT_ROOT = Join-Path $QT_BASE "$QT_VERSION\$QT_INSTALL_DIR"

function Log($msg) { Write-Host "  $msg" }
function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Error $msg; exit 1 }

function Test-FileCmd($name) {
    if ($PSVersionTable.PSEdition -eq "Core") {
        $ext = if ($IsWindows) { ".exe" } else { "" }
        $exe = $name + $ext
    } else {
        $exe = $name + ".exe"
    }
    return [bool](Get-Command $exe -ErrorAction SilentlyContinue)
}

# Run a native command line through cmd.exe so pip/aqtinstall progress and
# warnings written to stderr do not become terminating NativeCommandError
# records under $ErrorActionPreference = "Stop".
function Invoke-CmdLine([string]$Line) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { cmd.exe /d /s /c $Line } finally { $ErrorActionPreference = $prev }
    return $LASTEXITCODE
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

Step "Checking prerequisites"
$missing = @()

# VS Build Tools (with ClangCL)
$hasVS = $false
if (Test-Path $vswhere) {
    $foundPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $hasVS = [bool]$foundPath
}
if (-not $hasVS) { $missing += "VS Build Tools 2022 (ClangCL + Win11 SDK)" }

# CMake (>= 3.21 for DOWNLOAD_EXTRACT_TIMESTAMP)
$cmakeVersion = ""
if (Test-FileCmd "cmake") {
    $cmakeVersion = (& cmake --version | Select-Object -First 1) -replace '^cmake version (\d+)\.(\d+).*','$1.$2'
    if ([version]"$cmakeVersion" -lt [version]"3.21") { $missing += "CMake >= 3.21 (found $cmakeVersion)" }
} else { $missing += "CMake" }

# Ninja
if (-not (Test-FileCmd "ninja")) { $missing += "Ninja" }

# Python 3
if (-not (Test-FileCmd "python")) { $missing += "Python 3" }

if ($missing.Count -gt 0) {
    Write-Host ("Missing: " + ($missing -join ", ")) -ForegroundColor Yellow
} else {
    Log "All core tools present (VS, CMake $cmakeVersion, Ninja, Python)"
}

# Python packages + Qt
$pipPresent = $false
if (Test-FileCmd "python") {
    Step "Installing Python packages (aqtinstall, pefile)"
    $pipRc = Invoke-CmdLine "python -m pip install --quiet --upgrade --disable-pip-version-check --no-warn-script-location aqtinstall pefile 2>nul"
    if ($pipRc -ne 0) { Fail "pip install failed (exit $pipRc)" }
    $pipPresent = $true
    Log "aqtinstall + pefile ready"
}

$qtPresent = Test-Path $QT_ROOT
if ($pipPresent) {
    if ($qtPresent) {
        Log "Qt found at $QT_ROOT"
    } else {
        Step "Downloading Qt $QT_VERSION ($QT_HOST, $QT_TARGET) via aqtinstall"
        $aqtRc = Invoke-CmdLine "python -m aqt install-qt $QT_HOST desktop $QT_VERSION $QT_TARGET --outputdir `"$QT_BASE`" 2>nul"
        if ($aqtRc -ne 0) { Fail "aqt install-qt failed (exit $aqtRc)" }
        if (-not (Test-Path $QT_ROOT)) { Fail "Qt not found at $QT_ROOT after install" }
        Log "Qt installed: $QT_ROOT"
    }
}

if (-not $Install) {
    if ($missing.Count -gt 0 -or -not $qtPresent) {
        Write-Host "`nRun '.\install-deps-windows.ps1 -Install' to install missing components." -ForegroundColor Yellow
    }
    exit 0
}

# ─── Install phase ─────────────────────────────────────────────────────────────
if ($missing.Count -gt 0) {
    # Si estamos en GitHub Actions, no usar winget para herramientas base (ya están preinstaladas en el runner)
    if ($env:GITHUB_ACTIONS -eq "true") {
        Log "Running in GitHub Actions: Skipping winget installation for core tools ($($missing -join ', '))."
    } else {
        Step "Installing missing prerequisites via winget"

        if (-not $hasVS) {
            Step "Installing VS Build Tools 2022 (this takes several minutes)"
            $override = "--quiet --wait --norestart " +
                "--add Microsoft.VisualStudio.Workload.VCTools " +
                "--add Microsoft.VisualStudio.Component.VC.Llvm.Clang " +
                "--add Microsoft.VisualStudio.Component.VC.Tools.LLVM " +
                "--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 " +
                "--add Microsoft.VisualStudio.Component.Windows11SDK.22621 " +
                "--includeRecommended"
            winget install -e --id Microsoft.VisualStudio.2022.BuildTools `
                --accept-source-agreements --accept-package-agreements --override $override
            if ($LASTEXITCODE -ne 0) { Fail "winget install VS Build Tools failed (exit $LASTEXITCODE)" }
            Log "VS Build Tools installed"
        }

        if (-not (Test-FileCmd "cmake")) {
            winget install -e --id Kitware.CMake --accept-source-agreements --accept-package-agreements
            $env:PATH = "$env:LOCALAPPDATA\Microsoft\WinGet\Links" + [System.IO.Path]::PathSeparator + $env:PATH
            if (-not (Test-FileCmd "cmake")) { Fail "cmake installed but not on PATH" }
        }

        if (-not (Test-FileCmd "ninja")) {
            winget install -e --id Ninja-build.Ninja --accept-source-agreements --accept-package-agreements
            $env:PATH = "$env:LOCALAPPDATA\Microsoft\WinGet\Links" + [System.IO.Path]::PathSeparator + $env:PATH
            if (-not (Test-FileCmd "ninja")) { Fail "ninja installed but not on PATH" }
        }

        if (-not (Test-FileCmd "python")) {
            winget install -e --id Python.Python.3.12 --accept-source-agreements --accept-package-agreements
            $env:PATH = "$env:LOCALAPPDATA\Programs\Python\Python312\Scripts" + [System.IO.Path]::PathSeparator + $env:PATH
            if (-not (Test-FileCmd "python")) { Fail "python installed but not on PATH; open a new terminal" }
        }
    }
}

Step "Done"
Write-Host "  All dependencies ready. Now run '.\build-windows.ps1'." -ForegroundColor Green
