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
# Deliberately not Get-PnpDevice's Status as evidence of *health*, which
# says OK for a device whose user mode driver failed to load and so cannot
# tell the broken case from the working one. The log can: nothing but the
# driver writes it.
#
# This question alone is not enough, though, and shipping it alone was a
# mistake. It covers a driver that never loaded and nothing else, so a
# driver that loaded, ran for a minute and then died was read as "already
# run, nothing to do". See the error check below.
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

# Has Windows stopped the device because it reported a problem?
#
# The other half of the decision, and it was missing. The two directions of
# the devnode status are not symmetric, which is the point:
#
#   OK    proves nothing. It reads OK for a device whose user mode driver
#         never loaded, so it cannot be used to decide a repair is needless.
#   Error is Windows' own verdict that it has stopped the device. A working
#         display never reads that way, so it can be used to decide a repair
#         is needed.
#
# Measured on a cold boot: the driver loaded 17 s in, ran for a minute, then
# its transfers failed, the adapter dropped off the bus and the devnode went
# to CM_PROB_FAILED_POST_START. This script then ran twice and declined both
# times, because the driver had indeed "already run". The user had one screen
# for the rest of the session.
#
# This is not the automatic recovery that was withdrawn for blinking healthy
# displays. That was driven by a chip register describing what the adapter
# was transmitting, which has been wrong on a working panel. A Plug and Play
# problem code cannot be a false alarm.
#
# A device somebody switched off is excluded, and from the restart below as
# well. Being disabled is a problem code like any other, and turning it back
# on behind the user's back would overrule a deliberate choice, which a
# repair must never do.
$deliberate = 'CM_PROB_DISABLED', 'CM_PROB_HARDWARE_DISABLED', 'CM_PROB_DISABLED_SERVICE'
$switchedOff = @($devices | Where-Object { $deliberate -contains [string]$_.Problem })
foreach ($d in $switchedOff) {
    Note ("{0} is switched off, so it is left alone" -f $d.InstanceId)
}

# Only these are candidates for a restart.
$devices = @($devices | Where-Object { $deliberate -notcontains [string]$_.Problem })
if ($devices.Count -eq 0) {
    Note 'every display device is switched off, so there is nothing to repair'
    Note 'repair done'
    exit 0
}

$failed = @($devices | Where-Object { $_.Status -ne 'OK' })

if ($ranThisSession -and $failed.Count -eq 0 -and -not $Force) {
    Note "the driver has already run this session and the device is healthy, so nothing is restarted: $because"
    Note 'repair done'
    exit 0
}

if ($Force) {
    Note 'restarting because -Force was given'
} elseif ($failed.Count -gt 0) {
    foreach ($d in $failed) {
        Note ("the device has failed: {0} reports {1} / {2}" -f
              $d.InstanceId, $d.Status, $d.Problem)
    }
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
