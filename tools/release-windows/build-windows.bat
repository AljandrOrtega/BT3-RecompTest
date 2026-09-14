@echo off
setlocal EnableExtensions EnableDelayedExpansion
title BT3-Recomp - Windows release build
cd /d "%~dp0"

echo ============================================================
echo   BT3-Recomp - Windows release build (Docker)
echo ============================================================

rem ---- 1. bash (Git for Windows preferred, any bash on PATH as a backup) ----
set "BASH="
if exist "%ProgramFiles%\Git\bin\bash.exe"       set "BASH=%ProgramFiles%\Git\bin\bash.exe"
if not defined BASH if exist "%ProgramFiles(x86)%\Git\bin\bash.exe" set "BASH=%ProgramFiles(x86)%\Git\bin\bash.exe"
if not defined BASH (
    where bash >nul 2>&1
    if not errorlevel 1 (
        for /f "delims=" %%i in ('where bash') do set "BASH=%%i"
    )
)
if not defined BASH (
    echo ERROR: bash not found. Install "Git for Windows" and re-run:
    echo        https://gitforwindows.org/
    pause
    exit /b 1
)
echo   bash : %BASH%

rem ---- 2. Docker daemon (start Docker Desktop and wait up to 120s) ------
docker info >nul 2>&1
if not errorlevel 1 goto dockerok
echo   Docker is not running - starting Docker Desktop...
if exist "%ProgramFiles%\Docker\Docker\Docker Desktop.exe" (
    start "" "%ProgramFiles%\Docker\Docker\Docker Desktop.exe"
) else if exist "%ProgramFiles(x86)%\Docker\Docker\Docker Desktop.exe" (
    start "" "%ProgramFiles(x86)%\Docker\Docker\Docker Desktop.exe"
)
set /a WAIT=0
:waitdocker
timeout /t 5 /nobreak >nul
set /a WAIT+=5
docker info >nul 2>&1
if not errorlevel 1 goto dockerok
if %WAIT% GEQ 120 (
    echo   ERROR: Docker did not come up within 120s.
    echo          Launch "Docker Desktop" manually, wait for it run, then re-run.
    pause
    exit /b 1
)
goto waitdocker
:dockerok
echo   docker: OK

rem ---- 3. ISO (drag & drop the .iso onto this .bat to skip the prompt) ----
set "ISO="
if not "%~1"=="" set "ISO=%~f1"
if defined ISO set "ISO=%ISO:\=/%"

rem ---- 4. run the release build -------------------------------------------
if defined ISO (
    echo   iso  : "%ISO%"
    "%BASH%" "%~dp0build-windows.sh" --iso "%ISO%"
) else (
    "%BASH%" "%~dp0build-windows.sh"
)
set "RC=%errorlevel%"

rem ---- 5. package the zip on success -------------------------------------
if "%RC%"=="0" (
    echo.
    set /p PKG=Build OK. Package the release zip now? [y/N]:
    if /i "!PKG!"=="y" (
        "%BASH%" "%~dp0package.sh"
        set "RC=!errorlevel!"
    )
)

echo.
if "%RC%"=="0" (
    echo Done. Release artefacts in build\release-windows\out\
) else (
    echo Build failed ^(exit %RC%^). Scroll up for the error.
)
pause
exit /b %RC%