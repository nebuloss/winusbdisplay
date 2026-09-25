# Enables UMDF tracing, restarts the device, and dumps everything relevant.
$ErrorActionPreference = 'SilentlyContinue'

$log = 'Microsoft-Windows-DriverFrameworks-UserMode/Operational'
Write-Output "==> enabling $log"
wevtutil sl $log /e:true /q:true
wevtutil cl $log

Write-Output "==> restarting the display interface"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_345F.*MI_03' -and $_.Status -ne 'Unknown' } | Select-Object -First 1
if ($dev) {
    Write-Output ("    " + $dev.InstanceId)
    Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
    Start-Sleep -Seconds 1
    Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
    Start-Sleep -Seconds 4
} else {
    Write-Output "    no device found"
}

Write-Output ""
Write-Output "==> UMDF operational events"
Get-WinEvent -LogName $log -MaxEvents 60 -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    Where-Object { $_.Message -match 'ms912x|IddCx|Idd|fail|error|0x' -or $_.LevelDisplayName -in @('Error','Warning') } |
    Select-Object -Last 20 |
    ForEach-Object {
        Write-Output ("[" + $_.TimeCreated.ToString('HH:mm:ss') + "] " + $_.LevelDisplayName + " id=" + $_.Id)
        ($_.Message -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -First 3 |
            ForEach-Object { Write-Output ("    " + $_.Trim()) }
    }

Write-Output ""
Write-Output "==> System log, driver related, last 10 minutes"
Get-WinEvent -FilterHashtable @{ LogName='System'; StartTime=(Get-Date).AddMinutes(-10) } -MaxEvents 200 -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'WUDF|Kernel-PnP|UserPnp|Display' } |
    Select-Object -First 15 |
    ForEach-Object {
        Write-Output ("[" + $_.TimeCreated.ToString('HH:mm:ss') + "] " + $_.LevelDisplayName + " " + $_.ProviderName + " id=" + $_.Id)
        ($_.Message -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -First 3 |
            ForEach-Object { Write-Output ("    " + $_.Trim()) }
    }

Write-Output ""
Write-Output "==> setupapi device install tail"
$sp = "$env:windir\inf\setupapi.dev.log"
if (Test-Path $sp) {
    Get-Content $sp -Tail 60 | Where-Object { $_ -match 'ms912x|iddcx|Idd|error|fail|!!!' } |
        Select-Object -Last 25
}
