@echo off
setlocal

set "SOLUTION=%~dp0build-vs2026\rme.slnx"

if not exist "%SOLUTION%" (
    echo ERROR: The Visual Studio solution has not been generated yet.
    echo Open the repository folder in Visual Studio or configure the vs2026-vcpkg CMake preset first.
    pause
    exit /b 1
)

start "" "%SOLUTION%"
endlocal
