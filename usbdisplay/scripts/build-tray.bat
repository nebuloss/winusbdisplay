@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Builds the notification-area brightness control.

setlocal enabledelayedexpansion
set ROOT=%~dp0..
set OUT=%ROOT%\build

if "%VSCMD_ARG_TGT_ARCH%"=="" (
  set VCVARS=C:\Program Files ^(x86^)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
  if not exist "!VCVARS!" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do (
      set VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat
    )
  )
  if not exist "!VCVARS!" (
    echo error: could not locate vcvars64.bat.
    echo Visual Studio 2022 Build Tools with the C++ workload is required.
    exit /b 1
  )
  call "!VCVARS!" >nul
)

if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE ^
  /Fo"%OUT%\tray\\" /Fe"%OUT%\usbdisplaytray.exe" ^
  "%ROOT%\src\tools\usbdisplaytray\control.cpp" ^
  "%ROOT%\src\tools\usbdisplaytray\main.cpp" ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib shell32.lib ^
  dxva2.lib advapi32.lib dwmapi.lib shcore.lib

if errorlevel 1 ( echo build failed & exit /b 1 )
echo built %OUT%\usbdisplaytray.exe
endlocal
