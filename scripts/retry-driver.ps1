# Reinstall the driver, restart the device, and report the exact failure code.
$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot 'install-driver.ps1') -SkipBuild *>&1 | Out-Null

$log = 'Microsoft-Windows-DriverFrameworks-UserMode/Operational'
wevtutil sl $log /e:true /q:true
wevtutil cl $log

$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_345F.*MI_03' -and $_.Status -ne 'Unknown' } | Select-Object -First 1
if ($dev) {
    Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
    Start-Sleep -Seconds 1
    Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
    Start-Sleep -Seconds 4
}

$codes = @{
    '0xC0000182' = 'WdfUsbBackend::Create failed (USB target / bulk pipe)'
    '0xC0000034' = 'WdfDeviceQueryPropertyEx(ContainerId) failed'
    '0xC0000024' = 'ContainerId property had unexpected type'
    '0xC0000022' = 'HidTransport::OpenForContainer failed (cannot open HID sibling)'
    '0x00000000' = 'PrepareHardware succeeded'
}

Write-Output "==> PrepareHardware result"
$hit = Get-WinEvent -LogName $log -MaxEvents 80 -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    Where-Object { $_.Id -eq 2103 -or ($_.Id -eq 2101 -and $_.Message -match '27, 0') }
foreach ($e in $hit) {
    if ($e.Message -match '(0x[0-9A-Fa-f]{8})') {
        $c = '0x' + $Matches[1].Substring(2).ToUpper()
        $desc = $codes[$c]
        if (-not $desc) { $desc = 'unmapped status' }
        Write-Output ("  " + $c + "  " + $desc)
    }
}

Write-Output ""
Write-Output "==> device status"
Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_345F.*MI_03' } | ForEach-Object {
    $p = $_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_ProblemCode'
    Write-Output ("  " + $_.Status + " problem=" + ($p | Where-Object KeyName -eq 'DEVPKEY_Device_ProblemCode').Data + "  " + $_.InstanceId)
}

Write-Output ""
Write-Output "==> WUDFHost identity"
Get-CimInstance Win32_Process -Filter "Name='WUDFHost.exe'" | ForEach-Object {
    $o = Invoke-CimMethod -InputObject $_ -MethodName GetOwner
    Write-Output ("  pid " + $_.ProcessId + " running as " + $o.Domain + "\" + $o.User)
}
