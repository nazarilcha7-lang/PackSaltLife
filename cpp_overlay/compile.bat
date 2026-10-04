@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul
title Compiling Coords Overlay
cd /d "%~dp0"

:: 1. MinGW g++ in PATH
where g++ >nul 2>nul
if %errorlevel% equ 0 goto build_mingw

:: 2. MSVC cl in PATH
where cl >nul 2>nul
if %errorlevel% equ 0 goto build_msvc

:: 3. Visual Studio via vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_DIR=%%i"
    if defined VS_DIR if exist "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat" (
        call "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat"
        goto build_msvc
    )
)

:: 4. MinGW in common folders
for %%D in (C:\w64devkit\bin C:\msys64\mingw64\bin C:\mingw64\bin) do (
    if exist "%%D\g++.exe" (
        set "PATH=%%D;!PATH!"
        goto build_mingw
    )
)

echo.
echo [ERROR] C++ compiler not found.
echo Install one of them:
echo   winget install skeeto.w64devkit
echo   or "Desktop development with C++" in Visual Studio Installer
echo.
pause
exit /b 1

:build_mingw
echo Compiling with MinGW (g++)...
windres MCOverlay.rc -O coff -o MCOverlay_res.o
if %errorlevel% neq 0 ( echo [ERROR] windres failed & pause & exit /b 1 )
g++ -std=c++17 -O2 main.cpp MCOverlay_res.o -o MCOverlay.exe -mwindows -static -static-libgcc -static-libstdc++ -ld2d1 -ldwrite -lgdi32 -luser32 -ladvapi32 -lwinmm
if %errorlevel% neq 0 ( echo [ERROR] Failed to compile MCOverlay.exe & pause & exit /b 1 )
g++ -O2 mock_client.cpp -o mock_client.exe -static
goto finish

:build_msvc
echo Compiling with MSVC (cl.exe)...
cl /nologo /EHsc /O2 /std:c++17 /utf-8 main.cpp /Fe:MCOverlay.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:NO user32.lib gdi32.lib d2d1.lib dwrite.lib winmm.lib
if %errorlevel% neq 0 ( echo [ERROR] Failed to compile MCOverlay.exe & pause & exit /b 1 )
cl /nologo /EHsc /O2 /utf-8 mock_client.cpp /Fe:mock_client.exe
del *.obj >nul 2>nul
goto finish

:finish
echo.
echo ====================================================
echo DONE: MCOverlay.exe, mock_client.exe
echo ====================================================
pause
