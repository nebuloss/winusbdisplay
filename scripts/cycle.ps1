# Reinstall the driver package and report the resulting state. Intended to be
# run through scripts\elev.ps1 for fast iteration.
$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot 'install-driver.ps1') -SkipBuild 2>&1 |
    Select-String -Pattern 'DriverVer|Nom publi|Published|install|jour|error' |
    ForEach-Object { Write-Output ("  " + $_.Line.Trim()) }

& (Join-Path $PSScriptRoot 'diagnose-driver.ps1')

Write-Output ""
Write-Output "==> filters actually applied"
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_345F&PID_9133&MI_03\6&29884711&0&0003"
$p = Get-ItemProperty -Path $k
Write-Output ("  UpperFilters = " + ($p.UpperFilters -join ','))
Write-Output ("  LowerFilters = " + ($p.LowerFilters -join ','))
