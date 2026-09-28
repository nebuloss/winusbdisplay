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
# Searched in every place the kit puts it. Looking in only one worked on the
# machine it was written on and silently skipped the removal everywhere else,
# leaving a device node behind while the script still reported success.
$devconRoots = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Tools",
    "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
    "${env:ProgramFiles}\Windows Kits\10\Tools",
    "${env:ProgramFiles}\Windows Kits\10\bin"
) | Where-Object { Test-Path $_ }

$devcon = $null
foreach ($r in $devconRoots) {
    $hit = Get-ChildItem $r -Recurse -Filter devcon.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
    if ($hit) { $devcon = $hit; break }
}

if ($devcon) {
    & $devcon.FullName remove 'root\usbhdmidd'
    & $devcon.FullName remove 'root\ms912xidd'
} else {
    Write-Output "  devcon not found in the Windows Kits."
    Write-Output "  The driver packages below will still be removed, but the"
    Write-Output "  device node will remain and reappear as a phantom monitor."
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
Write-Output "==> settings"
# Left behind otherwise, along with the widened permissions on it.
if (Test-Path 'HKLM:\SOFTWARE\usbhdmi') {
    Remove-Item 'HKLM:\SOFTWARE\usbhdmi' -Recurse -Force
    Write-Output "  removed HKLM\SOFTWARE\usbhdmi"
}

Write-Output ""
Write-Output "==> signing certificate"
# The installer creates a code signing certificate and trusts it machine
# wide. Leaving that behind after an uninstall is not acceptable: it is a
# trust root with years left on it that the user did not ask to keep.
$subject = 'CN=usbhdmi local signing'
foreach ($store in 'My', 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -eq $subject } | ForEach-Object {
            Write-Output "  removing from $store"
            Remove-Item $_.PSPath -Force -ErrorAction SilentlyContinue
        }
}

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
