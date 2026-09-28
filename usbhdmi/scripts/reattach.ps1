# SPDX-License-Identifier: GPL-2.0-only
#
# Cycles the adapter's pixel interface and then the display device.
#
# Needed after the raw USB package is replaced. Windows detaches the
# interface the driver looks for and does not republish it until the device
# is restarted, so an install that reported success leaves the display driver
# reporting that the package is not installed. It is, it is just not attached
# yet.

$ErrorActionPreference = 'Continue'

Write-Output '==> cycling the pixel interface'
Get-PnpDevice | Where-Object {
    $_.InstanceId -match 'VID_345F.*MI_03' -or $_.InstanceId -match 'VID_534D'
} | ForEach-Object {
    Write-Output ("  " + $_.InstanceId)
    Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
}

Start-Sleep -Seconds 3
pnputil /scan-devices | Out-Null

Write-Output ''
Write-Output '==> restarting the display device'
Disable-PnpDevice -InstanceId 'ROOT\DISPLAY\0000' -Confirm:$false -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Enable-PnpDevice -InstanceId 'ROOT\DISPLAY\0000' -Confirm:$false -ErrorAction SilentlyContinue
Start-Sleep -Seconds 4

Write-Output ''
Write-Output '==> state'
Get-PnpDevice | Where-Object {
    $_.InstanceId -match 'VID_345F.*MI_03' -or $_.InstanceId -eq 'ROOT\DISPLAY\0000'
} | ForEach-Object {
    Write-Output ("  " + $_.Status.ToString().PadRight(8) + " " + $_.InstanceId)
}
