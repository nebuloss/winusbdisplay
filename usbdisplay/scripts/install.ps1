# SPDX-License-Identifier: GPL-2.0-only
#
# Installs both packages: the one that claims the adapter's pixel interface
# for raw USB access, and the display driver that presents it to Windows as a
# monitor. Both are required; see inf\usbdisplaydd.inf for why there are two.
#
# No reboot and no test signing are needed. Neither package loads anything
# into the kernel, so driver signature enforcement never comes into it; all
# Windows wants is a catalog that chains to a certificate it trusts, which
# this creates. It works with Secure Boot on. It does need elevation.
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
    $subject = 'CN=usbdisplay local signing'
    $cert = Get-ChildItem Cert:\LocalMachine\My |
        Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Subject $subject -Type CodeSigningCert `
            -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(5)
        # Trusted Publisher is what makes Windows accept the package without
        # prompting; Root is what makes the chain validate at all.
        foreach ($store in 'Root', 'TrustedPublisher') {
            $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
            $s.Open('ReadWrite'); $s.Add($cert); $s.Close()
        }
    }
    return $cert
}

# Windows keeps every copy of a package ever added, under a fresh oemNN name.
# Reinstalling during development therefore piles them up: the first driver
# left fifty nine copies of its two packages behind, and the stale ones go on
# competing to claim the adapter. Clearing our own previous copies before
# adding a new one keeps that from happening.
function Remove-PreviousCopies([string]$infName) {
    $blocks = (pnputil /enum-drivers | Out-String) -split "`r?`n`r?`n"
    foreach ($block in $blocks) {
        if ($block -match [regex]::Escape($infName)) {
            $oem = [regex]::Match($block, 'oem\d+\.inf').Value
            if ($oem) {
                Write-Host "    removing previous copy $oem"
                pnputil /delete-driver $oem /uninstall /force | Out-Null
            }
        }
    }
}

function Publish-Package([string]$stage, [string]$infName) {
    Remove-PreviousCopies $infName

    # The catalog tool needs the INF and every file it copies alone together
    # in one directory, which is why each package is staged first. Note the
    # OS names are case sensitive and there is no plain 10_ARM64.
    & (Get-KitTool 'Inf2Cat.exe') /driver:$stage /os:10_X64,10_RS3_ARM64 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "catalog generation failed for $infName" }

    $cert = Get-SigningCert
    $signtool = Get-KitTool 'signtool.exe'
    # /sm so signtool looks in the machine store rather than the user's.
    Get-ChildItem $stage -Include '*.cat', '*.dll' -Recurse | ForEach-Object {
        & $signtool sign /q /sm /fd sha256 /sha1 $cert.Thumbprint $_.FullName
        if ($LASTEXITCODE -ne 0) { throw "signing failed on $($_.Name)" }
    }

    pnputil /add-driver (Join-Path $stage $infName) /install
    # 3010 is "installed, but a restart would finish tidying up", which
    # happens when the package being replaced is in use. It is a success.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) {
        throw "installing $infName failed with code $LASTEXITCODE"
    }
}

# Reinstalling this package detaches the adapter's pixel interface, and it
# does not come back on its own: the device has to be cycled before Windows
# republishes the interface the driver looks for. Since the package almost
# never changes, skip it when the installed copy is already current. This is
# not a micro-optimisation, it is the difference between reinstalling the
# display driver and having to repair the USB binding afterwards.
function Test-WinUsbPackageCurrent {
    $wanted = (Select-String -Path (Join-Path $root 'inf\usbdisplay_winusb.inf') `
        -Pattern '^DriverVer').Line
    $blocks = (pnputil /enum-drivers | Out-String) -split "`r?`n`r?`n"
    foreach ($block in $blocks) {
        if ($block -match 'usbdisplay_winusb\.inf') {
            $version = [regex]::Match($wanted, '[\d/]+,([\d.]+)').Groups[1].Value
            if ($version -and $block -match [regex]::Escape($version)) { return $true }
        }
    }
    return $false
}

function Reset-PixelInterfaces {
    Get-PnpDevice | Where-Object {
        $_.InstanceId -match 'VID_345F.*MI_03' -or $_.InstanceId -match 'VID_534D'
    } | ForEach-Object {
        Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
        Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2
}

Write-Host '=== 1. raw USB access to the pixel interface ==='
if (Test-WinUsbPackageCurrent) {
    Write-Host '    already installed and current, leaving it alone'
} else {
    $stage1 = Join-Path $root 'build\package-winusb'
    if (Test-Path $stage1) { Remove-Item $stage1 -Recurse -Force }
    New-Item -ItemType Directory -Path $stage1 | Out-Null
    Copy-Item (Join-Path $root 'inf\usbdisplay_winusb.inf') $stage1
    Publish-Package $stage1 'usbdisplay_winusb.inf'
    Write-Host '    cycling the adapter so the interface is republished'
    Reset-PixelInterfaces
}

Write-Host ''
Write-Host '=== 2. display driver ==='
$binDir = Join-Path $root "build\driver\$Platform\$Configuration"
if (-not (Test-Path (Join-Path $binDir 'usbdisplaydd.dll'))) {
    throw "no driver binary in $binDir. Run scripts\build-driver.bat first."
}

# Use the build-stamped INF, not the source one. Windows keeps the previous
# binary if the version in the INF has not changed, which produces the
# memorable experience of fixing a bug, reinstalling, and watching the old
# bug persist.
$inf = Join-Path $binDir 'usbdisplaydd.inf'
if (-not (Test-Path $inf)) { $inf = Join-Path $root 'inf\usbdisplaydd.inf' }
Write-Host ("    " + ((Select-String -Path $inf -Pattern '^DriverVer').Line))

$stage2 = Join-Path $root 'build\package'
if (Test-Path $stage2) { Remove-Item $stage2 -Recurse -Force }
New-Item -ItemType Directory -Path $stage2 | Out-Null
Copy-Item (Join-Path $binDir 'usbdisplaydd.dll') $stage2
Copy-Item $inf (Join-Path $stage2 'usbdisplaydd.inf')
Publish-Package $stage2 'usbdisplaydd.inf'

Write-Host ''
Write-Host '=== 3. device node ==='
$devcon = Get-KitTool 'devcon.exe'
$existing = Get-PnpDevice -InstanceId 'ROOT\DISPLAY\*' -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -match 'USB Display' }
if ($existing) {
    Write-Host '    already present, updating'
    & $devcon update (Join-Path $stage2 'usbdisplaydd.inf') 'root\usbdisplaydd'
} else {
    Write-Host '    creating root\usbdisplaydd'
    & $devcon install (Join-Path $stage2 'usbdisplaydd.inf') 'root\usbdisplaydd'
}

Start-Sleep -Seconds 3
pnputil /scan-devices | Out-Null

Write-Host ''
Write-Host '=== 4. picture settings ==='
# The driver runs as LOCAL SERVICE and cannot read a user hive, so these live
# in the machine hive. Widen that one key so a brightness control can write it
# without asking for elevation every time somebody moves a slider. The scope
# is deliberately one key whose only values adjust the picture.
$settingsKey = 'HKLM:\SOFTWARE\usbdisplay'
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

Write-Host ''
Write-Host '=== result ==='
Get-PnpDevice | Where-Object {
    $_.InstanceId -match 'usbdisplaydd' -or $_.InstanceId -match 'VID_345F.*MI_03' -or
    $_.InstanceId -match 'VID_534D'
} | ForEach-Object {
    $p = $_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_ProblemCode','DEVPKEY_Device_Service'
    Write-Host ("  " + $_.Status.ToString().PadRight(8) +
                " problem=" + ($p | Where-Object KeyName -eq 'DEVPKEY_Device_ProblemCode').Data +
                " svc=" + ($p | Where-Object KeyName -eq 'DEVPKEY_Device_Service').Data +
                "  " + $_.InstanceId)
}
Write-Host ''
Write-Host 'If the display device is not started, read C:\Windows\Temp\usbdisplaydd.log:'
Write-Host 'it records the exact status of every step, which the event log does not.'
