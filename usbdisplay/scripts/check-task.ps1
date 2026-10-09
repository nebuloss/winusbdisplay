# Checks the task definition driversetup writes, without registering it.
#
# The event subscriptions are XML inside XML, so a mistake in them is silent:
# the task registers and simply never fires. Setting XmlText runs the task
# scheduler's own parser, and the XPath is then run against the live system
# log, which is the only way to find out that a filter matches nothing.

$ErrorActionPreference = 'Stop'

$path = Join-Path $env:TEMP 'usbdisplaytask.xml'
if (-not (Test-Path $path)) { throw "no task definition at $path (run driversetup /taskxml)" }

$xml = Get-Content $path -Raw

$svc = New-Object -ComObject Schedule.Service
$svc.Connect()
$task = $svc.NewTask(0)
$task.XmlText = $xml
Write-Output "definition parses: $($task.Triggers.Count) triggers"

foreach ($t in $task.Triggers) {
    $kind = $t.Type
    $delay = ''
    try { $delay = $t.Delay } catch { }
    Write-Output ("  type=$kind delay=$delay rep=" + $t.Repetition.Interval + '/' + $t.Repetition.Duration)
    if ($kind -eq 0) {
        $sub = $t.Subscription
        Write-Output "    subscription parses as xml: $([bool]([xml]$sub))"
        $select = ([xml]$sub).QueryList.Query.Select.'#text'
        if (-not $select) { $select = ([xml]$sub).QueryList.Query.Select }
        Write-Output "    query: $select"
        try {
            $hits = @(Get-WinEvent -LogName System -FilterXPath $select -MaxEvents 3 -ErrorAction Stop)
            Write-Output ("    matches in the live log: " + $hits.Count + " (" +
                (($hits | ForEach-Object { "$($_.Id)@$($_.TimeCreated)" }) -join ', ') + ")")
        } catch {
            Write-Output ("    no match in the live log: " + $_.Exception.Message)
        }
    }
}

Write-Output ''
Write-Output "action: $($task.Actions | ForEach-Object { $_.Path + ' ' + $_.Arguments })"
