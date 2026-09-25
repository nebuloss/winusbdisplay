# SPDX-License-Identifier: GPL-2.0-only
#
# Stages, signs and installs the IddCx indirect display driver.
#
# Requires an elevated shell and test signing:
#     bcdedit /set testsigning on      (then reboot; Secure Boot must be off)
#
# This replaces the vendor driver on the dongle's display interface. Reverse
# with scripts\uninstall-driver.ps1.

[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [switch]$KeepVendorDriver,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

function Assert-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'This script must be run from an elevated PowerShell prompt.'
    }
}

function Get-KitTool([string]$name) {
    $roots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    ) | Where-Object { Test-Path $_ }

    foreach ($root in $roots) {
        $hit = Get-ChildItem -Path $root -Recurse -Filter $name -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw "could not find $name in the Windows Kits; install the WDK"
}

Assert-Elevated

$root = Split-Path -Parent $PSScriptRoot

if (-not $SkipBuild) {
    Write-Host '==> building driver'
    & (Join-Path $PSScriptRoot 'build-driver.bat') $Configuration $Platform
    if ($LASTEXITCODE -ne 0) { throw 'driver build failed' }
}

$binDir = Join-Path $root "build\driver\$Platform\$Configuration"
$dll = Join-Path $binDir 'ms912xidd.dll'
$inf = Join-Path $root 'inf\ms912xidd.inf'
foreach ($f in @($dll, $inf)) {
    if (-not (Test-Path $f)) { throw "missing $f" }
}

# Inf2Cat needs the INF and every file it copies in one directory.
$stage = Join-Path $root 'build\package'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item $dll $stage
Copy-Item $inf $stage
Write-Host "==> staged package in $stage"

$testSigning = (bcdedit /enum '{current}' | Select-String 'testsigning\s+Yes')
if (-not $testSigning) {
    Write-Warning 'Test signing does not appear to be enabled. Installation will likely fail.'
    Write-Warning 'Run: bcdedit /set testsigning on   then reboot (Secure Boot must be off).'
}

Write-Host '==> generating catalog'
$osTarget = if ($Platform -eq 'ARM64') { '10_ARM64' } else { '10_x64' }
& (Get-KitTool 'Inf2Cat.exe') /driver:$stage /os:$osTarget /verbose
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }

Write-Host '==> ensuring a test signing certificate exists'
$subject = 'CN=winusbdisplay test signing'
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $subject } |
    Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Subject $subject -Type CodeSigningCert `
        -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(5)
    foreach ($store in 'Root', 'TrustedPublisher') {
        $target = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
        $target.Open('ReadWrite')
        $target.Add($cert)
        $target.Close()
    }
}
Write-Host "    using $($cert.Thumbprint)"

Write-Host '==> signing catalog and driver binary'
$signtool = Get-KitTool 'signtool.exe'
foreach ($file in @((Join-Path $stage 'ms912xidd.cat'), (Join-Path $stage 'ms912xidd.dll'))) {
    & $signtool sign /v /fd sha256 /sha1 $cert.Thumbprint $file
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $file" }
}

Write-Host '==> installing driver package'
pnputil /add-driver (Join-Path $stage 'ms912xidd.inf') /install
if ($LASTEXITCODE -ne 0) { throw 'pnputil /add-driver failed' }

if (-not $KeepVendorDriver) {
    Write-Host '==> removing competing vendor driver packages'
    # Windows ranks a WHQL signed vendor package above a test signed one, so
    # the only reliable way to get our binding is to remove the competition.
    foreach ($block in ((pnputil /enum-drivers | Out-String) -split "`r?`n`r?`n")) {
        if ($block -match 'oem\d+\.inf' -and
            $block -match 'USBDisplay|MacroSilicon|msusbdisplay' -and
            $block -notmatch 'winusbdisplay') {
            $oem = [regex]::Match($block, 'oem\d+\.inf').Value
            Write-Host "    deleting $oem"
            pnputil /delete-driver $oem /uninstall /force
        }
    }
}

Write-Host '==> rescanning'
pnputil /scan-devices

Write-Host ''
Write-Host 'Done. An extra monitor should appear in Settings > System > Display.'
Write-Host 'If it does not, check Device Manager for the display interface and'
Write-Host 'see docs/troubleshooting.md.'
