# SPDX-License-Identifier: GPL-2.0-only
#
# Unelevated client for the elevated worker. Starts the worker on first use
# (one UAC prompt for the whole session) and then submits commands to it
# without further prompting.
#
#   scripts\elev.ps1 -Start
#   scripts\elev.ps1 -Script C:\path\to\script.ps1 -Arguments '-Foo bar'
#   scripts\elev.ps1 -Command 'pnputil /enum-drivers'
#   scripts\elev.ps1 -Stop
#
# See elevated-worker.ps1 for the security tradeoff this makes.

param(
    [switch]$Start,
    [switch]$Stop,
    [switch]$Status,
    [string]$Script,
    [string]$Arguments = '',
    [string]$Command,
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$spool = Join-Path $root 'build\elev'
$alive = Join-Path $spool 'worker.alive'
$workerScript = Join-Path $PSScriptRoot 'elevated-worker.ps1'

function Test-WorkerAlive {
    if (-not (Test-Path $alive)) { return $false }
    try {
        $raw = (Get-Content $alive -Raw).Trim()
        $ticks, $workerPid = $raw -split '\|'
        $age = (Get-Date) - [datetime]::new([int64]$ticks)
        if ($age.TotalSeconds -gt 10) { return $false }
        return [bool](Get-Process -Id ([int]$workerPid) -ErrorAction SilentlyContinue)
    } catch { return $false }
}

function Start-Worker {
    if (Test-WorkerAlive) { return }
    if (-not (Test-Path $spool)) { New-Item -ItemType Directory -Path $spool -Force | Out-Null }
    Write-Host 'Starting elevated worker (this is the only UAC prompt)...'
    Start-Process -FilePath 'powershell.exe' `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $workerScript,
                        '-Spool', $spool) `
        -Verb RunAs -WindowStyle Hidden | Out-Null

    for ($i = 0; $i -lt 100; $i++) {
        if (Test-WorkerAlive) { Write-Host 'Elevated worker ready.'; return }
        Start-Sleep -Milliseconds 200
    }
    throw 'elevated worker did not come up'
}

function Invoke-Elevated([hashtable]$payload) {
    Start-Worker
    $id = [guid]::NewGuid().ToString('N')
    $reqTmp = Join-Path $spool "$id.tmp"
    $req    = Join-Path $spool "$id.req"
    ($payload | ConvertTo-Json -Compress) | Set-Content -Path $reqTmp -Encoding UTF8
    Move-Item $reqTmp $req   # atomic-ish: worker only ever sees a complete file

    $done = Join-Path $spool "$id.done"
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not (Test-Path $done)) {
        if ((Get-Date) -gt $deadline) { throw "elevated command timed out: $id" }
        Start-Sleep -Milliseconds 150
    }

    $outFile = Join-Path $spool "$id.out"
    if (Test-Path $outFile) { Get-Content $outFile }
    $codeFile = Join-Path $spool "$id.code"
    $code = 0
    if (Test-Path $codeFile) { $code = [int]((Get-Content $codeFile -Raw).Trim()) }

    Remove-Item $done, $outFile, $codeFile -Force -ErrorAction SilentlyContinue
    return $code
}

if ($Status) {
    Write-Output ("worker alive: " + (Test-WorkerAlive))
    return
}
if ($Start) { Start-Worker; return }
if ($Stop) {
    if (Test-WorkerAlive) { Invoke-Elevated @{ Kind = 'stop' } | Out-Null }
    Write-Host 'Elevated worker stopped.'
    return
}

if ($Script) {
    $argArray = @()
    if ($Arguments) {
        $argArray = [System.Management.Automation.PSParser]::Tokenize($Arguments, [ref]$null) |
            ForEach-Object { $_.Content }
    }
    $code = Invoke-Elevated @{ Kind = 'script'; Path = $Script; Args = $argArray }
    exit $code
}

if ($Command) {
    $code = Invoke-Elevated @{ Kind = 'command'; Command = $Command }
    exit $code
}

Write-Output 'nothing to do; pass -Start, -Stop, -Status, -Script or -Command'
