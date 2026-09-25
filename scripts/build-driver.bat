@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Builds the IddCx UMDF2 driver. Requires the WDK (Windows Driver Kit)
rem extension for Visual Studio, not just the SDK.

setlocal

set ROOT=%~dp0..
set MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe
if not exist "%MSBUILD%" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do (
    set "MSBUILD=%%i\MSBuild\Current\Bin\MSBuild.exe"
  )
)
if not exist "%MSBUILD%" (
  echo error: could not locate MSBuild.exe
  exit /b 1
)

rem The compute shader is compiled offline into a header, so the driver does
rem not need d3dcompiler at runtime. Only rebuilt when the source is newer.
set FXC=
for /f "usebackq tokens=*" %%i in (`dir /b /s "%ProgramFiles(x86)%\Windows Kits\10\bin\*\x64\fxc.exe" 2^>nul`) do set FXC=%%i
if defined FXC (
  "%FXC%" /nologo /T cs_5_0 /E main /O3 ^
    /Fh "%ROOT%\src\driver\convert_cs.h" /Vn kConvertComputeShader ^
    "%ROOT%\src\driver\convert_cs.hlsl" >nul
  if errorlevel 1 (
    echo shader compilation failed
    exit /b 1
  )
)

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set PLATFORM=%2
if "%PLATFORM%"=="" set PLATFORM=x64

"%MSBUILD%" "%ROOT%\src\driver\ms912xidd.vcxproj" ^
  /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% ^
  /p:SolutionDir=%ROOT%\ ^
  /p:SignMode=Off ^
  /nologo /verbosity:minimal

if errorlevel 1 (
  echo build failed
  exit /b 1
)
echo built %ROOT%\build\driver\%PLATFORM%\%CONFIG%\ms912xidd.dll
endlocal
