# SPDX-License-Identifier: GPL-2.0-only
#
# Installs the complete stack: the WinUSB package that claims the dongle's
# pixel interface, and the root-enumerated indirect display driver that
# presents it to Windows as a monitor. Both are required.
#
# Run through scripts\elev.ps1, or from an elevated prompt.

[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Get-KitTool([string]$name) {
    $roots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles(x86)}\Windows Kits\10\Tools",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    ) | Where-Object { Test-Path $_ }
    $found = @()
    foreach ($r in $roots) {
        $found += Get-ChildItem -Path $r -Recurse -Filter $name -ErrorAction SilentlyContinue
    }
    if (-not $found) { throw "could not find $name in the Windows Kits" }
    $hit = $found | Where-Object { $_.FullName -match '\\x64\\' } |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $hit) { $hit = $found | Sort-Object FullName -Descending | Select-Object -First 1 }
    return $hit.FullName
}

function Get-SigningCert {
    $subject = 'CN=winusbdisplay test signing'
    $cert = Get-ChildItem Cert:\LocalMachine\My |
        Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Subject $subject -Type CodeSigningCert `
            -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(5)
        foreach ($store in 'Root', 'TrustedPublisher') {
            $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
            $s.Open('ReadWrite'); $s.Add($cert); $s.Close()
        }
    }
    return $cert
}

function Publish-Package([string]$stage, [string]$infName) {
    # Inf2Cat needs the INF and everything it copies alone in one directory.
    & (Get-KitTool 'Inf2Cat.exe') /driver:$stage /os:10_X64,10_RS3_ARM64 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed for $infName" }

    $cert = Get-SigningCert
    $signtool = Get-KitTool 'signtool.exe'
    Get-ChildItem $stage -Include '*.cat', '*.dll' -Recurse | ForEach-Object {
        & $signtool sign /q /sm /fd sha256 /sha1 $cert.Thumbprint $_.FullName
        if ($LASTEXITCODE -ne 0) { throw "signtool failed on $($_.Name)" }
    }

    pnputil /add-driver (Join-Path $stage $infName) /install
    if ($LASTEXITCODE -ne 0) { throw "pnputil failed for $infName" }
}

Write-Host '=== 1. WinUSB package (claims the pixel interface) ==='
$stage1 = Join-Path $root 'build\package-winusb'
if (Test-Path $stage1) { Remove-Item $stage1 -Recurse -Force }
New-Item -ItemType Directory -Path $stage1 | Out-Null
Copy-Item (Join-Path $root 'inf\ms912x_winusb.inf') $stage1
Publish-Package $stage1 'ms912x_winusb.inf'

Write-Host ''
Write-Host '=== 2. indirect display driver (root enumerated) ==='
$binDir = Join-Path $root "build\driver\$Platform\$Configuration"
# Use the build-stamped INF: it carries a fresh DriverVer, without which
# pnputil reports "already exists" and keeps the previous binary.
$inf = Join-Path $binDir 'ms912xidd.inf'
if (-not (Test-Path $inf)) { $inf = Join-Path $root 'inf\ms912xidd.inf' }
Write-Host ("    " + ((Select-String -Path $inf -Pattern '^DriverVer').Line))

$stage2 = Join-Path $root 'build\package'
if (Test-Path $stage2) { Remove-Item $stage2 -Recurse -Force }
New-Item -ItemType Directory -Path $stage2 | Out-Null
Copy-Item (Join-Path $binDir 'ms912xidd.dll') $stage2
Copy-Item $inf (Join-Path $stage2 'ms912xidd.inf')
Publish-Package $stage2 'ms912xidd.inf'

Write-Host ''
Write-Host '=== 3. root device node ==='
$devcon = Get-KitTool 'devcon.exe'
$existing = Get-PnpDevice -InstanceId 'ROOT\DISPLAY\*' -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -match 'MacroSilicon' }
if ($existing) {
    Write-Host '    node already present, updating driver'
    & $devcon update (Join-Path $stage2 'ms912xidd.inf') 'root\ms912xidd'
} else {
    Write-Host '    creating root\ms912xidd'
    & $devcon install (Join-Path $stage2 'ms912xidd.inf') 'root\ms912xidd'
}

Start-Sleep -Seconds 3
pnputil /scan-devices | Out-Null

Write-Host ''
Write-Host '=== 4. brightness settings key ==='
# The driver runs as LOCAL SERVICE and cannot read a user hive, so the
# brightness value lives in HKLM. Widen that one key so the tray app can set
# it without elevation. Scope is deliberately narrow: one key, and the only
# values in it control picture settings.
$settingsKey = 'HKLM:\SOFTWARE\winusbdisplay'
if (-not (Test-Path $settingsKey)) {
    New-Item -Path $settingsKey -Force | Out-Null
}
foreach ($pair in @(@('Brightness', 100), @('Contrast', 50))) {
    if ($null -eq (Get-ItemProperty -Path $settingsKey -Name $pair[0] -ErrorAction SilentlyContinue)) {
        New-ItemProperty -Path $settingsKey -Name $pair[0] -Value $pair[1] -PropertyType DWord -Force | Out-Null
    }
}
$acl = Get-Acl $settingsKey
$users = New-Object System.Security.Principal.SecurityIdentifier(
    [System.Security.Principal.WellKnownSidType]::BuiltinUsersSid, $null)
$rule = New-Object System.Security.AccessControl.RegistryAccessRule(
    $users, 'SetValue,QueryValues,ReadKey', 'None', 'None', 'Allow')
$acl.SetAccessRule($rule)
Set-Acl -Path $settingsKey -AclObject $acl
Write-Host '    users may now set brightness without elevation'

Write-Host ''
Write-Host '=== 5. tray brightness control ==='
$tray = Join-Path $root 'build\msbright.exe'
if (Test-Path $tray) {
    Write-Host ("    " + $tray)
    Write-Host '    run it to get a tray icon; right-click it for Start with Windows'
} else {
    Write-Host '    not built yet, run scripts\build-tray.bat'
}

Write-Host ''
Write-Host '=== result ==='
Get-PnpDevice | Where-Object {
    $_.InstanceId -match 'ms912xidd' -or $_.InstanceId -match 'VID_345F.*MI_03'
} | ForEach-Object {
    $p = $_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_ProblemCode','DEVPKEY_Device_Service'
    Write-Host ("  " + $_.Status.ToString().PadRight(8) +
                " problem=" + ($p | Where-Object KeyName -eq 'DEVPKEY_Device_ProblemCode').Data +
                " svc=" + ($p | Where-Object KeyName -eq 'DEVPKEY_Device_Service').Data +
                "  " + $_.InstanceId)
}
