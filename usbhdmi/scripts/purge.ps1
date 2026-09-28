# SPDX-License-Identifier: GPL-2.0-only
#
# Removes both packages and the device node, so the next install starts from
# nothing.
#
# Worth doing more often than seems necessary. A stale package that matches
# the adapter's USB ids keeps winning the binding for the pixel interface,
# and the symptom is an install that reports success while the adapter stays
# attached to the old driver. This also removes the frozen first driver's
# packages, since the two cannot coexist: they install the same kind of
# device node and compete for the same exclusive USB pipe.

$ErrorActionPreference = 'SilentlyContinue'

Write-Output "==> removing device nodes"
$devcon = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Tools" -Recurse -Filter devcon.exe |
    Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
if ($devcon) {
    & $devcon.FullName remove 'root\usbhdmidd'
    & $devcon.FullName remove 'root\ms912xidd'
} else {
    Write-Output "  devcon not found, skipping"
}

Write-Output ""
Write-Output "==> removing driver packages"
$all = (pnputil /enum-drivers | Out-String) -split "`r?`n`r?`n"
foreach ($block in $all) {
    if ($block -match 'usbhdmidd\.inf|usbhdmi_winusb\.inf|ms912xidd\.inf|ms912x_winusb\.inf') {
        $oem = [regex]::Match($block, 'oem\d+\.inf').Value
        if ($oem) {
            Write-Output "  deleting $oem"
            pnputil /delete-driver $oem /uninstall /force | Out-Null
        }
    }
}

Write-Output ""
Write-Output "==> rescanning"
pnputil /scan-devices | Out-Null
Start-Sleep -Seconds 2

Write-Output ""
Write-Output "==> what is left"
Get-PnpDevice | Where-Object {
    $_.InstanceId -match 'VID_345F' -or $_.InstanceId -match 'VID_534D' -or
    $_.FriendlyName -match 'MacroSilicon' -or $_.FriendlyName -match 'USB HDMI'
} | ForEach-Object {
    $p = $_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_Service','DEVPKEY_Device_DriverInfPath'
    Write-Output ("  " + $_.Status.ToString().PadRight(8) +
                  " svc=" + (($p | Where-Object KeyName -eq 'DEVPKEY_Device_Service').Data) +
                  " inf=" + (($p | Where-Object KeyName -eq 'DEVPKEY_Device_DriverInfPath').Data) +
                  "  " + $_.InstanceId)
}
