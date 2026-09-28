# SPDX-License-Identifier: GPL-2.0-only
#
# Visual confirmation sweep: cycles solid colours then colour bars, pausing
# between each so a human can watch the panel. Use this to tell "our frames
# are landing" apart from "the panel is showing stale memory".

param(
    [string]$Mode = '1920x1080@60',
    [int]$PauseSeconds = 2
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$msdisp = Join-Path $root 'build\msdisp.exe'
if (-not (Test-Path $msdisp)) { throw "missing $msdisp; run scripts\build-tool.bat" }

Write-Host "==> modeset $Mode"
& $msdisp modeset --mode $Mode
if ($LASTEXITCODE -ne 0) { throw 'modeset failed' }

$steps = @(
    @{ Name = 'RED';    Args = @('--solid', '255,0,0') },
    @{ Name = 'GREEN';  Args = @('--solid', '0,255,0') },
    @{ Name = 'BLUE';   Args = @('--solid', '0,0,255') },
    @{ Name = 'WHITE';  Args = @('--solid', '255,255,255') },
    @{ Name = 'BLACK';  Args = @('--solid', '0,0,0') },
    @{ Name = 'COLOUR BARS'; Args = @('--bars') }
)

foreach ($step in $steps) {
    Write-Host ("--> " + $step.Name)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $msdisp testpattern --mode $Mode --no-modeset @($step.Args) | Out-Null
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) { throw ("failed on " + $step.Name) }
    Write-Host ("    sent in " + [math]::Round($sw.Elapsed.TotalMilliseconds) + " ms")
    Start-Sleep -Seconds $PauseSeconds
}

Write-Host ''
Write-Host 'Sequence complete with no transfer errors.'
Write-Host 'If the panel stayed black throughout, see docs/troubleshooting.md.'
