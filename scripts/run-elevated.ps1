# SPDX-License-Identifier: GPL-2.0-only
#
# Runs another script elevated via UAC, with no visible window, capturing all
# output streams to a log file that the unelevated caller then prints.
#
# The elevated window is hidden on purpose: an interactive window here just
# shows a blank console while the script works, which looks like a hang.

param(
    [Parameter(Mandatory = $true)][string]$Script,
    [string]$LogFile,
    [string]$Arguments = ''
)

$ErrorActionPreference = 'Stop'

if (-not $LogFile) { $LogFile = Join-Path $PSScriptRoot 'elevated.log' }
if (Test-Path $LogFile) { Remove-Item $LogFile -Force }

# *> captures output, error, warning, verbose and information streams.
$command = "& '$Script' $Arguments *> '$LogFile'"

$p = Start-Process -FilePath 'powershell.exe' `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $command) `
    -Verb RunAs -WindowStyle Hidden -Wait -PassThru

if (Test-Path $LogFile) {
    Get-Content $LogFile
} else {
    Write-Output "(no output captured)"
}
Write-Output ("[elevated exit code: " + $p.ExitCode + "]")
