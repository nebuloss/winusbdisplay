# Dumps the actual device stack configuration so we can see what the INF did.
$ErrorActionPreference = 'SilentlyContinue'

$inst = 'USB\VID_345F&PID_9133&MI_03\6&29884711&0&0003'
$key  = "HKLM:\SYSTEM\CurrentControlSet\Enum\$inst"

Write-Output "==> device enum key"
$p = Get-ItemProperty -Path $key
foreach ($n in 'Service','ClassGUID','Driver','UpperFilters','LowerFilters','ConfigFlags') {
    Write-Output ("  " + $n.PadRight(14) + " = " + ($p.$n -join ', '))
}

Write-Output ""
Write-Output "==> device parameters / WUDF subkeys"
Get-ChildItem "$key\Device Parameters" -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Output ("  [" + $_.PSPath.Replace('Microsoft.PowerShell.Core\Registry::','') + "]")
    $props = Get-ItemProperty -Path $_.PSPath
    $props.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object {
        Write-Output ("      " + $_.Name + " = " + ($_.Value -join ', '))
    }
}

Write-Output ""
Write-Output "==> WinUsb service state"
Get-Service WinUsb -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Output ("  status=" + $_.Status + " starttype=" + $_.StartType)
}
sc.exe query WinUsb 2>&1 | Select-String 'STATE|SERVICE_NAME' | ForEach-Object { Write-Output ("  " + $_.Line.Trim()) }

Write-Output ""
Write-Output "==> is WinUSB.sys loaded"
$d = Get-CimInstance Win32_SystemDriver -Filter "Name='WinUsb'"
if ($d) { Write-Output ("  " + $d.Name + " state=" + $d.State + " started=" + $d.Started) }
else { Write-Output "  WinUsb driver object not present" }

Write-Output ""
Write-Output "==> UMDF reflector view of the device"
$wudf = "$key\Device Parameters\WUDF"
if (Test-Path $wudf) {
    (Get-ItemProperty $wudf).PSObject.Properties |
        Where-Object { $_.Name -notmatch '^PS' } |
        ForEach-Object { Write-Output ("  " + $_.Name + " = " + ($_.Value -join ', ')) }
} else {
    Write-Output "  no WUDF subkey"
}
