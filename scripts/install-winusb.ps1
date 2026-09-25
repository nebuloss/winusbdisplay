# SPDX-License-Identifier: GPL-2.0-only
#
# Signs and installs the WinUSB INF, then displaces the vendor driver so that
# Windows binds ours to the display interface.
#
# Requires an elevated shell. It does NOT require test signing or a reboot:
# this package loads only WinUSB.sys, which is Microsoft signed and in-box, so
# kernel driver signature enforcement never comes into play. All that PnP
# needs is a catalog chaining to a certificate in the Trusted Publisher store,
# which this script creates. Works with Secure Boot enabled.
#
# Reverse with scripts\uninstall-winusb.ps1.

[CmdletBinding()]
param(
    [switch]$KeepVendorDriver
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

    $found = @()
    foreach ($root in $roots) {
        $found += Get-ChildItem -Path $root -Recurse -Filter $name -ErrorAction SilentlyContinue
    }
    if (-not $found) {
        throw "could not find $name in the Windows Kits; install the WDK"
    }
    # Prefer x64, but Inf2Cat.exe only ever ships as x86, so fall back to any
    # architecture rather than failing.
    $hit = $found | Where-Object { $_.FullName -match '\\x64\\' } |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $hit) {
        $hit = $found | Sort-Object FullName -Descending | Select-Object -First 1
    }
    return $hit.FullName
}

Assert-Elevated

$root    = Split-Path -Parent $PSScriptRoot
$infDir  = Join-Path $root 'inf'
$infPath = Join-Path $infDir 'ms912x_winusb.inf'
if (-not (Test-Path $infPath)) { throw "missing $infPath" }

$testSigning = (bcdedit /enum '{current}' | Select-String 'testsigning\s+Yes')
if ($testSigning) {
    Write-Host 'note: test signing is on. Not required, but harmless.'
}

Write-Host '==> generating catalog'
$inf2cat = Get-KitTool 'Inf2Cat.exe'
# Inf2Cat OS names are case sensitive and there is no plain "10_ARM64";
# ARM64 only exists from RS3 onwards. Run Inf2Cat /? for the full list.
& $inf2cat /driver:$infDir /os:10_X64,10_RS3_ARM64 /verbose
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }

Write-Host '==> ensuring a test signing certificate exists'
$subject = 'CN=winusbdisplay test signing'
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $subject } |
    Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Subject $subject -Type CodeSigningCert `
        -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(5)
    # Root makes the chain valid; TrustedPublisher is what lets PnP install the
    # package without prompting. Both are required, and both take effect
    # immediately, with no reboot.
    foreach ($store in 'Root', 'TrustedPublisher') {
        $target = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
        $target.Open('ReadWrite')
        $target.Add($cert)
        $target.Close()
    }
    Write-Host "    created and trusted $($cert.Thumbprint)"
} else {
    Write-Host "    reusing $($cert.Thumbprint)"
}

Write-Host '==> signing catalog'
$signtool = Get-KitTool 'signtool.exe'
& $signtool sign /v /fd sha256 /sha1 $cert.Thumbprint `
    (Join-Path $infDir 'ms912x_winusb.cat')
if ($LASTEXITCODE -ne 0) { throw 'signtool failed' }

Write-Host '==> installing driver package'
pnputil /add-driver $infPath /install
if ($LASTEXITCODE -ne 0) { throw 'pnputil /add-driver failed' }

if (-not $KeepVendorDriver) {
    Write-Host '==> removing vendor display driver packages'
    # Windows ranks the WHQL signed vendor package above ours, so the only
    # reliable way to get our binding is to remove the competing package.
    $vendor = pnputil /enum-drivers | Out-String
    $blocks = $vendor -split "`r?`n`r?`n"
    foreach ($block in $blocks) {
        if ($block -match 'oem\d+\.inf' -and $block -match 'USBDisplay|MacroSilicon|MS91') {
            $oem = [regex]::Match($block, 'oem\d+\.inf').Value
            if ($oem -and $block -notmatch 'winusbdisplay') {
                Write-Host "    deleting $oem"
                pnputil /delete-driver $oem /uninstall /force
            }
        }
    }
}

Write-Host '==> rescanning'
pnputil /scan-devices

Write-Host ''
Write-Host 'Done. Verify with: build\msdisp.exe dump'
