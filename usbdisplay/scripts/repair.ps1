# SPDX-License-Identifier: GPL-2.0-only
#
# Restarts the display device, which is the whole of the cold boot fix.
#
# Windows fails to load this driver during boot, every single time, and the
# reason is structural rather than a bug in the driver. The device is root
# enumerated, so it has no parent hardware whose arrival could start it
# later; Plug and Play starts it during early boot device enumeration,
# before the user mode driver framework is running. A user mode driver
# cannot load that early, so the reflector fails with
# STATUS_FAILED_DRIVER_ENTRY and Plug and Play does not retry. The device
# sits in error for the rest of the session and there is no second monitor.
#
# Measured, six boots out of six, one to two seconds after each:
#
#     Driver \Driver\WUDFRd failed to load for the device ROOT\DISPLAY\0000.
#     Status: 0xC0000365
#
# It is also why every reinstall appeared to cure the problem: reinstalling
# re-enumerates the device, and by then the framework is up.
#
# install.ps1 registers this to run shortly after startup. It is safe to run
# by hand at any time; restarting a working display costs about four seconds
# of black screen.

$ErrorActionPreference = 'Stop'

$log = Join-Path $env:TEMP 'usbdisplay-repair.log'
function Note([string]$text) {
    $line = '{0}  {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $text
    Write-Output $line
    Add-Content -Path $log -Value $line -ErrorAction SilentlyContinue
}

Note ("repair starting, {0} s after this machine booted" -f
      [int]((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds)

$devices = @(Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'ROOT\DISPLAY*' -or
                   $_.InstanceId -like 'ROOT\USBDISPLAYDD*' })

if ($devices.Count -eq 0) {
    Note 'no display device is installed, nothing to repair'
    exit 1
}

foreach ($device in $devices) {
    Note ("found {0}, status {1}" -f $device.InstanceId, $device.Status)

    # Disable and enable rather than a restart: a device that failed to
    # start is not running, so there is nothing for a restart to cycle.
    #
    # Disable first and let it settle, and never remove. A live display
    # device removed with its monitor still in the desktop bug checked a
    # machine during development.
    try {
        Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false
        Start-Sleep -Seconds 2
        Enable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false
        Start-Sleep -Seconds 3
        $now = (Get-PnpDevice -InstanceId $device.InstanceId).Status
        Note ("restarted {0}, status now {1}" -f $device.InstanceId, $now)
    } catch {
        Note ("could not restart {0}: {1}" -f $device.InstanceId, $_.Exception.Message)
        exit 1
    }
}

Note 'repair done'
exit 0
