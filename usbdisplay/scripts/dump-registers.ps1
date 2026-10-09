# SPDX-License-Identifier: GPL-2.0-only
#
# Dumps large ranges of the adapter's register space to a file, so that the
# state one driver leaves can be diffed against another's.
#
# The point of a wide dump rather than a list of named registers: the project
# has names for about a dozen addresses, and the register that matters may not
# be one of them. It was not. Diffing two of these files is what found 0xFB00.
#
#   dump-registers.ps1 -Label vendor-lit
#
# Needs elevation. Reads over HID, so it works no matter which driver owns
# the pixel interface.

param(
    [string]$Label = 'dump',
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\chipstate'),
    [string]$Tool = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\usbdisplayctl.exe')
)

$ErrorActionPreference = 'Continue'
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$report = Join-Path $OutDir "$Label.dump.txt"

# The ranges worth having. Chosen from where this project has ever found
# anything, plus the whole of the 0xFBxx page, which is where the display
# status lives and therefore where its neighbours are most likely to matter.
$ranges = @(
    @{ Start = 0x0000; Count = 0x80 }
    @{ Start = 0xC600; Count = 0x40 }
    @{ Start = 0xD000; Count = 0x80 }
    @{ Start = 0xF500; Count = 0x40 }
    @{ Start = 0xF900; Count = 0x20 }
    @{ Start = 0xFB00; Count = 0x100 }
    @{ Start = 0xFF00; Count = 0x20 }
)

$lines = @()
$lines += "register dump: $Label"
$lines += "taken at     : " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
$lines += "live reading : " + (((& $Tool health 2>&1) | Where-Object { $_ -match 'display' }) -join ' ').Trim()
$lines += ''

foreach ($r in $ranges) {
    $lines += ('--- {0:X4} .. {1:X4}' -f $r.Start, ($r.Start + $r.Count - 1))
    # Sixteen bytes a line, because that is what the tool prints and what a
    # person reading a diff can locate.
    for ($off = 0; $off -lt $r.Count; $off += 16) {
        $addr = '0x{0:X4}' -f ($r.Start + $off)
        $n = [Math]::Min(16, $r.Count - $off)
        $out = @(& $Tool peek $addr $n 2>&1) | Where-Object { $_ -match '^[0-9A-Fa-f]{4}\s' }
        foreach ($l in $out) { $lines += ('  ' + $l.Trim()) }
    }
    $lines += ''
}

$lines | Set-Content -Path $report -Encoding UTF8
Write-Output ("written to " + $report + "  (" + $lines.Count + " lines)")
