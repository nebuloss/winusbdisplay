@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Builds and runs the test suite. Everything here runs without hardware.

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

rem Both directories, because the compiler will not create the one it
rem writes object files into and fails with a path error that names
rem the source file rather than the missing directory.
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\tests" mkdir "%OUT%\tests"

cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE ^
  /Fo"%OUT%\tests\\" /Fe"%OUT%\usbdisplay-tests.exe" ^
  "%ROOT%\src\core\proto.cpp" ^
  "%ROOT%\src\core\usb.cpp" ^
  "%ROOT%\src\core\open_device.cpp" ^
  "%ROOT%\src\core\macrosilicon.cpp" ^
  "%ROOT%\src\render\rect.cpp" ^
  "%ROOT%\src\render\damage.cpp" ^
  "%ROOT%\src\render\convert.cpp" ^
  "%ROOT%\src\render\overlay.cpp" ^
  "%ROOT%\tests\testing.cpp" ^
  "%ROOT%\tests\test_rect.cpp" ^
  "%ROOT%\tests\test_damage.cpp" ^
  "%ROOT%\tests\test_convert.cpp" ^
  "%ROOT%\tests\test_chip.cpp" ^
  "%ROOT%\tests\test_overlay.cpp" ^
  "%ROOT%\tests\test_portable.cpp" ^
  /link setupapi.lib hid.lib winusb.lib

if errorlevel 1 (
  echo build failed
  exit /b 1
)

"%OUT%\usbdisplay-tests.exe" %1
endlocal
