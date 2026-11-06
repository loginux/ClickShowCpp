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
    echo Could not find cl.exe. Install Visual Studio Build Tools with the C++ workload,
    echo or run this from a Developer Command Prompt.
    popd
    exit /b 1
)

where rc >nul 2>nul
if errorlevel 1 (
    echo Could not find rc.exe. Install the Windows SDK.
    popd
    exit /b 1
)

if not exist "build_direct" mkdir "build_direct"

pushd "build_direct"

powershell -NoProfile -ExecutionPolicy Bypass -Command "$root='%~dp0'; $s=[IO.File]::ReadAllText($root+'res\app.rc.in'); $icon=($root+'res\clickshow.ico').Replace('\','/'); $manifest=($root+'res\app.manifest').Replace('\','/'); $s=$s.Replace('@CLICKSHOW_ICON@',$icon).Replace('@CLICKSHOW_MANIFEST@',$manifest); [IO.File]::WriteAllText($root+'build_direct\app.rc', $s, (New-Object System.Text.UTF8Encoding($false)))"
if errorlevel 1 (
    echo Failed to generate app.rc from res\app.rc.in.
    popd
    goto :fail
)

rc /nologo /I "..\res" /fo "app.res" "app.rc"
if errorlevel 1 (
    popd
    goto :fail
)

cl /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /permissive- ^
    /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 ^
    /I "..\src" /I "..\res" ^
    "..\src\main.cpp" "..\src\util.cpp" "..\src\json.cpp" "..\src\settings.cpp" "..\src\counts.cpp" "..\src\input.cpp" "..\src\overlay.cpp" ^
    app.res ^
    /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:ClickShow.exe ^
    user32.lib gdi32.lib gdiplus.lib shell32.lib advapi32.lib winhttp.lib comctl32.lib comdlg32.lib ole32.lib
if errorlevel 1 (
    popd
    goto :fail
)

popd
echo.
echo Built: %~dp0build_direct\ClickShow.exe
popd
exit /b 0

:fail
echo.
echo Build failed.
popd
exit /b 1
