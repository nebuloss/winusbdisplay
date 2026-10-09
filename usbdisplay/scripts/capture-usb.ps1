# SPDX-License-Identifier: GPL-2.0-only
#
# Captures the USB traffic to the display adapter with the tracing that is
# already built into Windows, and summarises it by endpoint and transfer size.
#
# There is no packet sniffer on this machine and installing one means a kernel
# filter driver, which is a poor thing to add to a machine whose display
# driver is the subject of the investigation. The xHCI driver's own tracing
# gives what this question needs: the endpoint, the direction and the length
# of every transfer, with timestamps. The payload is not needed. What is being
# asked is whether the vendor's driver sends small regions or whole frames
# when small text redraws, and how many times it sends each one.
#
#   capture-usb.ps1 -Seconds 20 -Label vendor
#
# Must run elevated. Leaves the .etl and a .csv beside it in build\usbtrace.

param(
    [int]$Seconds = 20,
    [string]$Label = 'capture',
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\usbtrace')
)

$ErrorActionPreference = 'Continue'

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$session = 'usbdisplaycapture'
$etl = Join-Path $OutDir "$Label.etl"
$csv = Join-Path $OutDir "$Label.csv"

# A stale session survives a crashed run and makes the next one fail with a
# message that does not say so.
& logman stop $session -ets 2>&1 | Out-Null
foreach ($f in $etl, $csv) { if (Test-Path $f) { Remove-Item $f -Force } }

# Both controllers' providers, because which one the adapter landed on depends
# on the port it is in, and a capture from the wrong one is silently empty.
# Keyword and level are wide open: narrowing them loses the transfer events.
& logman create trace $session -ets `
    -p '{30E1D284-5D88-459C-83FD-6345B39B19EC}' 0xffffffffffffffff 0xff `
    -o $etl -nb 64 256 -bs 1024 -mode Circular -max 512 2>&1 | Out-Null
& logman update trace $session -ets `
    -p '{AC52AD17-CC01-4F85-8DF5-4DCE4333C99B}' 0xffffffffffffffff 0xff 2>&1 | Out-Null
& logman update trace $session -ets `
    -p '{C88A4EF5-D048-4013-9408-E04B7DB2814A}' 0xffffffffffffffff 0xff 2>&1 | Out-Null

$state = & logman query $session -ets 2>&1
Write-Host ("session: " + (($state | Select-String -Pattern 'Status|�tat' | Select-Object -First 1).Line))

Write-Host "capturing for $Seconds s -- make small text redraw on the USB panel now"
Start-Sleep -Seconds $Seconds
& logman stop $session -ets 2>&1 | Out-Null

if (-not (Test-Path $etl)) { throw "no trace was written to $etl" }
Write-Host ("captured {0:N0} bytes" -f (Get-Item $etl).Length)

# tracerpt writes a dump and a summary; the dump is what carries the fields.
Push-Location $OutDir
& tracerpt $etl -o (Join-Path $OutDir "$Label.xml") -summary (Join-Path $OutDir "$Label.summary.txt") -f XML -y 2>&1 |
    Select-Object -Last 3 | ForEach-Object { Write-Host ("  " + $_) }
Pop-Location

if (Test-Path (Join-Path $OutDir "$Label.summary.txt")) {
    Write-Host ''
    Write-Host '=== event counts by provider and id ==='
    Get-Content (Join-Path $OutDir "$Label.summary.txt") |
        Select-Object -First 60 | ForEach-Object { Write-Host ("  " + $_) }
}
