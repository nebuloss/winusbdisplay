# Restarts the display interface and decodes the exact PrepareHardware status.
# Assumes the driver package is already installed.
$ErrorActionPreference = 'SilentlyContinue'

$log = 'Microsoft-Windows-DriverFrameworks-UserMode/Operational'
wevtutil sl $log /e:true /q:true | Out-Null
wevtutil cl $log | Out-Null

$dev = Get-PnpDevice | Where-Object {
    $_.InstanceId -match 'VID_345F.*MI_03' -and $_.Status -ne 'Unknown'
} | Select-Object -First 1

if ($dev) {
    Write-Output ("==> restarting " + $dev.InstanceId)
    Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
    Start-Sleep -Seconds 1
    Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
    Start-Sleep -Seconds 5
} else {
    Write-Output "==> no live MI_03 device found"
}

$codes = @{
    'C0000182' = 'WdfUsbBackend::Create failed (USB target / bulk pipe)'
    'C0000034' = 'WdfDeviceQueryPropertyEx(ContainerId) failed'
    'C0000024' = 'ContainerId property had unexpected type'
    'C0000022' = 'HidTransport::OpenForContainer failed (cannot open HID sibling)'
}

Write-Output ""
Write-Output "==> PrepareHardware status codes seen"
Get-WinEvent -LogName $log -MaxEvents 120 -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    Where-Object { $_.Id -in @(2101, 2103) } |
    ForEach-Object {
        if ($_.Message -match '0x(C[0-9A-Fa-f]{7})') {
            $c = $Matches[1].ToUpper()
            $d = $codes[$c]
            if (-not $d) { $d = 'unmapped' }
            Write-Output ("  0x" + $c + "  " + $d)
        }
    }

Write-Output ""
Write-Output "==> device status"
Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_345F.*MI_03' } | ForEach-Object {
    $p = $_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_ProblemCode'
    Write-Output ("  " + $_.Status + " problem=" +
                  ($p | Where-Object KeyName -eq 'DEVPKEY_Device_ProblemCode').Data +
                  "  " + $_.InstanceId)
}

Write-Output ""
Write-Output "==> WUDFHost identity"
Get-CimInstance Win32_Process -Filter "Name='WUDFHost.exe'" | ForEach-Object {
    $o = Invoke-CimMethod -InputObject $_ -MethodName GetOwner
    Write-Output ("  pid " + $_.ProcessId + " as " + $o.Domain + "\" + $o.User)
}
