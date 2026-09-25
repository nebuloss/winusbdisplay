# SPDX-License-Identifier: GPL-2.0-only
#
# Captures IddCx's own ETW trace while the device restarts, so the class
# extension can explain in its own words why it rejected a call. The provider
# GUID is the one the in-box rdpidd.inf registers for IddCx.

$ErrorActionPreference = 'SilentlyContinue'

$session  = 'iddcxtrace'
$provider = '{D92BCB52-FA78-406F-A9A5-2037509FADEA}'
$etl      = 'C:\Windows\Temp\iddcx.etl'
$out      = 'C:\Windows\Temp\iddcx.txt'

logman stop $session -ets | Out-Null
Remove-Item $etl, $out -Force -ErrorAction SilentlyContinue

Write-Output "==> starting trace"
logman create trace $session -p $provider 0xffffffffffffffff 5 -o $etl -ets | Out-Null

Write-Output "==> restarting device"
$dev = Get-PnpDevice | Where-Object {
    $_.InstanceId -match 'ROOT\\DISPLAY' -and $_.FriendlyName -match 'MacroSilicon'
} | Select-Object -First 1
if ($dev) {
    Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
    Start-Sleep -Seconds 1
    Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
    Start-Sleep -Seconds 5
}

logman stop $session -ets | Out-Null

Write-Output "==> decoding"
tracerpt $etl -o $out -of CSV -y | Out-Null

if (Test-Path $out) {
    $lines = Get-Content $out
    Write-Output ("total events: " + $lines.Count)
    Write-Output ""
    # Surface anything that looks like a failure or mentions monitor/arrival.
    $lines | Where-Object {
        $_ -match 'rrival|onitor|ail|rror|0xC0|NOT_READY|Adapter'
    } | Select-Object -Last 40 | ForEach-Object {
        Write-Output ("  " + $_.Substring(0, [Math]::Min(400, $_.Length)))
    }
} else {
    Write-Output "no decoded output; tracerpt may need the manifest"
}
