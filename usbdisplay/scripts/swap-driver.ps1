# SPDX-License-Identifier: GPL-2.0-only
#
# Hands the adapter's pixel interface to the vendor's driver, or takes it
# back, so the two can be compared on the same hardware in the same session.
#
# Deliberately the smallest change that works, and reversible in one command,
# because this runs on a machine whose display is the thing being
# investigated:
#
#   - Our display device node is disabled, not removed. Removing a live
#     display device with its monitor still on the desktop bug checked a
#     machine during development; and a node that is merely disabled comes
#     back with everything it knew.
#   - Our driver packages are left in the driver store. They are not what
#     binds the hardware: the WinUSB package binding MI_03 is, and the vendor
#     package replaces it on that one device.
#   - Nothing touches MI_00, the HID control interface, which is how
#     chip-state.ps1 can interrogate the chip under either driver.
#
#   swap-driver.ps1 -To vendor
#   swap-driver.ps1 -To ours
#   swap-driver.ps1 -Status
#
# Needs elevation.

param(
    [ValidateSet('vendor', 'ours', 'status')]
    [string]$To = 'status',
    [switch]$Status,
    [string]$VendorInstaller = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) `
        'reference\MacroSilicon USBDisplay Driver Windows10_11 Installer_V4.2.8.11.exe')
)

$ErrorActionPreference = 'Continue'
if ($Status) { $To = 'status' }

$ourDisplay = 'ROOT\DISPLAY\0000'

function Our-WinUsbInf {
    # The published name of our WinUSB package, which is what has to be put
    # back on MI_03 afterwards. Found by asking the driver store rather than
    # assumed, because Windows renames every package it has ever been given.
    $lines = pnputil /enum-drivers
    $published = $null
    $result = @()
    foreach ($line in $lines) {
        if ($line -match '^\s*(?:Nom publi|Published Name)\S*\s*:\s*(\S+)') { $published = $Matches[1] }
        if ($line -match 'usbdisplay_winusb\.inf') { $result += $published }
    }
    return $result
}

function Show-State {
    $mi03 = @(Get-PnpDevice | Where-Object { $_.InstanceId -like '*PID_913?&MI_03*' -and $_.Present })
    foreach ($d in $mi03) {
        $inf = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
        $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
        $prov = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverProvider -ErrorAction SilentlyContinue).Data
        Write-Output ("  pixel interface : {0}" -f $d.InstanceId)
        Write-Output ("    driver        : {0}  service {1}  provider {2}" -f $inf, $svc, $prov)
        Write-Output ("    status        : {0}" -f $d.Status)
    }
    $ours = Get-PnpDevice -InstanceId $ourDisplay -ErrorAction SilentlyContinue
    if ($ours) {
        Write-Output ("  our device node : {0} / {1}" -f $ours.Status, $ours.Problem)
    }
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.Screen]::AllScreens | ForEach-Object {
        Write-Output ("  screen          : {0} {1} primary={2}" -f $_.DeviceName, $_.Bounds, $_.Primary)
    }
}

if ($To -eq 'status') {
    Write-Output 'current state'
    Show-State
    Write-Output ''
    Write-Output ('our winusb packages in the store: ' + ((Our-WinUsbInf) -join ', '))
    return
}

if ($To -eq 'vendor') {
    if (-not (Test-Path $VendorInstaller)) { throw "no vendor installer at $VendorInstaller" }

    Write-Output '1. stopping our driver so it releases the adapter'
    # Also stops the tray, which would otherwise keep writing brightness and
    # keep a handle on things while the packages move underneath it.
    Get-Process usbdisplaytray -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Output ('   stopping the tray, pid ' + $_.Id)
        $_.CloseMainWindow() | Out-Null
        Start-Sleep -Seconds 1
        if (-not $_.HasExited) { $_.Kill() }
    }
    Disable-PnpDevice -InstanceId $ourDisplay -Confirm:$false -ErrorAction Continue
    Start-Sleep -Seconds 4

    Write-Output '2. installing the vendor driver (its own installer, silently)'
    # The vendor installer is Inno Setup and binds the driver itself with
    # devcon. Running it rather than hand-placing the files is deliberate:
    # it is the path the vendor tests, it brings its own catalog, and it
    # leaves an uninstaller behind.
    $p = Start-Process -FilePath $VendorInstaller `
        -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' `
        -PassThru -Wait
    Write-Output ('   installer exit ' + $p.ExitCode)
    Start-Sleep -Seconds 6
    & pnputil /scan-devices | Out-Null
    Start-Sleep -Seconds 6

    Write-Output '3. where that left us'
    Show-State
    return
}

if ($To -eq 'ours') {
    Write-Output '1. removing the vendor driver'
    # Its uninstaller, by the registry entry it created. Falls back to
    # removing the package from the store, which is enough to free MI_03.
    $found = $false
    foreach ($root in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
                      'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall') {
        Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
            $v = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
            if ($v.DisplayName -match 'MacroSilicon') {
                Write-Output ('   ' + $v.DisplayName + ' -> ' + $v.UninstallString)
                $exe = $v.UninstallString.Trim('"')
                if (Test-Path $exe) {
                    Start-Process -FilePath $exe -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
                    $found = $true
                }
            }
        }
    }
    if (-not $found) { Write-Output '   no vendor uninstaller found' }

    $lines = pnputil /enum-drivers
    $published = $null
    foreach ($line in $lines) {
        if ($line -match '^\s*(?:Nom publi|Published Name)\S*\s*:\s*(\S+)') { $published = $Matches[1] }
        if ($line -match 'msUsbDisplayDriver\.inf') {
            Write-Output ('   removing package ' + $published)
            & pnputil /delete-driver $published /uninstall /force | Out-Null
        }
    }

    Write-Output '2. putting our WinUSB package back on the pixel interface'
    foreach ($inf in Our-WinUsbInf) {
        & pnputil /add-driver (Join-Path $env:WINDIR ("INF\" + $inf)) /install 2>&1 | Out-Null
    }
    & pnputil /scan-devices | Out-Null
    Start-Sleep -Seconds 5

    Write-Output '3. starting our driver again'
    Enable-PnpDevice -InstanceId $ourDisplay -Confirm:$false -ErrorAction Continue
    Start-Sleep -Seconds 8

    $tray = 'C:\Program Files\usbdisplay\usbdisplaytray.exe'
    if (Test-Path $tray) { Start-Process -FilePath $tray | Out-Null }

    Write-Output '4. where that left us'
    Show-State
    return
}
