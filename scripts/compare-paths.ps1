# SPDX-License-Identifier: GPL-2.0-only
#
# Compares the CPU and GPU conversion paths under identical load: the same
# animation is driven on the USB display with each path, and the CPU time
# consumed by the driver host process is measured.
#
# Wall clock per frame is only half the story. The GPU path can be no faster
# end to end yet still be the better choice if it leaves the processor free.

param([int]$Seconds = 12)

$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$key = 'HKLM:\SOFTWARE\winusbdisplay'

function Get-HostProcess {
    # The driver runs in whichever WUDFHost has our DLL loaded.
    Get-Process WUDFHost | Where-Object {
        $_.Modules.ModuleName -contains 'ms912xidd.dll'
    } | Select-Object -First 1
}

function Measure-Path([string]$name, [int]$useCompute) {
    Set-ItemProperty -Path $key -Name UseComputeShader -Value $useCompute -Type DWord

    # Restart the device so the new policy takes effect.
    Disable-PnpDevice -InstanceId 'ROOT\DISPLAY\0000' -Confirm:$false
    Start-Sleep -Seconds 2
    Enable-PnpDevice -InstanceId 'ROOT\DISPLAY\0000' -Confirm:$false
    Start-Sleep -Seconds 5

    & (Join-Path $PSScriptRoot 'extend-desktop.ps1') | Out-Null
    Start-Sleep -Seconds 2

    $proc = Get-HostProcess
    if (-not $proc) {
        Write-Output "  $name : driver host not found"
        return
    }

    $before = $proc.TotalProcessorTime
    $start = Get-Date
    & (Join-Path $PSScriptRoot 'animate-test.ps1') -PauseSeconds 0 | Out-Null
    $elapsed = ((Get-Date) - $start).TotalSeconds

    $proc.Refresh()
    $cpu = ($proc.TotalProcessorTime - $before).TotalSeconds

    Write-Output ("  {0,-4} : {1,6:N2} s CPU over {2,5:N1} s wall  ({3,5:N1}% of one core)" -f `
        $name, $cpu, $elapsed, (100 * $cpu / $elapsed))
}

Write-Output "comparing conversion paths"
Measure-Path 'cpu' 0
Measure-Path 'gpu' 1

# Leave the GPU path enabled, it is the default.
Set-ItemProperty -Path $key -Name UseComputeShader -Value 1 -Type DWord
