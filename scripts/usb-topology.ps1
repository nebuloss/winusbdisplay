$ErrorActionPreference = 'SilentlyContinue'

Write-Output "=== USB host controllers ==="
Get-PnpDevice -Class USB | Where-Object { $_.FriendlyName -match 'Controller|contr' } |
    ForEach-Object { Write-Output ("  " + $_.Status + "  " + $_.FriendlyName) }

Write-Output ""
Write-Output "=== USB hubs (Root hubs tell you what generation is available) ==="
Get-PnpDevice -Class USB | Where-Object { $_.FriendlyName -match 'hub|Hub|concentrateur' } |
    ForEach-Object { Write-Output ("  " + $_.Status + "  " + $_.FriendlyName + "  " + $_.InstanceId) }

Write-Output ""
Write-Output "=== our dongle: negotiated speed ==="
# DEVPKEY_Device_LocationInfo plus the USB speed property expose what the
# device actually negotiated, as opposed to what it is capable of.
$speeds = @{
  0 = 'Low (1.5 Mbps)'
  1 = 'Full (12 Mbps)'
  2 = 'High (480 Mbps)'
  3 = 'SuperSpeed (5 Gbps)'
  4 = 'SuperSpeedPlus (10 Gbps)'
}
Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_345F' -and $_.InstanceId -notmatch 'MI_' } |
    ForEach-Object {
        Write-Output ("  " + $_.InstanceId)
        $p = $_ | Get-PnpDeviceProperty -KeyName `
            '{83DA6326-97A6-4088-9453-A1923F573B29} 3', `
            'DEVPKEY_Device_LocationInfo', `
            'DEVPKEY_Device_Parent'
        foreach ($item in $p) {
            $val = $item.Data
            if ($item.KeyName -match '3$' -and $null -ne $val -and $speeds.ContainsKey([int]$val)) {
                $val = "$val = " + $speeds[[int]$val]
            }
            Write-Output ("    " + $item.KeyName + " = " + $val)
        }
    }

Write-Output ""
Write-Output "=== which hub is it plugged into ==="
Get-CimInstance Win32_USBControllerDevice | ForEach-Object {
    $dep = [wmi]$_.Dependent
    if ($dep.DeviceID -match 'VID_345F') {
        $ctrl = [wmi]$_.Antecedent
        Write-Output ("  device : " + $dep.DeviceID)
        Write-Output ("  on ctrl: " + $ctrl.Name)
    }
}
