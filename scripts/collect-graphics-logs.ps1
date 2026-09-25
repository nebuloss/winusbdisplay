$ErrorActionPreference = 'SilentlyContinue'
$since = (Get-Date).AddMinutes(-15)

Write-Output "=== channels mentioning display/graphics with recent entries ==="
$names = wevtutil el 2>$null | Where-Object {
    $_ -match 'Dxgkrnl|Display|Graphics|Idd|Indirect|DeviceSetupManager'
}
foreach ($n in $names) {
    # Analytic/debug channels must be read Oldest-first; skip them entirely.
    $ev = Get-WinEvent -FilterHashtable @{ LogName = $n; StartTime = $since } `
        -MaxEvents 12 -ErrorAction SilentlyContinue 2>$null
    if (-not $ev) { continue }
    Write-Output ""
    Write-Output ("--- " + $n)
    $ev | Sort-Object TimeCreated | ForEach-Object {
        Write-Output ("  [" + $_.TimeCreated.ToString('HH:mm:ss') + "] " + $_.LevelDisplayName + " id=" + $_.Id)
        ($_.Message -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -First 2 |
            ForEach-Object { Write-Output ("      " + $_.Trim()) }
    }
}

Write-Output ""
Write-Output "=== graphics adapters registered with the OS ==="
Get-CimInstance Win32_VideoController | ForEach-Object {
    Write-Output ("  " + $_.Name + "  status=" + $_.Status + " availability=" + $_.Availability)
}

Write-Output ""
Write-Output "=== Control\Video keys ==="
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Video' -ErrorAction SilentlyContinue |
    ForEach-Object {
        $svc = (Get-ItemProperty ($_.PSPath + '\0000') -ErrorAction SilentlyContinue).Service
        $desc = (Get-ItemProperty ($_.PSPath + '\0000') -ErrorAction SilentlyContinue).'Device Description'
        Write-Output ("  " + $_.PSChildName + "  svc=" + $svc + "  " + $desc)
    }
