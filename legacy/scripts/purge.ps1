# SPDX-License-Identifier: GPL-2.0-only
#
# Removes every winusbdisplay driver package and the root device node, so a
# subsequent install starts from a clean slate.
#
# This matters because earlier revisions of ms912xidd.inf matched the dongle's
# USB hardware ids directly. Those stale packages keep winning the binding for
# the pixel interface and have to go before the WinUSB package can claim it.

$ErrorActionPreference = 'SilentlyContinue'

Write-Output "==> removing root device node"
$devcon = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Tools" -Recurse -Filter devcon.exe |
    Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
if ($devcon) {
    & $devcon.FullName remove 'root\ms912xidd'
} else {
    Write-Output "  devcon not found, skipping"
}

Write-Output ""
Write-Output "==> removing driver packages"
$all = (pnputil /enum-drivers | Out-String) -split "`r?`n`r?`n"
foreach ($block in $all) {
    if ($block -match 'ms912xidd\.inf|ms912x_winusb\.inf') {
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
Write-Output "==> remaining state"
Get-PnpDevice | Where-Object {
    $_.InstanceId -match 'VID_345F' -or $_.FriendlyName -match 'MacroSilicon'
} | ForEach-Object {
    $p = $_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_Service','DEVPKEY_Device_DriverInfPath'
    Write-Output ("  " + $_.Status.ToString().PadRight(8) +
                  " svc=" + (($p | Where-Object KeyName -eq 'DEVPKEY_Device_Service').Data) +
                  " inf=" + (($p | Where-Object KeyName -eq 'DEVPKEY_Device_DriverInfPath').Data) +
                  "  " + $_.InstanceId)
}
