@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Builds the brightness path probe, a diagnostic rather than part of the
rem driver: it reports which brightness mechanisms each monitor answers.

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
  if not exist "!VCVARS!" ( echo error: no vcvars64.bat & exit /b 1 )
  call "!VCVARS!" >nul
)

rem Both directories, because the compiler will not create the one it
rem writes object files into and fails with a path error that names
rem the source file rather than the missing directory.
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\probe" mkdir "%OUT%\probe"
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE ^
  /Fo"%OUT%\probe\\" /Fe"%OUT%\brightnessprobe.exe" ^
  "%ROOT%\src\tools\brightnessprobe\main.cpp" ^
  /link user32.lib gdi32.lib dxva2.lib

if errorlevel 1 ( echo build failed & exit /b 1 )
echo built %OUT%\brightnessprobe.exe
endlocal
