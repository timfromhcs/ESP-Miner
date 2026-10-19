<#
.SYNOPSIS
    Samples /api/system/info from one or more Bitaxe units and writes a CSV plus a
    summary, so a firmware change can be compared against a measured baseline
    instead of a single API read.

.DESCRIPTION
    Every sample is appended to the CSV, so the deltas between the first and the
    last row give rates that are independent of whatever the boot-time counters
    happen to hold (session counters reset on pool reconnect, cumulative counters
    do not).

.PARAMETER Ip
    Device address, e.g. 192.168.178.80.

.PARAMETER DurationSec
    Sampling window in seconds.

.PARAMETER IntervalSec
    Seconds between samples.

.PARAMETER OutPrefix
    Output path prefix; the CSV gets .csv, the summary .summary.txt.

.EXAMPLE
    .\collect_telemetry.ps1 -Ip 192.168.178.80 -DurationSec 180
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Ip,
    [int]$DurationSec = 180,
    [int]$IntervalSec = 5,
    [string]$OutPrefix = "telemetry_${Ip}"
)

$ErrorActionPreference = 'Stop'

function Get-Info([string]$address) {
    try {
        $raw = Invoke-WebRequest -Uri "http://$address/api/system/info" -TimeoutSec 10 -UseBasicParsing
        return ($raw.Content | ConvertFrom-Json)
    } catch {
        return $null
    }
}

$csv = "$OutPrefix.csv"
$summary = "$OutPrefix.summary.txt"

$t0 = Get-Date
$rows = New-Object System.Collections.Generic.List[object]
$deadline = $t0.AddSeconds($DurationSec)

Write-Host "Collecting $DurationSec s from $Ip every $IntervalSec s -> $csv" -ForegroundColor Cyan

while ((Get-Date) -lt $deadline) {
    $j = Get-Info $Ip
    if ($null -ne $j) {
        $now = Get-Date
        $rows.Add([pscustomobject]@{
            t                = [math]::Round(($now - $t0).TotalSeconds, 1)
            wallclock        = $now.ToString('o')
            uptime           = $j.uptimeSeconds
            version          = $j.version
            partition        = $j.runningPartition
            hashrate         = $j.hashRate
            hashrate_1m      = $j.hashRate_1m
            hashrate_10m     = $j.hashRate_10m
            hashrate_1h      = $j.hashRate_1h
            errorPercentage  = $j.errorPercentage
            errorCount       = $j.hashrateMonitor.asics[0].errorCount
            sharesAccepted   = $j.sharesAccepted
            sharesRejected   = $j.sharesRejected
            sharesStale      = $j.sharesRejectedStale
            sharesOther      = $j.sharesRejectedOther
            poolDifficulty   = $j.poolDifficulty
            bestDiff         = $j.bestDiff
            blockFound       = $j.blockFound
            power            = $j.power
            voltage          = $j.voltage
            current          = $j.current
            temp             = $j.temp
            vrTemp           = $j.vrTemp
            frequency        = $j.actualFrequency
            coreVoltage      = $j.coreVoltageActual
            fanspeed         = $j.fanspeed
            responseTime     = $j.responseTime
            processTime      = $j.processTime
            notifyReceived   = $j.notifyReceived
            notifyDropped    = $j.notifyDropped
            workReceived     = $j.workReceived
            freeHeapInternal = $j.freeHeapInternal
            freeHeapSpiram   = $j.freeHeapSpiram
            wifiRSSI         = $j.wifiRSSI
            cpuUsage         = $j.cpuUsage
        })
    } else {
        Write-Warning "sample failed at t=$([math]::Round(((Get-Date) - $t0).TotalSeconds,1))s"
    }
    Start-Sleep -Seconds $IntervalSec
}

if ($rows.Count -lt 2) {
    Write-Error "only $($rows.Count) successful samples - nothing to summarise"
    exit 1
}

$rows | Export-Csv -Path $csv -NoTypeInformation -Encoding UTF8

function Stats($name, $values) {
    $v = @($values | Where-Object { $_ -ne $null })
    if ($v.Count -eq 0) { return [pscustomobject]@{ Metric = $name; First = '-'; Last = '-'; Min = '-'; Max = '-'; Mean = '-'; Delta = '-' } }
    $m = $v | Measure-Object -Minimum -Maximum -Average
    [pscustomobject]@{
        Metric = $name
        First  = [math]::Round($v[0], 3)
        Last   = [math]::Round($v[-1], 3)
        Min    = [math]::Round($m.Minimum, 3)
        Max    = [math]::Round($m.Maximum, 3)
        Mean   = [math]::Round($m.Average, 3)
        Delta  = [math]::Round(($v[-1] - $v[0]), 3)
    }
}

$span = $rows[-1].t - $rows[0].t
$report = New-Object System.Collections.Generic.List[object]
$report.Add(([pscustomobject]@{ Metric = 'SAMPLES'; First = $rows.Count; Last = ''; Min = ''; Max = ''; Mean = ''; Delta = '' }))
$report.Add(([pscustomobject]@{ Metric = 'SPAN_SEC'; First = [math]::Round($span, 1); Last = ''; Min = ''; Max = ''; Mean = ''; Delta = '' }))
$report.Add(([pscustomobject]@{ Metric = 'VERSION'; First = $rows[0].version; Last = $rows[-1].version; Min = ''; Max = ''; Mean = ''; Delta = '' }))

foreach ($col in 'hashrate', 'hashrate_1m', 'hashrate_10m', 'hashrate_1h', 'errorPercentage', 'errorCount',
    'sharesAccepted', 'sharesRejected', 'sharesStale', 'sharesOther', 'poolDifficulty', 'power',
    'temp', 'vrTemp', 'responseTime', 'processTime', 'notifyReceived', 'notifyDropped', 'workReceived',
    'freeHeapInternal', 'cpuUsage') {
    $report.Add((Stats $col ($rows | ForEach-Object { $_.$col })))
}

# Rates need the first/last value of a *monotonic* counter, so a reconnect that
# resets the counter shows up as a negative delta instead of a plausible-looking
# huge rate.
$report.Add([pscustomobject]@{
    Metric = 'shares_per_min'
    First  = ''
    Last   = ''
    Min    = ''
    Max    = ''
    Mean   = ''
    Delta  = [math]::Round((($rows[-1].sharesAccepted - $rows[0].sharesAccepted) / $span) * 60, 2)
})
$report.Add([pscustomobject]@{
    Metric = 'rejects_per_min'
    First  = ''; Last = ''; Min = ''; Max = ''; Mean = ''
    Delta  = [math]::Round((($rows[-1].sharesRejected - $rows[0].sharesRejected) / $span) * 60, 2)
})
$report.Add([pscustomobject]@{
    Metric = 'hashrate_1h_mean_GHs'
    First  = ''; Last = ''; Min = ''; Max = ''
    Mean   = [math]::Round((($rows | ForEach-Object { $_.hashrate_1h } | Measure-Object -Average).Average), 1)
    Delta  = ''
})
$report.Add([pscustomobject]@{
    Metric = 'J_per_TH'
    First  = ''; Last = ''; Min = ''; Max = ''; Mean = ''
    Delta  = [math]::Round((($rows | ForEach-Object { $_.power } | Measure-Object -Average).Average) /
        [math]::Max(1e-9, (($rows | ForEach-Object { $_.hashrate_1h } | Measure-Object -Average).Average / 1000)), 2)
})

$report | Format-Table -AutoSize | Out-String -Width 200 | Tee-Object -FilePath $summary
Write-Host "CSV: $csv" -ForegroundColor Green
Write-Host "Summary: $summary" -ForegroundColor Green