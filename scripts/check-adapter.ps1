$ErrorActionPreference = 'SilentlyContinue'

Write-Output "=== our device's Display class instance ==="
$classRoot = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
Get-ChildItem $classRoot | ForEach-Object {
    $p = Get-ItemProperty $_.PSPath
    if ($p.MatchingDeviceId -match 'ms912xidd' -or $p.DriverDesc -match 'MacroSilicon') {
        Write-Output ("  key         = " + $_.PSChildName)
        foreach ($n in 'DriverDesc','MatchingDeviceId','InfPath','VideoID','AdapterDACType','HardwareInformation.AdapterString') {
            if ($null -ne $p.$n) { Write-Output ("  " + $n.PadRight(18) + " = " + ($p.$n -join ',')) }
        }
        $vid = $p.VideoID
        if ($vid) {
            Write-Output ("  -> Control\Video\" + $vid + " exists: " +
                (Test-Path ("HKLM:\SYSTEM\CurrentControlSet\Control\Video\" + $vid)))
        } else {
            Write-Output "  -> no VideoID: the OS never created a display adapter object"
        }
    }
}

Write-Output ""
Write-Output "=== IndirectKmd attached to our devnode? ==="
$inst = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY\0000'
$p = Get-ItemProperty $inst
Write-Output ("  UpperFilters = " + ($p.UpperFilters -join ','))
Write-Output ("  Driver       = " + $p.Driver)
Write-Output ("  Service      = " + $p.Service)

Write-Output ""
Write-Output "=== all display adapters the graphics stack knows ==="
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Video' | ForEach-Object {
    $sub = Get-ItemProperty ($_.PSPath + '\Video')
    $desc = (Get-ItemProperty ($_.PSPath + '\0000')).'HardwareInformation.AdapterString'
    if (-not $desc) { $desc = (Get-ItemProperty ($_.PSPath + '\0000')).'Device Description' }
    Write-Output ("  " + $_.PSChildName + "  " + $desc)
}

Write-Output ""
Write-Output "=== QueryDisplayConfig target count ==="
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Ccd {
  [DllImport("user32.dll")]
  public static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPath, out uint numMode);
}
"@
$np = 0; $nm = 0
$r = [Ccd]::GetDisplayConfigBufferSizes(2, [ref]$np, [ref]$nm)   # QDC_ALL_PATHS
Write-Output ("  result=" + $r + " paths=" + $np + " modes=" + $nm)
