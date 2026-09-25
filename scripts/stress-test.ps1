# SPDX-License-Identifier: GPL-2.0-only
#
# Drives changes on the USB display as fast as the compositor will present
# them, so the achieved update rate can be compared against the 60 per second
# the chip allows. The window animation test paces itself and therefore
# measures the test, not the driver.

param([int]$Seconds = 6)

$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$target = @([System.Windows.Forms.Screen]::AllScreens) |
    Where-Object { -not $_.Primary } | Select-Object -First 1
if (-not $target) { Write-Output 'no secondary screen'; exit 1 }

$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.StartPosition = 'Manual'
$form.Size = New-Object System.Drawing.Size(360, 280)
$form.Location = New-Object System.Drawing.Point(
    ($target.Bounds.X + 200), ($target.Bounds.Y + 200))
$form.TopMost = $true
$form.Show()

$deadline = (Get-Date).AddSeconds($Seconds)
$frames = 0
while ((Get-Date) -lt $deadline) {
    # Repaint with no artificial delay so the compositor is the only limit.
    $form.BackColor = [System.Drawing.Color]::FromArgb(
        (($frames * 17) % 255), (($frames * 31) % 255), (($frames * 53) % 255))
    $form.Refresh()
    [System.Windows.Forms.Application]::DoEvents()
    $frames++
}
$form.Close()

Write-Output ("generated $frames repaints in $Seconds s " +
              "({0:N0} per second offered)" -f ($frames / $Seconds))
