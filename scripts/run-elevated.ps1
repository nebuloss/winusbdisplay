# Runs another script elevated via UAC and tees its output to a log file, so
# the non-elevated caller can read the result.
param(
    [Parameter(Mandatory = $true)][string]$Script,
    [string]$LogFile,
    [string[]]$Arguments = @()
)

$ErrorActionPreference = 'Stop'
if (-not $LogFile) { $LogFile = Join-Path $PSScriptRoot 'elevated.log' }
if (Test-Path $LogFile) { Remove-Item $LogFile -Force }

$inner = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command',
    ('& { try { & "' + $Script + '" ' + ($Arguments -join ' ') +
     ' *>&1 | Tee-Object -FilePath "' + $LogFile + '" } catch { $_ | Out-String | Tee-Object -FilePath "' +
     $LogFile + '" -Append } ; "EXITCODE=$LASTEXITCODE" | Out-File -Append "' + $LogFile + '" }')
)

$p = Start-Process -FilePath 'powershell.exe' -ArgumentList $inner -Verb RunAs -Wait -PassThru
Write-Output ("elevated process exit: " + $p.ExitCode)
if (Test-Path $LogFile) { Get-Content $LogFile }
