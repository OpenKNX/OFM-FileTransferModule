#!/usr/bin/env pwsh
<#
Open ■
┬────┴  7-NonBlocking.Tests
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: scripts/Hardening/Suites/7-NonBlocking.Tests.ps1

.SYNOPSIS
    F-N - the non-blocking requirement, as a measurement rather than an intention.

.DESCRIPTION
    Dot-sourced by Invoke-FtmHardening.ps1, which then calls Invoke-FtmSuiteNonBlocking.

    "Never do long CRC, IO or compute in one loop() pass" is a standing rule of this
    firmware, and it is the rule that is easiest to break without noticing: the build stays
    green, the feature works, and only the KNX side degrades. A synchronous whole-file CRC
    on a large file has already been shown to reboot the device.

    So this suite does not read code - it measures. During each of the three operations
    that are known to be able to block, it keeps probing the device and records how long
    the longest gap was. A gap above threshold is a FAIL, not a note: the whole point of
    the rule is that there is a number.

    Caveat that is enforced, not merely documented: never measure during an OTA update.
    The measurement traffic itself competes with the update.
#>

Set-StrictMode -Version Latest

function Measure-FtmResponsiveness {
    <#
    .SYNOPSIS
        Probes the console repeatedly for a duration and returns the worst gap in ms.
    .DESCRIPTION
        A cheap command is used on purpose: what is being measured is whether the device
        gets back to its loop, not how fast it can do work.
    #>
    param($Console, [int]$Seconds = 10, [int]$ProbeTimeoutMs = 3000)
    $gaps = @()
    $worst = 0
    $end = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $end) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $out = Invoke-FtmConsoleCommand -Console $Console -Command 'version' -TimeoutMs $ProbeTimeoutMs -QuietMs 250
        $sw.Stop()
        $ms = [int]$sw.ElapsedMilliseconds
        if ($out.Trim().Length -eq 0) { $ms = $ProbeTimeoutMs }
        $gaps += $ms
        if ($ms -gt $worst) { $worst = $ms }
        Start-Sleep -Milliseconds 120
    }
    $avg = 0
    if ($gaps.Count -gt 0) { $avg = [int](($gaps | Measure-Object -Sum).Sum / $gaps.Count) }
    return [pscustomobject]@{ Worst = $worst; Average = $avg; Samples = $gaps.Count }
}

function Invoke-FtmSuiteNonBlocking {
    param([Parameter(Mandatory)]$Ctx, [string]$SuiteTitle = 'F-N Non-blocking')

    $con = $Ctx.Console
    $t = $Ctx.Target

    # The threshold. A device that stays inside its loop answers a cheap console command
    # in tens of milliseconds; a second-long gap means something ran to completion inside
    # one pass. 1200 ms leaves room for serial and scheduling noise without hiding a stall.
    $maxGapMs = 1200

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-N-1' -Title 'Baseline responsiveness while idle' -Reference 'standing rule: never do long work in one loop() pass' -Body {
        $m = Measure-FtmResponsiveness -Console $con -Seconds 8
        Add-FtmEvidence -Note "idle: worst $($m.Worst) ms, average $($m.Average) ms over $($m.Samples) probes"
        Assert-FtmTrue ($m.Samples -gt 0) 'no probes completed - the console is not usable'
        Assert-FtmTrue ($m.Worst -le $maxGapMs) "the device already stalls for $($m.Worst) ms while IDLE - every later measurement is meaningless until this is fixed"
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-N-2' -Title 'Device stays responsive while a CRC is computed' -Reference 'FTC-Reference: LittleFS CRC is cooperative across loop passes; a synchronous one reboots on a big file' -Body {
        # FileInfo on LittleFS starts the cooperative CRC job. If it were computed in one
        # pass, this is exactly where the device would disappear.
        [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t info ftm-perf.bin" -TimeoutMs 2000 -QuietMs 200)
        $m = Measure-FtmResponsiveness -Console $con -Seconds 10
        Add-FtmEvidence -Note "during CRC: worst $($m.Worst) ms, average $($m.Average) ms over $($m.Samples) probes"
        Assert-FtmTrue ($m.Samples -gt 0) 'the console became unusable while a CRC was running'
        Assert-FtmTrue ($m.Worst -le $maxGapMs) "the device stalled for $($m.Worst) ms during CRC computation - the CRC is not chunked across loop passes"
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-N-3' -Title 'Device stays responsive during a transfer' -Reference 'standing rule: the per-chunk upload path must stay lean' -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'runs a real transfer - run with -IncludeDestructive' }
        [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-perf.bin safe" -TimeoutMs 2500 -QuietMs 200)
        $m = Measure-FtmResponsiveness -Console $con -Seconds 15
        Add-FtmEvidence -Note "during transfer: worst $($m.Worst) ms, average $($m.Average) ms over $($m.Samples) probes"
        [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t cancel" -TimeoutMs 20000)
        Assert-FtmTrue ($m.Samples -gt 0) 'the console became unusable during a transfer'
        Assert-FtmTrue ($m.Worst -le $maxGapMs) "the device stalled for $($m.Worst) ms during a transfer - a chunk write is blocking the loop"
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-N-4' -Title 'Device stays responsive while the console ring is drained' -Reference 'FTC-Console: conLoop() runs under freeLoopTime()' -Body {
        [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t con help" -TimeoutMs 2500 -QuietMs 200)
        $m = Measure-FtmResponsiveness -Console $con -Seconds 10
        Add-FtmEvidence -Note "during console drain: worst $($m.Worst) ms, average $($m.Average) ms over $($m.Samples) probes"
        Assert-FtmTrue ($m.Samples -gt 0) 'the console became unusable while draining the ring'
        Assert-FtmTrue ($m.Worst -le $maxGapMs) "the device stalled for $($m.Worst) ms while draining the console ring"
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-N-5' -Title 'Device stays responsive while a directory is listed' -Reference 'FTC-Reference: DirList reads the provider store; a large directory must not block' -Body {
        [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t ll" -TimeoutMs 2000 -QuietMs 200)
        $m = Measure-FtmResponsiveness -Console $con -Seconds 8
        Add-FtmEvidence -Note "during listing: worst $($m.Worst) ms, average $($m.Average) ms over $($m.Samples) probes"
        Assert-FtmTrue ($m.Worst -le $maxGapMs) "the device stalled for $($m.Worst) ms while listing a directory"
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-N-6' -Title 'No measurement is taken during an OTA update' -Reference 'operational rule: never probe the runtime during OTA - the probe competes with the update' -Body {
        # A guard rather than a measurement: if an update is in progress, the numbers above
        # would be meaningless and the probing itself would be harmful.
        $out = Invoke-FtmConsoleCommand -Console $con -Command 'version' -TimeoutMs 8000
        Add-FtmEvidence -Output $out
        Assert-FtmNotMatch $out '(?i)(ota|update in progress|flashing)' 'an update appears to be running - stop the run and repeat it afterwards'
    }
}
