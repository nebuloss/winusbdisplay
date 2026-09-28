$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$screens = @([System.Windows.Forms.Screen]::AllScreens)
$target = $screens | Where-Object { -not $_.Primary } | Select-Object -First 1
if (-not $target) { Write-Output "no secondary screen"; exit 1 }

Write-Output ("animating on " + $target.DeviceName + " at " + $target.Bounds.X)

$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.StartPosition = 'Manual'
$form.Size = New-Object System.Drawing.Size(500, 400)
$form.TopMost = $true
$form.BackColor = [System.Drawing.Color]::DarkSlateBlue
$form.Show()

for ($i = 0; $i -lt 40; $i++) {
    $x = $target.Bounds.X + 100 + (($i * 30) % 1200)
    $y = $target.Bounds.Y + 100 + [int](200 * [Math]::Sin($i / 4.0))
    $form.Location = New-Object System.Drawing.Point($x, $y)
    $form.BackColor = [System.Drawing.Color]::FromArgb(
        (($i * 13) % 255), (($i * 29) % 255), (($i * 47) % 255))
    $form.Refresh()
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 120
}

$form.Close()
Write-Output "done"
