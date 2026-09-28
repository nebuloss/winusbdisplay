@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem
rem Recompiles the conversion shader and updates the committed copy.
rem
rem Run this after editing convert_cs.hlsl, and commit the result. The
rem compiled form is kept in the tree because the compiler that produces it
rem runs only on Windows, and requiring it would mean an emulator on every
rem other platform for one small artefact. See the comment at the top of
rem src\render\generated\convert_cs.h.

setlocal enabledelayedexpansion
set ROOT=%~dp0..
set SHADER=%ROOT%\src\render\convert_cs.hlsl
set OUTPUT=%ROOT%\src\render\generated\convert_cs.h

for /f "delims=" %%i in ('dir /b /o-n "%ProgramFiles(x86)%\Windows Kits\10\bin\10.*" 2^>nul') do (
  if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%i\x64\fxc.exe" (
    set FXC=%ProgramFiles(x86)%\Windows Kits\10\bin\%%i\x64\fxc.exe
    goto :found
  )
)
echo error: fxc.exe was not found. It ships with the Windows SDK.
exit /b 1

:found
echo using !FXC!

rem A temporary file first, so a failed compile cannot leave a half written
rem header behind for the next build to pick up.
"!FXC!" /nologo /T cs_5_0 /E main /Vn kConvertComputeShader ^
  /Fh "%TEMP%\convert_cs.h" "%SHADER%"
if errorlevel 1 (
  echo shader compilation failed
  exit /b 1
)

rem The explanatory header is kept and the bytecode replaced beneath it.
powershell -NoProfile -Command ^
  "$head = Get-Content '%OUTPUT%' -TotalCount 33; " ^
  "$body = Get-Content \"$env:TEMP\convert_cs.h\"; " ^
  "Set-Content '%OUTPUT%' ($head + $body)"

del "%TEMP%\convert_cs.h" 2>nul
echo updated %OUTPUT%
echo remember to commit it
endlocal
