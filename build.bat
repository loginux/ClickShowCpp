@echo off
setlocal EnableExtensions EnableDelayedExpansion
pushd "%~dp0"

where cl >nul 2>nul
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
    if not defined VSINSTALL if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VSINSTALL=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools"
    if defined VSINSTALL if exist "!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat" call "!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

where cl >nul 2>nul
if errorlevel 1 (
    echo Could not find cl.exe. Run this from a Visual Studio Developer Command Prompt.
    popd
    exit /b 1
)

set "CMAKE_EXE=cmake"
where cmake >nul 2>nul
if errorlevel 1 (
    if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
)

rem A CMakeCache.txt stores absolute paths. If the repo was moved (or the
rem generator changed) the first configure fails; retry with a clean cache
rem instead of checking a single line and guessing.
"%CMAKE_EXE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release %*
if errorlevel 1 (
    echo Stale CMake cache detected, reconfiguring from scratch...
    rd /s /q build 2>nul
    "%CMAKE_EXE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release %*
)
if errorlevel 1 goto :fail
"%CMAKE_EXE%" --build build
if errorlevel 1 goto :fail

echo.
echo Built: %~dp0build\ClickShow.exe
popd
exit /b 0

:fail
echo.
echo Build failed.
popd
exit /b 1
