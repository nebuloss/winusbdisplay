@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Builds usbdisplayctl, the bring-up tool and test harness.

setlocal enabledelayedexpansion

set ROOT=%~dp0..
set OUT=%ROOT%\build

if "%VSCMD_ARG_TGT_ARCH%"=="" (
  set VCVARS=C:\Program Files ^(x86^)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
  if not exist "!VCVARS!" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
      set VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat
    )
  )
  if not exist "!VCVARS!" (
    echo error: could not locate vcvars64.bat
    exit /b 1
  )
  call "!VCVARS!" >nul
)

if not exist "%OUT%" mkdir "%OUT%"

cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE ^
  /Fo"%OUT%\\" /Fe"%OUT%\usbdisplayctl.exe" ^
  "%ROOT%\src\core\proto.cpp" ^
  "%ROOT%\src\core\usb.cpp" ^
  "%ROOT%\src\core\open_device.cpp" ^
  "%ROOT%\src\core\macrosilicon.cpp" ^
  "%ROOT%\src\render\rect.cpp" ^
  "%ROOT%\src\render\damage.cpp" ^
  "%ROOT%\src\render\convert.cpp" ^
  "%ROOT%\src\tools\usbdisplayctl\main.cpp" ^
  /link setupapi.lib hid.lib winusb.lib

if errorlevel 1 (
  echo build failed
  exit /b 1
)
echo built %OUT%\usbdisplayctl.exe
endlocal
