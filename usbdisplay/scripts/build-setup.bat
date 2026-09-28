@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Builds the installer.

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
if not exist "%OUT%\setup" mkdir "%OUT%\setup"

rem The manifest is linked in, so Windows raises the permission prompt
rem itself rather than the program failing for want of rights.
rem MANIFESTUAC:NO stops the linker adding a second, conflicting one of its
rem own, which it does by default and which makes embedding fail outright.
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE ^
  /Fo"%OUT%\setup\\" /Fe"%OUT%\usbdisplay-setup.exe" ^
  "%ROOT%\src\tools\setup\main.cpp" ^
  /link setupapi.lib newdev.lib advapi32.lib crypt32.lib ^
  cfgmgr32.lib ole32.lib shell32.lib user32.lib ^
  /MANIFESTUAC:NO /MANIFEST:EMBED ^
  /MANIFESTINPUT:"%ROOT%\src\tools\setup\setup.manifest"

if errorlevel 1 (
  echo build failed
  exit /b 1
)
echo built %OUT%\usbdisplay-setup.exe
endlocal
