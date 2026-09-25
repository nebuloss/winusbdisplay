$ErrorActionPreference = 'SilentlyContinue'
$log = 'C:\Windows\Temp\ms912xidd.log'

Add-Type -AssemblyName System.Windows.Forms

$last = 0
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Seconds 2
    $lines = @(Get-Content $log)
    if ($lines.Count -gt $last) {
        $lines[$last..($lines.Count - 1)] | ForEach-Object { Write-Output ("  " + $_) }
        $last = $lines.Count
    }
    $screens = @([System.Windows.Forms.Screen]::AllScreens)
    $hangs = @(Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddSeconds(-3)} -MaxEvents 50 -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -eq 10111 })
    $marks = ""
    if ($hangs.Count) { $marks = "  <-- HANG REPORTED" }
    Write-Output ("[t+" + ($i * 2) + "s] screens=" + $screens.Count + $marks)
}
