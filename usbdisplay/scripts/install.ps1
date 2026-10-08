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

# Where everything else lives, relative to this script.
#
# In a source tree this script sits in scripts\ and everything is a level
# up. In a released package it sits at the top with the files beside it.
# Deciding by looking is the only thing that works for both, and getting it
# wrong means a released package looks for its own contents one directory
# too high.
$root = if (Test-Path (Join-Path $PSScriptRoot 'driver\usbdisplaydd.inf')) {
    $PSScriptRoot
} else {
    Split-Path -Parent $PSScriptRoot
}

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
    }

    # Every time, not only when the certificate is new.
    #
    # Holding the key and being trusted are separate things, and they can
    # come apart: anything that empties the trust stores leaves the key
    # sitting in My, so this used to decide the certificate was fine and
    # install a package Windows then refused with a complaint about an
    # untrusted root. Which is exactly what one of this project's own
    # uninstallers did. Adding an entry that is already there costs
    # nothing.
    #
    # Trusted Publisher is what makes Windows accept the package without
    # prompting; Root is what makes the chain validate at all.
    foreach ($storeName in 'Root', 'TrustedPublisher') {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
        $store.Open('ReadWrite')
        $store.Add($cert)
        $store.Close()
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

# A released package arrives already signed and with its catalogs built,
# because doing that needs two tools from the Windows SDK and asking a user
# to install a developer kit to get their second monitor working is not
# reasonable. A tree built from source has neither, and makes both here.
$released = Test-Path (Join-Path $root 'driver\usbdisplaydd.cat')

if ($released) {
    Write-Host '=== installing a released package ==='

    # Windows will not accept the packages until it trusts the certificate
    # they were signed with. Worth being plain about: this is the step that
    # matters, and it is a real decision rather than a formality.
    $certificate = Join-Path $root 'usbdisplay.cer'
    if (Test-Path $certificate) {
        Write-Host '    trusting the release certificate'
        # Through the .NET store API rather than Import-Certificate, which
        # needs a provider that is missing on some machines and then fails
        # complaining about a drive rather than about certificates.
        $blob = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificate)
        foreach ($storeName in 'Root', 'TrustedPublisher') {
            $store = [System.Security.Cryptography.X509Certificates.X509Store]::new(
                $storeName, 'LocalMachine')
            $store.Open('ReadWrite')
            $store.Add($blob)
            $store.Close()
        }
    }

    foreach ($package in @(
        @{ Name = 'raw USB access'; Path = 'winusb\usbdisplay_winusb.inf' },
        @{ Name = 'display driver';  Path = 'driver\usbdisplaydd.inf' })) {
        Write-Host "    installing the $($package.Name)"
        pnputil /add-driver (Join-Path $root $package.Path) /install
        # 0 is plain success. 3010 means installed, restart to tidy up.
        # 259 means the package was added but no device currently needs it,
        # which is what happens when it is already current: a success that
        # reads like a failure if taken at face value.
        if ($LASTEXITCODE -notin 0, 259, 3010) {
            throw "installing the $($package.Name) failed with code $LASTEXITCODE"
        }
    }

    $devcon = $null
    foreach ($r in @("${env:ProgramFiles(x86)}\Windows Kits\10\Tools",
                     "${env:ProgramFiles(x86)}\Windows Kits\10\bin")) {
        if (-not (Test-Path $r)) { continue }
        $devcon = Get-ChildItem $r -Recurse -Filter devcon.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
        if ($devcon) { break }
    }

    # Found by hardware id, which is the only thing that actually
    # identifies these.
    #
    # Not by name: names have changed between versions, so matching one
    # leaves the older device unrecognised, a second gets added, and the
    # user ends up with monitors that can never work. Not by service
    # either: every user mode driver runs under the same one, so that
    # matches every indirect display on the machine including other
    # people's. The hardware id is ours and nobody else's.
    function Get-OurDevices([string[]]$ids) {
        Get-PnpDevice -InstanceId 'ROOT\DISPLAY\*' -ErrorAction SilentlyContinue |
            Where-Object {
                $hardware = ($_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_HardwareIds' `
                    -ErrorAction SilentlyContinue).Data
                $hardware -and ($hardware | Where-Object { $ids -contains $_ })
            }
    }

    # Removing a display device is done carefully, and the reason is a
    # crashed machine.
    #
    # These nodes are not inert: while one exists and is started, its
    # monitor is part of the desktop and the kernel side of the display
    # stack holds state for it. Ripping one out with a removal call, several
    # in a row, with a monitor still attached, bug checked the machine
    # outright: IRQL_NOT_LESS_OR_EQUAL, inside kernel code this project does
    # not contain and cannot fix.
    #
    # So the order matters. Disable first, which makes Windows take the
    # monitor out of the desktop topology through the path designed for it
    # and lets this driver stop cleanly. Wait for that to finish. Only then
    # remove. And never in a tight loop.
    function Remove-DisplayDevice($device) {
        Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false `
            -ErrorAction SilentlyContinue
        # Long enough for the stop to complete. The framework reports a
        # driver that has not finished as hung, and hurrying this is what
        # caused the crash.
        Start-Sleep -Seconds 2
        if ($devcon) {
            & $devcon.FullName remove "@$($device.InstanceId)" | Out-Null
        } else {
            # Without devcon the device is left disabled rather than
            # removed. A disabled device is harmless and visible; forcing a
            # removal without the tool meant for it is not worth the risk.
            Write-Host '      left disabled, devcon is not available to remove it'
        }
        Start-Sleep -Seconds 1
    }

    foreach ($stale in Get-OurDevices @('root\usbhdmidd', 'root\ms912xidd')) {
        Write-Host '    retiring a device left by an earlier version'
        Remove-DisplayDevice $stale
    }

    $existing = @(Get-OurDevices @('root\usbdisplaydd'))

    # More than one is a duplicate an earlier version could create. Keep the
    # first and retire the rest, or the desktop grows a monitor for each and
    # only one of them is ever driven.
    if ($existing.Count -gt 1) {
        Write-Host "    retiring $($existing.Count - 1) duplicate device(s)"
        foreach ($duplicate in $existing[1..($existing.Count - 1)]) {
            Remove-DisplayDevice $duplicate
        }
        $existing = @($existing[0])
    }

    if (-not $existing) {
        if ($devcon) {
            & $devcon.FullName install (Join-Path $root 'driver\usbdisplaydd.inf') 'root\usbdisplaydd'
        } else {
            # Without devcon the node is created through the setup API
            # directly, so a released package needs nothing from the SDK.
            Write-Host '    creating the device node'
            $code = @'
using System;
using System.Runtime.InteropServices;
public class Dev {
  [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool UpdateDriverForPlugAndPlayDevicesW(
    IntPtr parent, string hardwareId, string infPath, uint flags, out bool reboot);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid cls, IntPtr parent);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name,
    ref Guid cls, string desc, IntPtr parent, uint flags, out SP_DEVINFO_DATA data);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set,
    ref SP_DEVINFO_DATA data, uint prop, byte[] buffer, uint size);
  [DllImport("setupapi.dll", SetLastError=true)]
  public static extern bool SetupDiCallClassInstaller(uint fn, IntPtr set, ref SP_DEVINFO_DATA data);
  [StructLayout(LayoutKind.Sequential)]
  public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid;
    public uint DevInst; public IntPtr Reserved; }
}
'@
            Add-Type -TypeDefinition $code -ErrorAction SilentlyContinue
            $display = [Guid]'4d36e968-e325-11ce-bfc1-08002be10318'
            $set = [Dev]::SetupDiCreateDeviceInfoList([ref]$display, [IntPtr]::Zero)
            $info = New-Object Dev+SP_DEVINFO_DATA
            $info.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($info)
            if ([Dev]::SetupDiCreateDeviceInfoW($set, 'Display', [ref]$display,
                    $null, [IntPtr]::Zero, 0x00000001, [ref]$info)) {
                $id = [Text.Encoding]::Unicode.GetBytes("root\usbdisplaydd`0`0")
                [void][Dev]::SetupDiSetDeviceRegistryPropertyW($set, [ref]$info, 1, $id, $id.Length)
                [void][Dev]::SetupDiCallClassInstaller(0x0000000f, $set, [ref]$info)
                $reboot = $false
                [void][Dev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero,
                    'root\usbdisplaydd', (Join-Path $root 'driver\usbdisplaydd.inf'), 1, [ref]$reboot)
            }
        }
    } else {
        Write-Host '    the device is already present, updating it'
        if ($devcon) {
            & $devcon.FullName update (Join-Path $root 'driver\usbdisplaydd.inf') 'root\usbdisplaydd'
        }
    }

    Start-Sleep -Seconds 3
    pnputil /scan-devices | Out-Null
}

if (-not $released) {

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

}

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
# Widened through the .NET types rather than the Get-Acl and Set-Acl
# cmdlets. Those live in a module that does not load on every machine, and
# when it does not they fail in a way that leaves the install looking
# successful while the setting stays unwritable: the brightness control then
# silently cannot save anything.
try {
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
        'SOFTWARE\usbdisplay',
        [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
        [System.Security.AccessControl.RegistryRights]::ChangePermissions)
    $acl = $key.GetAccessControl()
    $users = New-Object System.Security.Principal.SecurityIdentifier(
        [System.Security.Principal.WellKnownSidType]::BuiltinUsersSid, $null)
    $rule = New-Object System.Security.AccessControl.RegistryAccessRule(
        $users,
        [System.Security.AccessControl.RegistryRights]'SetValue,QueryValues,ReadKey,CreateSubKey',
        [System.Security.AccessControl.InheritanceFlags]::ContainerInherit,
        [System.Security.AccessControl.PropagationFlags]::None,
        [System.Security.AccessControl.AccessControlType]::Allow)
    $acl.AddAccessRule($rule)
    $key.SetAccessControl($acl)
    $key.Close()
    Write-Host '    users can now set brightness without elevation'
} catch {
    Write-Host "    WARNING: could not widen permissions on $settingsKey"
    Write-Host "    $($_.Exception.Message)"
    Write-Host '    Brightness will fall back to a lower quality method.'
}

Write-Host ''
Write-Host '=== 5. keeping it working after a reboot ==='
# Windows fails to load this driver during boot, every time. The device is
# root enumerated, so Plug and Play starts it during early boot device
# enumeration, before the user mode driver framework is running, and a user
# mode driver cannot load that early: the reflector fails with
# STATUS_FAILED_DRIVER_ENTRY (0xC0000365) and Plug and Play does not retry.
# The monitor then stays black for the whole session.
#
# This is also why reinstalling always appeared to fix it, which misled this
# project for a long time. Reinstalling re-enumerates the device, and by then
# the framework is up.
#
# So a scheduled task re-enumerates it shortly after startup, as SYSTEM.
# Both triggers are wanted: logon for the normal case, and boot with a delay
# so a machine sitting at the logon screen still drives the panel.
#
# Both therefore fire in an ordinary session, so repair.ps1 decides for
# itself whether a restart is needed. Without that it blanked the panel twice
# in the first two minutes of every session, the second time to cure a
# display that was already working.
$taskName = 'usbdisplay repair after startup'
$repairScript = Join-Path $PSScriptRoot 'repair.ps1'
if (Test-Path $repairScript) {
    try {
        $action = New-ScheduledTaskAction -Execute 'powershell.exe' `
            -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$repairScript`""
        $atLogon = New-ScheduledTaskTrigger -AtLogOn
        $atLogon.Delay = 'PT15S'
        $atBoot = New-ScheduledTaskTrigger -AtStartup
        $atBoot.Delay = 'PT1M'
        $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' `
            -LogonType ServiceAccount -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
            -DontStopIfGoingOnBatteries -StartWhenAvailable `
            -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
        Register-ScheduledTask -TaskName $taskName -Action $action `
            -Trigger $atLogon, $atBoot -Principal $principal `
            -Settings $settings -Force | Out-Null
        Write-Host '    the display will be restarted automatically after startup'
    } catch {
        Write-Host '    WARNING: could not register the startup task'
        Write-Host "    $($_.Exception.Message)"
        Write-Host '    The monitor may be black after a reboot until you run'
        Write-Host "    $repairScript"
    }
} else {
    Write-Host "    WARNING: $repairScript is missing, startup repair not set up"
}

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
