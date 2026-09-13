@echo off
setlocal enabledelayedexpansion

echo ======================================================================
echo   [CELS] Building Application DLL (Windows)
echo ======================================================================

set "SCRIPT_DIR=%~dp0"
set "ROOT_DIR=%SCRIPT_DIR%.."

:: 1. Determine build directory
set "BUILD_DIR="
if not "%~1"=="" (
    set "BUILD_DIR=%~1"
) else if exist "%ROOT_DIR%\cmake-build-debug\build.ninja" (
    set "BUILD_DIR=%ROOT_DIR%\cmake-build-debug"
) else if exist "%ROOT_DIR%\build\debug\windows" (
    set "BUILD_DIR=%ROOT_DIR%\cmake-build-debug"
) else (
    set "BUILD_DIR=%ROOT_DIR%\cmake-build-debug"
)

:: 2. Locate CMake executable
set "CMAKE_BIN=cmake"
where cmake >nul 2>&1
if %ERRORLEVEL% neq 0 (
    if exist "C:\Program Files\JetBrains\CLion 2026.2.1\bin\cmake\win\x64\bin\cmake.exe" (
        set "CMAKE_BIN=C:\Program Files\JetBrains\CLion 2026.2.1\bin\cmake\win\x64\bin\cmake.exe"
    ) else (
        for /d %%I in ("%LOCALAPPDATA%\Programs\CLion*") do (
            if exist "%%I\bin\cmake\win\x64\bin\cmake.exe" (
                set "CMAKE_BIN=%%I\bin\cmake\win\x64\bin\cmake.exe"
            )
        )
    )
)

echo [CELS] Using CMake : !CMAKE_BIN!
echo [CELS] Build Dir   : !BUILD_DIR!
echo [CELS] Target      : cel_app
echo.

"!CMAKE_BIN!" --build "!BUILD_DIR!" --target cel_app
set "BUILD_STATUS=%ERRORLEVEL%"

if %BUILD_STATUS% equ 0 (
    echo.
    echo ======================================================================
    echo   [CELS] Application DLL successfully updated!
    echo ======================================================================
) else (
    echo.
    echo [CELS] Error: Build failed with exit code %BUILD_STATUS%.
)

exit /b %BUILD_STATUS%
