@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Builds the indirect display driver. Needs the WDK extension for Visual
rem Studio, not just the SDK.
rem
rem   build-driver.bat [Release|Debug] [x64|ARM64]

setlocal enabledelayedexpansion

set ROOT=%~dp0..
set CONFIG=%1
set PLATFORM=%2
if "%CONFIG%"=="" set CONFIG=Release
if "%PLATFORM%"=="" set PLATFORM=x64

if "%VSCMD_ARG_TGT_ARCH%"=="" (
  set VCVARS=C:\Program Files ^(x86^)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
  if not exist "!VCVARS!" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
      set VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat
    )
  )
  call "!VCVARS!" >nul
)

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do (
  set MSBUILD=%%i\MSBuild\Current\Bin\MSBuild.exe
)
if not exist "%MSBUILD%" (
  echo error: could not locate MSBuild
  exit /b 1
)

rem SignMode Off because the catalog is produced and signed at install time,
rem once the INF and the binary have been staged into one directory.
"%MSBUILD%" "%ROOT%\src\driver\usbhdmidd.vcxproj" ^
  /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /p:SignMode=Off ^
  /v:minimal /nologo

if errorlevel 1 (
  echo build failed
  exit /b 1
)
echo built %ROOT%\build\driver\%PLATFORM%\%CONFIG%\usbhdmidd.dll
endlocal
