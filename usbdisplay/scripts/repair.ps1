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
# install.ps1 registers this to run shortly after startup, on two triggers,
# so it runs twice per session. It therefore has to decide whether a repair
# is actually needed, and the device's own status cannot answer that: it read
# OK at 46 s after a boot where the driver had definitively never loaded.
# What does answer it is the driver's log, because only the driver writes it.
# A log last written before this machine booted means the driver has not run
# this session, which is exactly the fault. Use -Force to restart regardless.

param([switch]$Force)

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

# Has the driver run at all since this machine booted?
#
# Deliberately not Get-PnpDevice's Status, which says OK for a device whose
# user mode driver failed to load, and so cannot tell the broken case from
# the working one. The log can: nothing but the driver writes it.
#
# This asks only whether the driver ever started this session, which is the
# cold boot fault and nothing else. It does not try to spot a driver that
# started and later stopped showing a picture. That is a different problem,
# and automatic recovery for it was implemented, measured and withdrawn
# because it blinked healthy displays; see docs/troubleshooting.md.
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$driverLog = Join-Path $env:WINDIR 'Temp\usbdisplaydd.log'
$ranThisSession = $false
$because = ''
try {
    if (Test-Path $driverLog) {
        $written = (Get-Item $driverLog).LastWriteTime
        if ($written -gt $boot) {
            $ranThisSession = $true
            $because = "driver log written at $written, after this machine booted at $boot"
        } else {
            $because = "driver log last written at $written, before this machine booted at $boot"
        }
    } else {
        $because = "no driver log at $driverLog"
    }
} catch {
    # Unreadable is not evidence of health, so repair rather than skip.
    $because = "could not read $driverLog ($($_.Exception.Message))"
}

if ($ranThisSession -and -not $Force) {
    Note "the driver has already run this session, so nothing is restarted: $because"
    Note 'repair done'
    exit 0
}

if ($Force) {
    Note 'restarting because -Force was given'
} else {
    Note "the driver has not run this session: $because"
}

# Where the log stands before anything is restarted. The check at the end has
# to see it move past this, not merely past the boot time: with -Force the
# driver has usually been running, so its log already postdates the boot and a
# check against that would pass without the driver having restarted at all.
$logBefore = $boot
try {
    if (Test-Path $driverLog) {
        $written = (Get-Item $driverLog).LastWriteTime
        if ($written -gt $logBefore) { $logBefore = $written }
    }
} catch { }

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

# Say whether the restart actually achieved anything, rather than reporting
# success because two Plug and Play calls returned. The status above is not
# the answer, for the same reason it was not the answer before: it reads OK
# either way. The driver writing its log is the answer.
#
# This matters for the earlier of the two triggers, which can fire before the
# user mode driver framework is ready. When it does, this says so, and the
# later trigger finds the driver still absent and tries again.
$started = $false
for ($i = 0; $i -lt 15; $i++) {
    try {
        if ((Test-Path $driverLog) -and
            (Get-Item $driverLog).LastWriteTime -gt $logBefore) {
            $started = $true
            break
        }
    } catch { }
    Start-Sleep -Seconds 1
}

if ($started) {
    Note 'the driver is running and writing its log'
    Note 'repair done'
    exit 0
}

Note 'the driver still has not written its log, so it did not start'
exit 1
