# SPDX-License-Identifier: GPL-2.0-only
#
# Removes the WinUSB driver package and lets Windows rebind the display
# interface to whatever else matches (the vendor driver, if still installed).

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This script must be run from an elevated PowerShell prompt.'
}

$drivers = pnputil /enum-drivers | Out-String
foreach ($block in ($drivers -split "`r?`n`r?`n")) {
    if ($block -match 'ms912x_winusb\.inf') {
        $oem = [regex]::Match($block, 'oem\d+\.inf').Value
        if ($oem) {
            Write-Host "deleting $oem"
            pnputil /delete-driver $oem /uninstall /force
        }
    }
}

pnputil /scan-devices
Write-Host 'Done.'
