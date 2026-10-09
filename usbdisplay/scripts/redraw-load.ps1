# SPDX-License-Identifier: GPL-2.0-only
#
# Draws small text on the USB panel and repaints a small part of it on a
# timer, which is the workload the shimmer appears under.
#
# This exists so the two captures are comparable. The shimmer was always
# described as "File Explorer, scrolling", which is not a controlled load:
# the damage rectangles depend on what the compositor decides to report, and
# comparing our driver against the vendor's needs the same stimulus on both.
# Here the repaint region is a fixed 400x120 box of 9 point text, redrawn ten
# times a second, so what the drivers do with it is the only variable.
#
#   redraw-load.ps1 -Seconds 20
#
# Runs in the interactive session, not elevated: it has to put a window on
# the desktop.

param(
    [int]$Seconds = 20,
    [int]$HzTarget = 10
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$screens = [System.Windows.Forms.Screen]::AllScreens
$target = $screens | Where-Object { -not $_.Primary } | Select-Object -First 1
if (-not $target) {
    throw 'there is no second screen, so there is nothing to draw on'
}
Write-Host ("drawing on " + $target.DeviceName + " " + $target.Bounds)

$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.StartPosition = 'Manual'
$form.Left = $target.Bounds.Left + 100
$form.Top = $target.Bounds.Top + 100
$form.Width = 600
$form.Height = 300
$form.BackColor = [System.Drawing.Color]::White
$form.TopMost = $true

# 9 point, which is roughly what the shimmer is reported on, and a font with
# hinting rather than a smooth one, because that is where single-bit
# differences in conversion show up.
$font = New-Object System.Drawing.Font('Segoe UI', 9)
$brush = [System.Drawing.Brushes]::Black
$counter = 0

# A fixed box, so the damage rectangle the compositor reports is the same
# every time and the drivers are being compared on identical input.
$box = New-Object System.Drawing.Rectangle 20, 20, 400, 120

$form.Add_Paint({
    param($sender, $e)
    $e.Graphics.FillRectangle([System.Drawing.Brushes]::White, $box)
    for ($line = 0; $line -lt 7; $line++) {
        $text = ("frame {0,6}  line {1}  the quick brown fox jumps over it" -f $counter, $line)
        $e.Graphics.DrawString($text, $font, $brush, 22, (22 + $line * 16))
    }
})

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = [int](1000 / $HzTarget)
$timer.Add_Tick({
    $script:counter++
    $form.Invalidate($box)
})

$stop = New-Object System.Windows.Forms.Timer
$stop.Interval = $Seconds * 1000
$stop.Add_Tick({ $timer.Stop(); $stop.Stop(); $form.Close() })

$form.Show()
$form.Refresh()
$timer.Start()
$stop.Start()
Write-Host ("repainting a {0}x{1} box of 9 pt text {2} times a second for {3} s" -f
    $box.Width, $box.Height, $HzTarget, $Seconds)
[System.Windows.Forms.Application]::Run($form)
Write-Host ("done, " + $counter + " repaints")
