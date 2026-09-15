#!/usr/bin/env pwsh
# Build BT3-Recomp natively on Windows (no Docker/WSL).
#
#   .\build-windows.ps1                                          # interactive (asks ISO + output)
#   .\build-windows.ps1 -Iso "C:\path\to\bt3.iso"             # non-interactive
#   .\build-windows.ps1 -SkipSetup -OutDir "C:\out"           # reuse generated sources
#
# Requires: VS Build Tools 2022 (ClangCL + Win11 SDK), CMake, Ninja, Python 3,
#           Qt 6.5.3 (win64_msvc2022_64), aqtinstall, pefile.
# First run installs all missing prerequisites via winget/pip (interactive).
param(
    [string]$Iso = "",
    [string]$OutDir = "",
    [int]$Jobs = [System.Environment]::ProcessorCount,
    [switch]$SkipSetup,
    [switch]$SkipDeps,
    [switch]$SkipPackage
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ROOT = $PSScriptRoot
$BUILD = Join-Path $ROOT "build"
$GAME_DIR = Join-Path $ROOT "games\bt3"
$LAUNCHER_SRC = Join-Path $ROOT "ps2xRuntime\src\launcher"
$LAUNCHER_BUILD = Join-Path $BUILD "launcher_qt"
$PGS_DIR = Join-Path $ROOT "ps2xRuntime\third_party\parallel-gs"
$ISO_DEFAULT = "C:\Users\Rexx\Desktop\DragonBall Z - Budokai Tenkaichi 3 (USA) (En,Ja).iso"
$QT_VERSION = "6.5.3"
$QT_HOST = "windows"
# Qt 6.5.x ships only the msvc2019_64 kit; it is ABI-compatible with MSVC 2022.
$QT_TARGET = "win64_msvc2019_64"   # aqtinstall arch name for the download
$QT_INSTALL_DIR = "msvc2019_64"    # directory aqt creates under <outputdir>/<version>
$QT_BASE = Join-Path $BUILD "qt"
$QT_ROOT = Join-Path $QT_BASE "$QT_VERSION\$QT_INSTALL_DIR"
$STAGE_NAME = "stage"
$RELEASE_BASE = Join-Path $BUILD "release-windows"
$OUT = if ($OutDir) { $OutDir } else { Join-Path $RELEASE_BASE "out" }
$STAGE = Join-Path $OUT $STAGE_NAME

# ─── Helpers ────────────────────────────────────────────────────────────────────
function Log($msg) { Write-Host "  $msg" }
function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Error $msg; exit 1 }

function Test-FileCmd($name) {
    if ($PSVersionTable.PSEdition -eq "Core") { $exe = $name + (if ($IsWindows) { ".exe" } else { "" }) }
    else { $exe = $name + ".exe" }
    return [bool](Get-Command $exe -ErrorAction SilentlyContinue)
}

# ─── VS environment ─────────────────────────────────────────────────────────────
function Import-VSEnvironment {
    Step "Locating Visual Studio Build Tools"
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        Fail ("vswhere not found. Install VS Build Tools 2022 with 'C++ Clang Compiler for Windows'.`n" +
              "  winget install -e --id Microsoft.VisualStudio.2022.BuildTools " +
              "--override `"--quiet --wait --norestart " +
              "--add Microsoft.VisualStudio.Workload.VCTools " +
              "--add Microsoft.VisualStudio.Component.VC.Llvm.Clang " +
              "--add Microsoft.VisualStudio.Component.VC.Tools.LLVM " +
              "--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 " +
              "--add Microsoft.VisualStudio.Component.Windows11SDK.22621 " +
              "--includeRecommended`"")
    }
    $foundPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $foundPath) {
        Fail ("VS Build Tools not found. Install with:`n" +
              "  winget install -e --id Microsoft.VisualStudio.2022.BuildTools " +
              "--override `"--quiet --wait --norestart " +
              "--add Microsoft.VisualStudio.Workload.VCTools " +
              "--add Microsoft.VisualStudio.Component.VC.Llvm.Clang " +
              "--add Microsoft.VisualStudio.Component.VC.Tools.LLVM " +
              "--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 " +
              "--add Microsoft.VisualStudio.Component.Windows11SDK.22621 " +
              "--includeRecommended`"")
    }
    Log "VS: $foundPath"

    $vcvars = Join-Path $foundPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { Fail "vcvars64.bat not found at $vcvars" }

    Step "Loading VS environment (vcvars64)"
    $envBlock = & cmd.exe /d /s /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $envBlock) {
        if ($line -match "^([^=]+)=(.*)$") {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
        }
    }
    Log "INCLUDE loaded: $([bool]$env:INCLUDE)"
    return $foundPath
}

# ─── Prerequisites ──────────────────────────────────────────────────────────────
function Ensure-Prerequisites {
    Step "Checking prerequisites"
    $missing = @()

    # VS Build Tools
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $hasVS = $false
    if (Test-Path $vswhere) {
        $hasVS = [bool](& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    }
    if (-not $hasVS) { $missing += "VS Build Tools 2022 (ClangCL + Win11 SDK)" }

    # CMake (>= 3.21 for DOWNLOAD_EXTRACT_TIMESTAMP)
    if (Test-FileCmd "cmake") {
        $cmakeVer = (& cmake --version | Select-Object -First 1) -replace '^cmake version (\d+)\.(\d+).*','$1.$2'
        if ([version]"$cmakeVer" -lt [version]"3.21") { $missing += "CMake >= 3.21 (found $cmakeVer)" }
    } else { $missing += "CMake" }

    # Ninja
    if (-not (Test-FileCmd "ninja")) { $missing += "Ninja" }

    # Python3
    if (-not (Test-FileCmd "python")) { $missing += "Python 3" }

    if ($missing.Count -gt 0) {
        Step "Installing missing prerequisites via winget"
        Write-Host ("Missing: " + ($missing -join ", ")) -ForegroundColor Yellow

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

        if (-not (Test-FileCmd "ninja")) {
            winget install -e --id Ninja-build.Ninja --accept-source-agreements --accept-package-agreements
            $env:PATH = "$env:LOCALAPPDATA\Microsoft\WinGet\Links" + [System.IO.Path]::PathSeparator + $env:PATH
            if (-not (Test-FileCmd "ninja")) { Fail "ninja installed but not on PATH" }
        }
    }

    Step "Installing Python packages (aqtinstall, pefile)"
    python -m pip install --quiet --upgrade aqtinstall pefile 2>$null
    if ($LASTEXITCODE -ne 0) { Fail "pip install failed" }
    Log "aqtinstall + pefile ready"

    # Qt via aqtinstall (skip if already present)
    if (-not (Test-Path $QT_ROOT)) {
        Step "Downloading Qt $QT_VERSION ($QT_HOST, $QT_TARGET) via aqtinstall"
        python -m aqt install-qt $QT_HOST desktop $QT_VERSION $QT_TARGET --outputdir $QT_BASE
        if ($LASTEXITCODE -ne 0) { Fail "aqt install-qt failed (exit $LASTEXITCODE)" }
        if (-not (Test-Path $QT_ROOT)) { Fail "Qt not found at $QT_ROOT after install" }
        Log "Qt installed: $QT_ROOT"
    } else {
        Log "Qt found at $QT_ROOT"
    }
}

# ─── ISO prompt ─────────────────────────────────────────────────────────────────
if (-not $SkipSetup) {
    if (-not $Iso) {
        $readIso = Read-Host "BT3 ISO path (Enter = $ISO_DEFAULT)"
        $Iso = if ($readIso) { $readIso } else { $ISO_DEFAULT }
    }
    $Iso = $Iso.Trim('"')
    if (-not (Test-Path $Iso)) { Fail "ISO not found: $Iso" }
}

if (-not $OutDir) {
    $readOut = Read-Host "Deploy output directory (Enter = $OUT)"
    $OutDir = if ($readOut) { $readOut } else { $OUT }
    $OutDir = $OutDir.Trim('"')
}
$OUT = $OutDir
$STAGE = Join-Path $OUT $STAGE_NAME

# ─── Main ───────────────────────────────────────────────────────────────────────
$sw = [Diagnostics.Stopwatch]::StartNew()

if (-not $SkipDeps) { Ensure-Prerequisites }
$vsPath = Import-VSEnvironment

# Ensure clang-cl and Ninja are on PATH after VS env + prereqs
$env:PATH = $env:PATH + [System.IO.Path]::PathSeparator + (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links")

# Pre-configure the runner build dir so setup.py reuses it (avoids re-configuration
# without this flag). PS2X_SHOW_WINDOWS_CONSOLE=OFF sets the runner's PE subsystem
# to Windows GUI (/SUBSYSTEM:WINDOWS + /ENTRY:mainCRTStartup) which the release gate
# expects — the launcher then launches it as a child process without a console window.
Step "Pre-configuring runner (PS2X_SHOW_WINDOWS_CONSOLE=OFF)"
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& cmake -S $ROOT -B $BUILD `
    -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
    -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_STUDIO=OFF `
    -DPS2X_SHOW_WINDOWS_CONSOLE=OFF *> $null
$ErrorActionPreference = $prevEAP
if ($LASTEXITCODE -ne 0) { Fail "Runner cmake preconfigure failed" }

if ($SkipSetup) {
    Step "Reusing existing generated sources (rebuild runner + deploy)"
    if (-not (Test-Path (Join-Path $GAME_DIR "work\SLUS_216.78"))) {
        Fail "--skip-setup requires existing games/bt3/work/SLUS_216.78"
    }
    # Clean previous stage to avoid stale/doubly-nested assets
    Remove-Item -Recurse -Force $STAGE -ErrorAction SilentlyContinue
    $setupArgs = @(
        "--skip-setup", "--deploy", $STAGE,
        "--jobs", "$Jobs"
    )
    python (Join-Path $GAME_DIR "setup.py") @setupArgs
    if ($LASTEXITCODE -ne 0) { Fail "setup.py --skip-setup failed (exit $LASTEXITCODE)" }
    Log "Runner rebuilt + deployed"
} else {
    Step "Running setup.py (extract ISO, build recompiler, generate sources, build runner)"
    $setupArgs = @(
        $Iso, "--deploy", $STAGE,
        "--jobs", "$Jobs"
    )
    python (Join-Path $GAME_DIR "setup.py") @setupArgs
    if ($LASTEXITCODE -ne 0) { Fail "setup.py failed (exit $LASTEXITCODE)" }
    Log "Runner built"
}

# Rename runner (idempotent: a previous skip-setup run may have already staged it)
$existingBt3Runner = Join-Path $STAGE "bt3-runner.exe"
$stageRunner = Get-ChildItem $STAGE -Filter "ps2EntryRunner.exe" -ErrorAction SilentlyContinue |
               Select-Object -First 1
if (-not $stageRunner) {
    if (Test-Path $existingBt3Runner) {
        Log "bt3-runner.exe already staged; skipping rename"
    } else {
        $buildRunner = Get-ChildItem $BUILD -Recurse -Filter "ps2EntryRunner.exe" -ErrorAction SilentlyContinue |
                       Select-Object -First 1
        if ($buildRunner) {
            Log "Found runner at $($buildRunner.FullName); copying to stage"
            New-Item -ItemType Directory -Force -Path $STAGE | Out-Null
            Copy-Item $buildRunner.FullName (Join-Path $STAGE "bt3-runner.exe") -Force
            Remove-Item $buildRunner.FullName -Force -ErrorAction SilentlyContinue
        } else {
            Fail "ps2EntryRunner.exe not found after build"
        }
    }
} else {
    $dest = Join-Path $STAGE "bt3-runner.exe"
    Move-Item $stageRunner.FullName $dest -Force
    Log "Renamed ps2EntryRunner.exe -> bt3-runner.exe"
}

# ─── Build Qt launcher ─────────────────────────────────────────────────────────
Step "Building Qt launcher"
New-Item -ItemType Directory -Force -Path $LAUNCHER_BUILD | Out-Null
$cmakeLauncherArgs = @(
    "-S", $LAUNCHER_SRC,
    "-B", $LAUNCHER_BUILD,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_CXX_COMPILER=clang-cl",
    "-DCMAKE_C_COMPILER=clang-cl",
    "-DCMAKE_PREFIX_PATH=$QT_ROOT",
    "-DPS2X_CMAKE_EXTRA_ROOTS=$QT_ROOT"
)
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& cmake @cmakeLauncherArgs *> $null
$ErrorActionPreference = $prevEAP
if ($LASTEXITCODE -ne 0) { Fail "Launcher cmake configure failed" }
$ErrorActionPreference = "Continue"
& cmake --build $LAUNCHER_BUILD -j $Jobs *> $null
$ErrorActionPreference = $prevEAP
if ($LASTEXITCODE -ne 0) { Fail "Launcher build failed" }
$launcherExe = Join-Path $LAUNCHER_BUILD "Launcher.exe"
if (-not (Test-Path $launcherExe)) { Fail "Launcher.exe not found after build" }
Copy-Item $launcherExe (Join-Path $STAGE "Launcher.exe") -Force
Log "Launcher.exe -> stage"

# ─── Bundle DLLs ────────────────────────────────────────────────────────────────
Step "Bundling DLLs"
$stageLib = Join-Path $STAGE "lib"
New-Item -ItemType Directory -Force -Path $stageLib | Out-Null

# Qt6 DLLs (only release — debug *d.dll require debug MSVC runtime the gate rejects)
$qtBin = Join-Path $QT_ROOT "bin"
if (Test-Path $qtBin) {
    Get-ChildItem $qtBin -Filter "Qt6*.dll" |
        Where-Object { $_.Name -notmatch "d\.dll$" } |
        ForEach-Object {
            Copy-Item $_.FullName (Join-Path $stageLib $_.Name) -Force
        }
    Log "Qt6 release DLLs copied"
} else { Fail "Qt bin dir not found: $qtBin" }

# Qt plugins (only release — no debug *d.dll)
$qtPluginSrc = Get-ChildItem $QT_ROOT -Directory -Filter "plugins" -ErrorAction SilentlyContinue |
               Select-Object -First 1
$qtPluginDst = Join-Path $stageLib "qt6\plugins"
New-Item -ItemType Directory -Force -Path $qtPluginDst | Out-Null
if ($qtPluginSrc) {
    # Copy the directory tree but exclude debug plugin DLLs and SQL driver
    # plugins (qsqlpsql.dll needs the non-system LIBPQ.dll; not used here).
    Copy-Item "$($qtPluginSrc.FullName)\*" $qtPluginDst -Recurse -Force
    Get-ChildItem $qtPluginDst -Recurse -Filter "*d.dll" | Remove-Item -Force
    Remove-Item -Recurse -Force (Join-Path $qtPluginDst "sqldrivers") -ErrorAction SilentlyContinue
    Log "Qt plugins (release) copied"
} else { Log "WARNING: Qt plugins dir not found" }

# FFmpeg DLLs (staged next to runner by CMake POST_BUILD)
Get-ChildItem $STAGE -Filter "avcodec-*.dll" -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $stageLib $_.Name) -Force }
Get-ChildItem $STAGE -Filter "avformat-*.dll" -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $stageLib $_.Name) -Force }
Get-ChildItem $STAGE -Filter "avutil-*.dll" -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $stageLib $_.Name) -Force }
Get-ChildItem $STAGE -Filter "swresample-*.dll" -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $stageLib $_.Name) -Force }
Get-ChildItem $STAGE -Filter "swscale-*.dll" -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $stageLib $_.Name) -Force }
Log "FFmpeg DLLs -> lib/"

# VC++ runtime DLLs — find AMD64 copies from the VS Redist
$vcDlls = @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll")
foreach ($dll in $vcDlls) {
    $found = $null
    # Search the x64 Redist directory tree (not x86)
    if ($vsPath) {
        $redist = Join-Path $vsPath "VC\Redist\MSVC"
        if (Test-Path $redist) {
            $candidates = Get-ChildItem $redist -Recurse -Filter $dll -ErrorAction SilentlyContinue |
                          Where-Object { $_.DirectoryName -match "\\x64\\" } |
                          Sort-Object FullName -Descending
            if ($candidates) { $found = $candidates[0].FullName }
        }
    }
    # Fallback: System32 (always AMD64 on 64-bit Windows)
    if (-not $found) { $found = (Get-Command $dll -ErrorAction SilentlyContinue).Source }
    if ($found) {
        Copy-Item $found (Join-Path $stageLib $dll) -Force
    } else {
        Write-Warning "VC++ runtime $dll not found; skipping"
    }
}
Log "VC++ runtime DLLs -> lib/"

# ─── Flatten DLLs next to EXEs ─────────────────────────────────────────────────
# Windows resolves DLLs from the exe's own directory at startup (before main).
# lib/ is not on PATH unless Launcher.bat is used, so copy the critical DLLs
# next to the exe so double-click works directly.
Step "Copying DLLs next to EXEs (flat layout)"
$exeRoot = $STAGE
foreach ($dll in @(
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll",
    "Qt6Concurrent.dll", "Qt6OpenGL.dll", "Qt6OpenGLWidgets.dll",
    "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
    "vcruntime140.dll", "vcruntime140_1.dll"
)) {
    $src = Join-Path $stageLib $dll
    if (Test-Path $src) { Copy-Item $src (Join-Path $exeRoot $dll) -Force }
}
Log "Qt6 + VC runtime DLLs copied next to EXEs"

# qt.conf: Qt discovers plugins relative to Prefix.
# Prefix=. means <exe-dir>, so Plugins=lib/qt6/plugins resolves to
# the plugins tree already staged under lib/.
$qtConf = @"
[Paths]
Prefix = .
Plugins = lib/qt6/plugins
"@
$qtConf | Out-File -FilePath (Join-Path $STAGE "qt.conf") -Encoding ASCII -Force
Log "qt.conf written"

# ─── Stage layout ───────────────────────────────────────────────────────────────
Step "Assembling stage layout"

# assets from launcher build (merge into stage/assets, never nest)
$assetsSrc = Join-Path $LAUNCHER_BUILD "assets"
$assetsDst = Join-Path $STAGE "assets"
New-Item -ItemType Directory -Force -Path $assetsDst | Out-Null
if (Test-Path $assetsSrc) {
    Get-ChildItem $assetsSrc -Force | ForEach-Object {
        $target = Join-Path $assetsDst $_.Name
        if ($_.PSIsContainer) {
            New-Item -ItemType Directory -Force -Path $target | Out-Null
            Copy-Item "$($_.FullName)\*" $target -Recurse -Force
        } else {
            Copy-Item $_.FullName $target -Force
        }
    }
    Log "assets/ copied"
}

# Licences
Copy-Item (Join-Path $ROOT "LICENSE") (Join-Path $STAGE "LICENSE") -Force
$pgsLicense = Join-Path $PGS_DIR "COPYING.LGPLv3"
if (Test-Path $pgsLicense) { Copy-Item $pgsLicense (Join-Path $STAGE "COPYING.LGPLv3") -Force }

# Default settings.toml
$settingsDefault = Join-Path $ROOT "tools\release\settings.toml.default"
if (Test-Path $settingsDefault) {
    New-Item -ItemType Directory -Force -Path (Join-Path $STAGE "savedata") | Out-Null
    Copy-Item $settingsDefault (Join-Path $STAGE "savedata\settings.toml") -Force
}

# fps60 pacing table
$fps60 = Join-Path $GAME_DIR "fps60_sites.txt"
if (Test-Path $fps60) {
    New-Item -ItemType Directory -Force -Path (Join-Path $STAGE "savedata") | Out-Null
    Copy-Item $fps60 (Join-Path $STAGE "savedata\fps60_sites.txt") -Force
}

# Launcher.bat wrapper (mirrors entrypoint.sh)
$batContent = @"
@echo off
setlocal
set "HERE=%~dp0"
set "PATH=%HERE%lib;%PATH%"
set "QT_PLUGIN_PATH=%HERE%lib\qt6\plugins"
set "PS2X_EXEDIR=%HERE%"
start "" "%HERE%Launcher.exe" %*
endlocal
"@
$batContent | Out-File -FilePath (Join-Path $STAGE "Launcher.bat") -Encoding ASCII
Log "Launcher.bat written"

# ─── PE gate ────────────────────────────────────────────────────────────────────
Step "Running PE dependency gate"
$gateScript = Join-Path $ROOT "tools\release-windows\check_windows_deps.py"
python $gateScript $STAGE
if ($LASTEXITCODE -ne 0) { Fail "PE gate failed" }

# ─── Package ────────────────────────────────────────────────────────────────────
if (-not $SkipPackage) {
    Step "Packaging release zip"
    # Invoke packaging (PowerShell 5.1) to produce zip + sha256 and copy to Desktop
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ROOT "package-windows.ps1") -StageDir $STAGE -OutDir $OUT
}

$sw.Stop()
Step "Done"
Write-Host "  Stage:  $STAGE"
Write-Host "  Time:   $($sw.Elapsed.ToString('hh\:mm\:ss'))"
$zipPath = Join-Path $OUT "BT3-Recomp-x86_64.zip"
if (Test-Path $zipPath) {
    Write-Host ""
    Write-Host "  Deploy ready: $zipPath" -ForegroundColor Green
    $desktopPath = [Environment]::GetFolderPath("Desktop")
    if (-not $desktopPath) { $desktopPath = Join-Path $env:USERPROFILE "Desktop" }
    if (Test-Path (Join-Path $desktopPath "BT3-Recomp-x86_64.zip")) {
        Write-Host "  Desktop copy: $desktopPath\BT3-Recomp-x86_64.zip" -ForegroundColor Green
    }
}
