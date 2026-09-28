# SPDX-License-Identifier: GPL-2.0-only
#
# Brings the adapter back when Windows has stopped offering its pixel
# interface.
#
# That interface is where frames are sent, and it goes missing for two
# reasons: replacing the package that claims it detaches it, and so does
# anything that upsets the adapter's own enumeration. Once it is gone it does
# not return on its own, and the driver then reports, accurately but
# confusingly, that the package is not installed. It is; there is simply
# nothing for it to bind to.
#
# The order below matters. The display driver holds handles to that
# interface, so it is stopped first; the adapter is then re-enumerated, which
# is what makes Windows publish the interface again; and the driver is
# started last, once there is something for it to find.
#
# Nothing here removes a device. Removing a live display device crashed a
# machine during development, and there is no reason to do it to recover an
# interface. See docs/troubleshooting.md.

$ErrorActionPreference = 'Continue'

function Get-OurDisplayDevices {
    Get-PnpDevice -InstanceId 'ROOT\DISPLAY\*' -ErrorAction SilentlyContinue |
        Where-Object {
            $hardware = ($_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_HardwareIds' `
                -ErrorAction SilentlyContinue).Data
            $hardware -and ($hardware | Where-Object {
                $_ -in 'root\usbdisplaydd', 'root\usbhdmidd', 'root\ms912xidd' })
        }
}

function Get-AdapterInterfaces {
    Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match 'VID_345F|VID_534D' }
}

Write-Output '==> what is present now'
if (Get-AdapterInterfaces | Where-Object { $_.InstanceId -match 'MI_03' }) {
    Write-Output '    the pixel interface is present'
} else {
    Write-Output '    the pixel interface is MISSING, which is what this repairs'
}

Write-Output ''
Write-Output '==> stopping the display driver'
foreach ($device in Get-OurDisplayDevices) {
    Write-Output ("    " + $device.InstanceId)
    Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 3

Write-Output ''
Write-Output '==> re-enumerating the adapter'
# The composite parent creates one child device per USB interface, so
# restarting that is what makes a missing one reappear. Restarting the
# children alone does not: the parent has already decided what exists.
$parents = Get-AdapterInterfaces | Where-Object {
    $service = ($_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_Service' `
        -ErrorAction SilentlyContinue).Data
    $service -eq 'usbccgp'
}
foreach ($parent in $parents) {
    Write-Output ("    " + $parent.InstanceId)
    Disable-PnpDevice -InstanceId $parent.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    Enable-PnpDevice -InstanceId $parent.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}

pnputil /scan-devices | Out-Null
Start-Sleep -Seconds 2

Write-Output ''
Write-Output '==> did the interface come back?'
$pixels = Get-AdapterInterfaces | Where-Object { $_.InstanceId -match 'MI_03' }
if ($pixels) {
    $pixels | ForEach-Object {
        $service = ($_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_Service' `
            -ErrorAction SilentlyContinue).Data
        Write-Output ("    {0,-9} svc={1,-8} {2}" -f $_.Status, $service, $_.InstanceId)
    }
} else {
    Write-Output '    still missing.'
    Write-Output '    Unplug the adapter, wait a few seconds, and plug it back in.'
    Write-Output '    Some states are only cleared by the adapter losing power, and'
    Write-Output '    nothing Windows can be asked to do will substitute for that.'
}

Write-Output ''
Write-Output '==> starting the display driver'
# One only: a second would add a monitor nothing ever draws to.
$devices = @(Get-OurDisplayDevices | Where-Object {
    $hardware = ($_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_HardwareIds' `
        -ErrorAction SilentlyContinue).Data
    $hardware -contains 'root\usbdisplaydd'
})
if ($devices.Count -eq 0) {
    Write-Output '    no display device is installed; run install.ps1'
} else {
    Write-Output ("    " + $devices[0].InstanceId)
    Enable-PnpDevice -InstanceId $devices[0].InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    if ($devices.Count -gt 1) {
        Write-Output ("    leaving " + ($devices.Count - 1) + " duplicate(s) disabled;")
        Write-Output '    run install.ps1 to retire them properly'
    }
}

Start-Sleep -Seconds 5
Write-Output ''
Write-Output '==> result'
Get-OurDisplayDevices | ForEach-Object {
    Write-Output ("    {0,-9} {1}" -f $_.Status, $_.InstanceId)
}
Write-Output ''
Write-Output 'The driver records what it found in C:\Windows\Temp\usbdisplaydd.log.'
