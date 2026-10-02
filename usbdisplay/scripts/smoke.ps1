# SPDX-License-Identifier: GPL-2.0-only
#
# Checks a built driver against real hardware, in two minutes, without
# anyone having to remember what to look at.
#
# Every test in tests\ runs without an adapter, which is what makes them
# fast and portable, and it also means not one of them would have caught
# any of the faults that actually shipped: a detector that read the wrong
# register on one chip, an adapter left dark while every counter said
# success, a pointer drawn as a black block. Those are only visible with
# hardware attached.
#
# This is the gap. It is not a substitute for looking at the panel, and it
# says so where it cannot tell.
#
#   usbdisplay\scripts\smoke.ps1              check what is installed
#   usbdisplay\scripts\smoke.ps1 -Install     install first, then check

[CmdletBinding()]
param([switch]$Install)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$tool = Join-Path $root 'build\usbdisplayctl.exe'
$log = 'C:\Windows\Temp\usbdisplaydd.log'

$failures = 0
$warnings = 0

function Pass([string]$what, [string]$detail) {
    '  ok    {0,-44} {1}' -f $what, $detail
}
function Fail([string]$what, [string]$detail) {
    $script:failures++
    '  FAIL  {0,-44} {1}' -f $what, $detail
}
function Warn([string]$what, [string]$detail) {
    $script:warnings++
    '  ?     {0,-44} {1}' -f $what, $detail
}

if (-not (Test-Path $tool)) {
    Write-Output "no tool at $tool, run scripts\build-tool.bat first"
    exit 1
}

if ($Install) {
    Write-Output '== installing'
    Remove-Item $log -ErrorAction SilentlyContinue
    & (Join-Path $PSScriptRoot 'install.ps1') 2>&1 |
        Select-String -Pattern 'failed|error' -CaseSensitive:$false |
        ForEach-Object { '  ' + $_ }
    Start-Sleep -Seconds 20
}

Write-Output ''
Write-Output '== the adapter'

$info = & $tool info 2>&1 | Out-String
if ($info -match 'chip:\s+(\S+)') {
    $chip = $Matches[1]
    Pass 'chip identified' $chip
} else {
    Fail 'chip identified' 'no answer, is a dongle plugged in'
    $chip = ''
}
if ($info -match 'connector:\s+(\S+)') { Pass 'connector' $Matches[1] }
if ($info -match 'memory:\s+(\d+ MB)') { Pass 'board memory' $Matches[1] }
if ($info -match 'display:\s+(\S+)') {
    if ($Matches[1] -eq 'connected') { Pass 'panel attached' '' }
    else { Warn 'panel attached' 'nothing on the HDMI socket' }
}

Write-Output ''
Write-Output '== the driver'

if (-not (Test-Path $log)) {
    Fail 'driver log' 'none, the driver has not run'
} else {
    # Joined into one string. Matching against an array returns only
    # whether any line matched, and leaves the captures from whatever
    # matched last, which is how this came to report the pointer as "1".
    $text = (Get-Content $log -ErrorAction SilentlyContinue) -join "`n"

    if ($text -match 'pipeline: running, mode (\S+)') {
        Pass 'pipeline running' $Matches[1]
    } else {
        Fail 'pipeline running' 'never reached'
    }

    $cost = $text | Select-String 'a full repaint costs' | Select-Object -Last 1
    if ($cost -match 'costs (\d+) period') {
        $periods = [int]$Matches[1]
        # The two families differ by a factor of eight here, and getting it
        # wrong is how the planner ends up making the display slower.
        $want = if ($chip -eq 'MS9132') { 1 } else { 8 }
        if ($periods -eq $want) {
            Pass 'cost model matches the chip' "$periods period(s)"
        } else {
            Fail 'cost model matches the chip' "$periods, expected $want for $chip"
        }
    }

    if ($text -match 'cursor: attached with ([^\r\n]+)') {
        Pass 'pointer' $Matches[1]
    } else {
        Fail 'pointer' 'never attached, there will be no mouse on the panel'
    }

    # The emulated inverting pointer comes back as a solid block, which is
    # the black rectangle around the text caret.
    if ($text -match 'cursor: attached with emulated') {
        Fail 'pointer format' 'emulated xor, which draws a black block'
    }

    $dark = ($text | Select-String 'stopped displaying').Count
    if ($dark -eq 0) {
        Pass 'no self inflicted reprogramming' ''
    } else {
        Warn 'no self inflicted reprogramming' "$dark recoveries, expected on a USB 3 part under load"
    }

    $last = $text | Select-String 'sent=\d+' | Select-Object -Last 1
    if ($last -match 'failed=(\d+)') {
        if ([int]$Matches[1] -eq 0) { Pass 'no failed transfers' '' }
        else { Fail 'no failed transfers' "$($Matches[1]) failed" }
    }
    if ($last -match 'sent=(\d+)') {
        if ([int]$Matches[1] -gt 0) { Pass 'frames reaching the adapter' $Matches[1] }
        else { Fail 'frames reaching the adapter' 'none' }
    }
}

Write-Output ''
Write-Output '== the panel'

# Only the USB 3 parts can answer this; see DisplayingPicture.
$health = & $tool health 2>&1 | Out-String
if ($health -match 'display:\s+showing') {
    if ($chip -eq 'MS9132') {
        Pass 'a picture is being transmitted' ''
    } else {
        Warn 'a picture is being transmitted' 'this part cannot report it'
    }
} elseif ($health -match 'display:\s+DARK') {
    Fail 'a picture is being transmitted' 'the adapter says it is dark'
}

Write-Output ''
Write-Output '-- what this cannot check --'
Write-Output '   Whether the picture is correct, whether the pointer looks'
Write-Output '   right over a pale background, and whether either flickers.'
Write-Output '   The register says what the chip is transmitting, not what'
Write-Output '   reaches the glass; those came apart once already. Look at'
Write-Output '   the screen.'

Write-Output ''
if ($failures -eq 0) {
    "passed, {0} thing(s) worth an eye" -f $warnings
    exit 0
}
"{0} failed, {1} worth an eye" -f $failures, $warnings
exit 1
