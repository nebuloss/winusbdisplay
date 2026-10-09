# SPDX-License-Identifier: GPL-2.0-only
#
# Reads the adapter's registers and prints them, so the chip's state can be
# compared between one driver and another.
#
# This is the way into the dark panel question that does not need a packet
# sniffer, and it works because of the one non-obvious thing about this
# hardware: **the control plane is HID on MI_00 and the pixel plane is bulk
# on MI_03, and they are different USB interfaces.** Whichever driver owns
# the pixel pipe, HID stays open to anyone, so the console tool can
# interrogate the chip while somebody else is driving it. That holds for the
# vendor's driver too, which takes MI_03 and leaves MI_00 alone.
#
# So: light the panel with one driver, dump this, light it with the other,
# dump this, and diff. Any register the working case sets and the dark case
# does not is a candidate for the whole fault.
#
#   chip-state.ps1 -Label vendor-lit
#
# Needs elevation, because the tool opens the HID device.

param(
    [string]$Label = 'state',
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\chipstate'),
    [string]$Tool = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\usbdisplayctl.exe')
)

$ErrorActionPreference = 'Continue'
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$report = Join-Path $OutDir "$Label.txt"

# Every register this project has a name for, plus the two windows that were
# read during mode programming. Addresses rather than names in the output, so
# a diff of two of these files is readable.
$registers = @(
    @{ A = '0x0030'; N = 'SdramType, board memory' }
    @{ A = '0x0031'; N = 'VideoPort, connector' }
    @{ A = '0x0032'; N = 'DisplayStatus, hot plug detect' }
    @{ A = '0x0033'; N = 'ModesetProbeA' }
    @{ A = '0xC620'; N = 'ModesetProbeB' }
    @{ A = '0xD003'; N = 'LiveImageIndex, which of the two buffers' }
    @{ A = '0xF000'; N = 'ChipId 912x window' }
    @{ A = '0xFF00'; N = 'ChipId 913x window' }
    @{ A = '0xFB1A'; N = 'DisplayLive: high nibble zero means dark' }
    @{ A = '0xF900'; N = 'PipeGuard: 0x9A healthy, 0x04 stuck' }
    @{ A = '0xF507'; N = 'HdmiMute 912x, bit 1' }
    @{ A = '0xFB07'; N = 'HdmiMute 913x, bit 1' }
)

$lines = @()
$lines += "chip state: $Label"
$lines += "taken at  : " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
$lines += ''

$lines += '--- what the tool makes of the link'
$lines += (& $Tool health 2>&1 | ForEach-Object { '  ' + $_ })
$lines += ''

$lines += '--- named registers'
foreach ($r in $registers) {
    $raw = (& $Tool peek $r.A 2>&1) -join ' '
    $lines += ('  {0}  {1,-44} {2}' -f $r.A, $r.N, $raw.Trim())
}
$lines += ''

# Blocks rather than single bytes, because a register that matters may sit
# next to one this project has never named, and a diff over a window finds it
# where a list of known names cannot.
$lines += '--- windows around everything interesting'
foreach ($w in @(
        @{ A = '0x0030'; C = 16 }, @{ A = '0xD000'; C = 16 },
        @{ A = '0xF900'; C = 16 }, @{ A = '0xFB00'; C = 32 },
        @{ A = '0xFF00'; C = 16 })) {
    $lines += ('  ' + $w.A + ' .. +' + $w.C)
    $lines += (& $Tool peek $w.A $w.C 2>&1 | ForEach-Object { '    ' + $_ })
}

$lines | Set-Content -Path $report -Encoding UTF8
$lines | ForEach-Object { Write-Output $_ }
Write-Output ''
Write-Output "written to $report"
