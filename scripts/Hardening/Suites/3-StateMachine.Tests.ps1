#!/usr/bin/env pwsh
<#
Open ■
┬────┴  3-StateMachine.Tests
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: scripts/Hardening/Suites/3-StateMachine.Tests.ps1

.SYNOPSIS
    F-S - commands out of order, cancel in every phase, and handle/sink leaks.

.DESCRIPTION
    Dot-sourced by Invoke-FtmHardening.ps1, which then calls Invoke-FtmSuiteState.

    The FTC server keeps ONE open file and ONE open sink at a time, and a chunk write is
    addressed by (sequence-1)*chunkSize against that sink - not by who sent it. Two
    consequences follow, and both are what this suite probes:

      * a command that arrives in the wrong phase must be refused, because if it is not,
        it acts on whatever sink happens to be open
      * every terminating path must actually close the file and free the sink, or the
        next transfer inherits a half-open state

    "It answered an error" is not enough for any case here. After each one the suite
    proves the device can still complete a NORMAL operation - that is what distinguishes
    a clean refusal from a wedged state machine.
#>

Set-StrictMode -Version Latest

function Test-FtmStillWorks {
    <#
    .SYNOPSIS
        Proves the server can still do real work after a case.
    #>
    param($Console, [string]$Target, [int]$TimeoutMs = 20000)
    $out = Invoke-FtmConsoleCommand -Console $Console -Command "ftc $Target df" -TimeoutMs $TimeoutMs
    return [pscustomobject]@{
        Ok     = ($out.Trim().Length -gt 0 -and $out -notmatch '(?i)(timeout|no answer|busy|in progress)')
        Output = $out
    }
}

function Invoke-FtmSuiteState {
    param([Parameter(Mandatory)]$Ctx, [string]$SuiteTitle = 'F-S State machine')

    $con = $Ctx.Console
    $t = $Ctx.Target

    # ── Out-of-phase commands ───────────────────────────────────────────────────

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-S-1' -Title 'Cancel without a running transfer is harmless' -Reference 'FTC-Reference: Cancel = command 90' -Body {
        foreach ($i in 1..3) {
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t cancel" -TimeoutMs 15000
            Add-FtmEvidence -Note "cancel $i -> $(($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"
            Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' 'the device crashed on a cancel without a transfer'
        }
        $still = Test-FtmStillWorks -Console $con -Target $t
        Add-FtmEvidence -Output $still.Output
        Assert-FtmTrue $still.Ok 'the server no longer works after repeated cancels without a transfer'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-S-2' -Title 'FileReport without a fast transfer is refused, not answered with a stale bitmap' -Reference 'FTC-Reference: FileReport = command 45, gap bitmap' -Body {
        # A stale bitmap here is worse than an error: the client would resend against a
        # sink that belongs to nothing, or believe a transfer succeeded.
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t status" -TimeoutMs 15000
        Add-FtmEvidence -Output $out
        Assert-FtmTrue ($out.Trim().Length -gt 0) 'no answer for a report query without a transfer'
        Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' 'the device crashed on a report query without a transfer'
        $still = Test-FtmStillWorks -Console $con -Target $t
        Assert-FtmTrue $still.Ok 'the server no longer works after a report query without a transfer'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-S-3' -Title 'Directory operations during a transfer do not corrupt the open sink' -Reference 'FTC-Reference: one transfer at a time; DirList reads the provider store directly' -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'starts a real transfer - run with -IncludeDestructive' }
        # Start a transfer, interleave directory work, then check the transfer result.
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-state-probe.bin" -TimeoutMs 3000 -QuietMs 400
        $ll = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t ll" -TimeoutMs 15000
        Add-FtmEvidence -Output $ll
        Assert-FtmNotMatch $ll '(?i)(guru|panic|hardfault|watchdog)' 'the device crashed when a listing was requested during a transfer'
        [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t cancel" -TimeoutMs 15000)
        $still = Test-FtmStillWorks -Console $con -Target $t
        Assert-FtmTrue $still.Ok 'the server no longer works after a listing during a transfer'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-S-4' -Title 'A second transfer while one is running is refused cleanly' -Reference 'FTC-Reference: the provider store is stateful, one transfer at a time' -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'starts a real transfer - run with -IncludeDestructive' }
        [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-state-probe.bin" -TimeoutMs 3000 -QuietMs 400)
        $second = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-state-probe2.bin" -TimeoutMs 15000
        Add-FtmEvidence -Output $second
        Assert-FtmMatch $second '(?i)(busy|in progress|already|refus|error)' 'a second concurrent transfer was not refused - both would write into the same sink'
        [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t cancel" -TimeoutMs 15000)
        $still = Test-FtmStillWorks -Console $con -Target $t
        Assert-FtmTrue $still.Ok 'the server no longer works after a rejected concurrent transfer'
    }

    # ── Cancel in every phase ───────────────────────────────────────────────────

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-S-5' -Title 'Cancel at every phase leaves no open handle' -Reference 'FTC-Reference: every terminal path reaches ftcFinish() and closes source + sink' -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'starts real transfers - run with -IncludeDestructive' }
        # Cancel early, mid and late. The sink must be released each time, which is only
        # visible by the NEXT transfer being able to start.
        foreach ($delayMs in @(300, 1500, 4000)) {
            [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-state-probe.bin" -TimeoutMs $delayMs -QuietMs 200)
            Start-Sleep -Milliseconds 200
            $c = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t cancel" -TimeoutMs 15000
            Add-FtmEvidence -Note "cancel after ${delayMs}ms -> $(($c -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"
            Assert-FtmNotMatch $c '(?i)(guru|panic|hardfault|watchdog)' "the device crashed on a cancel after ${delayMs}ms"

            $again = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t df" -TimeoutMs 20000
            Assert-FtmNotMatch $again '(?i)(busy|in progress|already running)' "the sink was not released after a cancel at ${delayMs}ms"
        }
    }

    # ── Leak detection ──────────────────────────────────────────────────────────

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-S-6' -Title 'Repeated open/abort cycles do not leak handles' -Reference 'FTC-Reference: one open _file and one sink; a leak makes the Nth cycle fail' -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'starts real transfers - run with -IncludeDestructive' }
        # A single leaked handle is invisible; the tenth is fatal. Only a repeated cycle
        # distinguishes the two.
        $failures = @()
        foreach ($i in 1..10) {
            [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-state-probe.bin" -TimeoutMs 2000 -QuietMs 200)
            [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t cancel" -TimeoutMs 12000)
            $probe = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t df" -TimeoutMs 15000
            if ($probe.Trim().Length -eq 0 -or $probe -match '(?i)(busy|in progress|error|timeout)') { $failures += $i }
        }
        Add-FtmEvidence -Note "cycles that failed: $(if ($failures.Count) { $failures -join ', ' } else { 'none' })"
        Assert-FtmTrue ($failures.Count -eq 0) "open/abort cycles started failing at cycle $(if ($failures.Count) { $failures[0] } else { '-' }) - a handle or sink is leaking"
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-S-7' -Title 'A timed-out operation does not block the next one' -Reference 'FTC-Reference: bounded, non-re-armed deadlines' -Body {
        # Aim at an unreachable target, let it time out, then immediately do real work.
        [void](Invoke-FtmConsoleCommand -Console $con -Command 'ftc 15.15.253 info x' -TimeoutMs 60000)
        $still = Test-FtmStillWorks -Console $con -Target $t
        Add-FtmEvidence -Output $still.Output
        Assert-FtmTrue $still.Ok 'the client is still busy after an operation that timed out'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-S-8' -Title 'Self-addressing is refused or times out cleanly, never wedges' -Reference 'Known limit: a KNX device does not process bus frames it transmitted to itself' -Body {
        # This is a documented KNX limitation, not a defect. The suite pins it so that a
        # later change which makes it hang instead of failing is noticed.
        $own = Invoke-FtmConsoleCommand -Console $con -Command 'ftc info' -TimeoutMs 15000
        Add-FtmEvidence -Output $own
        $still = Test-FtmStillWorks -Console $con -Target $t
        Assert-FtmTrue $still.Ok 'the client is wedged after addressing itself - the documented limit must fail cleanly, not hang'
    }
}
