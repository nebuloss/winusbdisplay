# SPDX-License-Identifier: GPL-2.0-only
#
# Sets the brightness and contrast of the USB display.
#
# Windows does not route DDC/CI to indirect displays, which is why Twinkle
# Tray and similar tools report the monitor as unsupported: Microsoft's own
# DDI documentation states the OS never calls an indirect display driver's
# I2C callbacks. The driver implements the DDC/CI slave anyway, but it also
# watches a registry value so brightness can still be controlled. The value
# is applied to the picture during RGB to UYVY conversion, so it dims the
# panel for real rather than just being remembered.
#
#   scripts\brightness.ps1 -Brightness 60
#   scripts\brightness.ps1 -Brightness 100 -Contrast 50   (defaults)
#   scripts\brightness.ps1                                 (show current)

[CmdletBinding()]
param(
    [ValidateRange(0, 100)][int]$Brightness = -1,
    [ValidateRange(0, 100)][int]$Contrast = -1
)

$ErrorActionPreference = 'Stop'
$key = 'HKLM:\SOFTWARE\winusbdisplay'

if ($Brightness -lt 0 -and $Contrast -lt 0) {
    if (Test-Path $key) {
        $p = Get-ItemProperty $key
        Write-Output ("brightness: " + $(if ($null -ne $p.Brightness) { $p.Brightness } else { "100 (default)" }))
        Write-Output ("contrast:   " + $(if ($null -ne $p.Contrast) { $p.Contrast } else { "50 (default)" }))
    } else {
        Write-Output "brightness: 100 (default)"
        Write-Output "contrast:   50 (default)"
    }
    return
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Writing the setting needs an elevated prompt (or run via scripts\elev.ps1).'
}

if (-not (Test-Path $key)) {
    New-Item -Path $key -Force | Out-Null
}
if ($Brightness -ge 0) {
    Set-ItemProperty -Path $key -Name Brightness -Value $Brightness -Type DWord
    Write-Output "brightness -> $Brightness"
}
if ($Contrast -ge 0) {
    Set-ItemProperty -Path $key -Name Contrast -Value $Contrast -Type DWord
    Write-Output "contrast -> $Contrast"
}
Write-Output "(the driver polls twice a second; the panel updates shortly)"
