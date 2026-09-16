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
$MESA_DIR = Join-Path $BUILD "mesa"   # Mesa lavapipe (software Vulkan) for the Windows Vulkan fallback
$ISO_DEFAULT = ""
foreach ($isoDir in @([Environment]::GetFolderPath("Desktop"), (Join-Path $env:USERPROFILE "Downloads"), $ROOT, $PSScriptRoot)) {
    if ($isoDir -and (Test-Path $isoDir)) {
        $isoHit = Get-ChildItem $isoDir -Filter "*.iso" -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -match 'budokai|tenkaichi|dbz|bt3' } |
                  Select-Object -First 1
        if ($isoHit) { $ISO_DEFAULT = $isoHit.FullName; break }
    }
}
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

# Qt ships both release and debug DLLs in the same folder: the debug one is
# "<base>d.dll" and its release sibling "<base>.dll" sits next to it. A naive
# "*d.dll" match is WRONG — it also matches release plugins whose name
# legitimately ends in a "d" before ".dll" (qschannelbackend.dll,
# qopensslbackend.dll, qcertonlybackend.dll). Deleting those silently removes
# Qt's TLS backends and makes HTTPS downloads fail with "TLS initialization
# failed". Only treat a file as debug when the release counterpart exists.
function Test-QtDebugDll([System.IO.FileInfo]$File) {
    $n = $File.Name
    if ($n -notmatch 'd\.dll$') { return $false }
    $release = $n.Substring(0, $n.Length - 5) + '.dll'
    return (Test-Path (Join-Path $File.DirectoryName $release))
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
    # Single source of truth for the toolchain: VS Build Tools 2022 (ClangCL +
    # Win11 SDK), CMake, Ninja, Python, aqtinstall+pefile, Qt, and the bundled
    # Mesa lavapipe. Run with -Install so a fresh machine needs no manual setup;
    # it is a no-op when everything is already present.
    Step "Installing / verifying dependencies"
    $depsScript = Join-Path $ROOT "install-deps-windows.ps1"
    if (-not (Test-Path $depsScript)) { Fail "Dependency installer not found: $depsScript" }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $depsScript -Install
    if ($LASTEXITCODE -ne 0) { Fail "install-deps-windows.ps1 failed (exit $LASTEXITCODE)" }

    # Tools installed just now may not be on this session's PATH yet.
    $env:PATH = "$env:LOCALAPPDATA\Microsoft\WinGet\Links" + [System.IO.Path]::PathSeparator + $env:PATH
    foreach ($tool in @("cmake", "ninja", "python")) {
        if (-not (Test-FileCmd $tool)) {
            Fail "$tool not found after installing dependencies; open a new terminal and re-run build-windows.ps1"
        }
    }
}

# ─── ISO prompt ─────────────────────────────────────────────────────────────────
if (-not $SkipSetup) {
    if (-not $Iso) {
        if ($ISO_DEFAULT) {
            $readIso = Read-Host "BT3 ISO path (Enter = $ISO_DEFAULT)"
            $Iso = if ($readIso) { $readIso } else { $ISO_DEFAULT }
        } else {
            $Iso = Read-Host "BT3 ISO path (e.g. C:\path\to\bt3-usa.iso)"
        }
    }
    $Iso = "$Iso".Trim('"')
    if (-not $Iso -or -not (Test-Path $Iso)) { Fail "ISO not found: $Iso" }
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

# Qt6 DLLs (only release — debug DLLs require the debug MSVC runtime the gate rejects)
$qtBin = Join-Path $QT_ROOT "bin"
if (Test-Path $qtBin) {
    Get-ChildItem $qtBin -Filter "Qt6*.dll" |
        Where-Object { -not (Test-QtDebugDll $_) } |
        ForEach-Object {
            Copy-Item $_.FullName (Join-Path $stageLib $_.Name) -Force
        }
    Log "Qt6 release DLLs copied"
} else { Fail "Qt bin dir not found: $qtBin" }

# Qt plugins (release only; keep the release TLS backends)
$qtPluginSrc = Get-ChildItem $QT_ROOT -Directory -Filter "plugins" -ErrorAction SilentlyContinue |
               Select-Object -First 1
$qtPluginDst = Join-Path $stageLib "qt6\plugins"
New-Item -ItemType Directory -Force -Path $qtPluginDst | Out-Null
if ($qtPluginSrc) {
    # Copy the directory tree but exclude the debug plugin DLLs and the SQL
    # driver plugins (qsqlpsql.dll needs the non-system LIBPQ.dll; unused here).
    Copy-Item "$($qtPluginSrc.FullName)\*" $qtPluginDst -Recurse -Force
    Get-ChildItem $qtPluginDst -Recurse -Filter "*.dll" | Where-Object { Test-QtDebugDll $_ } | Remove-Item -Force
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

# lavapipe (Mesa software Vulkan): bundled so the launcher can run paraLLEl-GS on
# a software ICD when the vendor Vulkan driver is broken (e.g. the AMD proprietary
# driver access-violates inside amdvlk64.dll during shader compilation on Polaris).
# This is the Windows-only Vulkan fallback path.
$lvpSrc = Join-Path $MESA_DIR "x64"
if (Test-Path (Join-Path $lvpSrc "vulkan_lvp.dll")) {
    $lvpDst = Join-Path $STAGE "lavapipe"
    New-Item -ItemType Directory -Force -Path $lvpDst | Out-Null
    Copy-Item (Join-Path $lvpSrc "vulkan_lvp.dll") $lvpDst -Force
    Copy-Item (Join-Path $lvpSrc "lvp_icd.x86_64.json") $lvpDst -Force
    Log "lavapipe bundled (lavapipe/vulkan_lvp.dll)"
} else {
    Write-Warning ("lavapipe not found at " + $lvpSrc +
                   " -- run install-deps-windows.ps1 -Install; the Windows Vulkan fallback will be unavailable")
}

# Launcher.bat is no longer produced: the stage is a flat self-contained tree
# (Qt6 + VC runtime DLLs and qt.conf live next to the executables), so double
# click Launcher.exe directly — no wrapper needed.

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
    if ($LASTEXITCODE -ne 0) { Fail "Package step failed (exit $LASTEXITCODE)" }
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
