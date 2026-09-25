# SPDX-License-Identifier: GPL-2.0-only
#
# Elevated worker. Started once (one UAC prompt) and then services commands
# submitted by the unelevated scripts\elev.ps1 client through a spool
# directory, so repeated administrative steps do not each prompt.
#
# SECURITY: anything that can write to the spool directory can run commands
# elevated. The spool is locked down to SYSTEM, Administrators and the user
# who started the worker. That still means any process running as that user
# can escalate, which is the same tradeoff as passwordless sudo. The worker
# exits automatically after an idle timeout, and scripts\elev.ps1 -Stop kills
# it immediately. Do not leave it running on a shared machine.

param(
    [Parameter(Mandatory = $true)][string]$Spool,
    [int]$IdleMinutes = 60
)

$ErrorActionPreference = 'Continue'

if (-not (Test-Path $Spool)) {
    New-Item -ItemType Directory -Path $Spool -Force | Out-Null
}

# Restrict the spool: no inheritance, only SYSTEM, Administrators and the
# invoking user.
try {
    $acl = Get-Acl $Spool
    $acl.SetAccessRuleProtection($true, $false)
    $acl.Access | ForEach-Object { $acl.RemoveAccessRule($_) | Out-Null }
    foreach ($id in @('NT AUTHORITY\SYSTEM', 'BUILTIN\Administrators', $env:USERNAME)) {
        try {
            $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
                $id, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
            $acl.AddAccessRule($rule)
        } catch { }
    }
    Set-Acl -Path $Spool -AclObject $acl
} catch {
    "warning: could not tighten spool ACL: $_" | Out-File (Join-Path $Spool 'worker.log') -Append
}

$alive = Join-Path $Spool 'worker.alive'
$lastWork = Get-Date

"worker started pid $PID at $(Get-Date -Format o)" |
    Out-File (Join-Path $Spool 'worker.log') -Append

while ($true) {
    # Heartbeat so the client can tell whether a worker is already running.
    Set-Content -Path $alive -Value ((Get-Date).Ticks.ToString() + "|" + $PID) -Force

    $req = Get-ChildItem -Path $Spool -Filter '*.req' -File -ErrorAction SilentlyContinue |
        Sort-Object CreationTimeUtc | Select-Object -First 1

    if (-not $req) {
        if (((Get-Date) - $lastWork).TotalMinutes -ge $IdleMinutes) {
            "worker idle timeout, exiting" | Out-File (Join-Path $Spool 'worker.log') -Append
            break
        }
        Start-Sleep -Milliseconds 250
        continue
    }

    $lastWork = Get-Date
    $id = $req.BaseName
    $outFile = Join-Path $Spool "$id.out"

    try {
        $payload = Get-Content $req.FullName -Raw | ConvertFrom-Json
    } catch {
        "bad request $id" | Out-File $outFile
        Set-Content (Join-Path $Spool "$id.code") '1'
        New-Item (Join-Path $Spool "$id.done") -ItemType File -Force | Out-Null
        Remove-Item $req.FullName -Force -ErrorAction SilentlyContinue
        continue
    }
    Remove-Item $req.FullName -Force -ErrorAction SilentlyContinue

    if ($payload.Kind -eq 'stop') {
        "worker stopping on request" | Out-File (Join-Path $Spool 'worker.log') -Append
        Set-Content (Join-Path $Spool "$id.code") '0'
        'stopped' | Out-File $outFile
        New-Item (Join-Path $Spool "$id.done") -ItemType File -Force | Out-Null
        break
    }

    $global:LASTEXITCODE = 0
    try {
        if ($payload.Kind -eq 'script') {
            $argList = @()
            if ($payload.Args) { $argList = [string[]]$payload.Args }
            & $payload.Path @argList *> $outFile
        } else {
            Invoke-Expression $payload.Command *> $outFile
        }
        $code = $LASTEXITCODE
        if ($null -eq $code) { $code = 0 }
    } catch {
        ($_ | Out-String) | Out-File $outFile -Append
        $code = 1
    }

    Set-Content (Join-Path $Spool "$id.code") ([string]$code)
    New-Item (Join-Path $Spool "$id.done") -ItemType File -Force | Out-Null
}

Remove-Item $alive -Force -ErrorAction SilentlyContinue
