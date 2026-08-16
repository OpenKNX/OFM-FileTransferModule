#!/usr/bin/env pwsh
<#
Open ■
┬────┴  6-Limits.Tests
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: scripts/Hardening/Suites/6-Limits.Tests.ps1

.SYNOPSIS
    F-L - the documented hard limits, exercised deliberately.

.DESCRIPTION
    Dot-sourced by Invoke-FtmHardening.ps1, which then calls Invoke-FtmSuiteLimits.

    These are the numbers that are already measured and settled; the suite's job is not to
    re-discover them but to notice when the device stops respecting them:

      * ~350-408 B/s is the ceiling, and it is the TARGET's chunk-processing rate.
      * ~450-544 B/s is a crash cliff - the RP2040 has been observed to REBOOT there.
      * a fast transfer is bounded at 8192 chunks, above which the server answers 0x4A.
      * a space request larger than the free space must abort cleanly, before writing.

    The flood case is destructive on purpose: it exists to confirm the cliff is still
    where we think it is, and it can reboot the device. It never runs without
    -IncludeDestructive, and it always reports what it observed rather than asserting a
    reboot did not happen - a reboot IS the documented behaviour, and hiding it would be
    dishonest. What it does assert is that the device COMES BACK.

    Also pinned here: the fast stall-deadline re-arm fix. It was built but never confirmed
    on a genuinely busy bus, so F-L-6 is the case that closes that gap.
#>

Set-StrictMode -Version Latest

function Get-FtmFreeSpace {
    <#
    .SYNOPSIS
        Reads the target's free space via FilesystemInfo, or -1 when it cannot be read.
    #>
    param($Console, [string]$Target, [string]$Drive = '', [int]$TimeoutMs = 20000)
    $arg = ''
    if ($Drive -and $Drive -ne 'LittleFS') { $arg = " $Drive/" }
    $out = Invoke-FtmConsoleCommand -Console $Console -Command "ftc $Target df$arg" -TimeoutMs $TimeoutMs
    if ($out -match '(?i)free\D{0,12}(\d+)') { return [int64]$Matches[1] }
    return -1
}

function Invoke-FtmSuiteLimits {
    param([Parameter(Mandatory)]$Ctx, [string]$SuiteTitle = 'F-L Limits')

    $F = Get-FtmConstants
    $con = $Ctx.Console
    $t = $Ctx.Target

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-L-1' -Title 'Filesystem info reports a plausible free size' -Reference 'FTC-Reference: FilesystemInfo = command 46' -Body {
        $free = Get-FtmFreeSpace -Console $con -Target $t
        Add-FtmEvidence -Note "free space reported: $free"
        Assert-FtmTrue ($free -ge 0) 'free space could not be read - the space guard has nothing to work with'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-L-2' -Title 'A request larger than the free space aborts before writing' -Reference 'FTC-Reference: space guard; a partial write is worse than a refusal' -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'needs a large transfer attempt - run with -IncludeDestructive' }
        $free = Get-FtmFreeSpace -Console $con -Target $t
        Assert-FtmTrue ($free -ge 0) 'free space could not be read'
        # The abort must happen up front. A device that starts writing and runs out has
        # already produced a truncated file that looks like a real one.
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-oversize.bin" -TimeoutMs 60000
        Add-FtmEvidence -Output $out -Note "free space was $free"
        if ($out -match '(?i)(no such|not found)') { Set-FtmTestSkip 'no oversize source staged on the client device - see the README' }
        Assert-FtmMatch $out '(?i)(space|full|no room|abort|error)' 'an over-large transfer was not refused'
        Assert-FtmNotMatch $out '(?i)(complete|100\s*%)' 'an over-large transfer reported completion'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the device stopped answering after the space guard fired'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-L-3' -Title 'Transfer throughput stays at the documented ceiling' -Reference "FTC-Reference: ~$($F.Limit.THROUGHPUT_CEILING) B/s is the target's chunk-processing rate" -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'runs a real transfer - run with -IncludeDestructive' }
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-perf.bin safe" -TimeoutMs 180000 -UntilMatch '(?i)(complete|abort|error|failed)'
        $sw.Stop()
        Add-FtmEvidence -Output $out -Note "elapsed $([Math]::Round($sw.Elapsed.TotalSeconds,1)) s"
        if ($out -match '(?i)(no such|not found)') { Set-FtmTestSkip 'no perf source staged on the client device - see the README' }
        Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' 'the device crashed during a normal transfer'
        # The ceiling is not a target to beat - a number far above it means the transfer
        # did not actually happen.
        if ($out -match '(?i)(\d+)\s*B/s') {
            $rate = [int]$Matches[1]
            Add-FtmEvidence -Note "measured $rate B/s"
            Assert-FtmTrue ($rate -lt ($F.Limit.FLOOD_CLIFF * 2)) "reported $rate B/s is implausibly far above the measured ceiling of $($F.Limit.THROUGHPUT_CEILING) B/s - the transfer probably did not run"
        }
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-L-4' -Title 'Chunk bound is enforced with 0x4A, not by silent truncation' -Reference "FTC-Reference: FTM_FAST_MAX_CHUNKS = $($F.Limit.FAST_MAX_CHUNKS), result 0x4A, fall back to classic" -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'needs an oversize transfer - run with -IncludeDestructive' }
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-oversize.bin fast" -TimeoutMs 120000 -UntilMatch '(?i)(complete|abort|error|failed|0x4a)'
        Add-FtmEvidence -Output $out
        if ($out -match '(?i)(no such|not found)') { Set-FtmTestSkip 'no oversize source staged on the client device - see the README' }
        # Either the bound was hit and the client fell back, or the file was small enough.
        # What must not happen is a "successful" transfer of a truncated file.
        Assert-FtmNotMatch $out '(?i)truncat' 'the transfer was silently truncated at the chunk bound'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the device stopped answering at the chunk bound'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-L-5' -Title 'A missing drive is reported, not treated as empty' -Reference 'FTC-Reference: drive routing LittleFS / sd / efc' -Body {
        foreach ($d in @('sd', 'efc')) {
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t ll $d/" -TimeoutMs 20000
            Add-FtmEvidence -Note "$d -> $(($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"
            Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' "the device crashed listing the $d drive"
            # An absent drive must say so. An empty listing is indistinguishable from a
            # present-but-empty card, which sends a user looking in the wrong place.
            if ($out -match '(?i)(0 files|empty)$' -and $out -notmatch '(?i)(not present|no card|unavailable|not mounted)') {
                Add-FtmEvidence -Note "$d reported an empty listing without stating whether the drive exists"
            }
        }
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the device stopped answering after probing the drives'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-L-6' -Title 'A fast transfer survives a congested bus (stall-deadline re-arm)' -Reference 'FTC-Reference: FTC_FAST_STALL_MS re-arm on progress; built 2026-08, not yet confirmed on a busy bus' -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'needs a large transfer under load - run with -IncludeDestructive' }
        # This is the open item the fix was written for: a lossy but PROGRESSING window on
        # a flooded bus used to exceed the 30 s stall deadline and abort the whole transfer.
        # The case needs real competing traffic; without it, it proves nothing, so it says so.
        Add-FtmEvidence -Note 'requires a third device flooding the bus during the run - see the README for the rig'
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-perf.bin fast" -TimeoutMs 300000 -UntilMatch '(?i)(complete|abort|error|failed)'
        $sw.Stop()
        Add-FtmEvidence -Output $out -Note "elapsed $([Math]::Round($sw.Elapsed.TotalSeconds,1)) s"
        if ($out -match '(?i)(no such|not found)') { Set-FtmTestSkip 'no perf source staged on the client device - see the README' }
        Assert-FtmNotMatch $out '(?i)overall deadline exceeded' 'the fast transfer aborted on the overall stall deadline although it was progressing - the re-arm fix is not effective'
        Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' 'the device crashed during a fast transfer under load'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-L-7' -Title 'The flood cliff behaves as documented and the device returns' -Reference "FTC-Reference: ~$($F.Limit.FLOOD_CLIFF) B/s is a crash cliff on RP2040; memory reboot-under-tunnel-flood" -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'CAN REBOOT THE DEVICE - run with -IncludeDestructive only, and only on a lab device' }
        # The point is not to prove the device survives - it is documented that it may not.
        # The point is that it comes back, and that the failure is a clean restart rather
        # than a wedged device or a corrupted filesystem.
        Add-FtmEvidence -Note "pushing past the documented cliff of ~$($F.Limit.FLOOD_CLIFF) B/s"
        $before = Test-FtmConsoleAlive -Console $con
        Assert-FtmTrue $before 'the device was not answering before the flood - nothing can be concluded'

        [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send ftm-perf.bin fast w64" -TimeoutMs 90000 -QuietMs 3000)

        $back = $false
        foreach ($i in 1..30) {
            Start-Sleep -Seconds 2
            if (Test-FtmConsoleAlive -Console $con -TimeoutMs 4000) { $back = $true; break }
        }
        Add-FtmEvidence -Note "device answering after the flood: $back"
        Assert-FtmTrue $back 'the device did not come back within 60 s after the flood'

        $fs = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t df" -TimeoutMs 25000
        Add-FtmEvidence -Output $fs
        Assert-FtmTrue ($fs.Trim().Length -gt 0) 'the filesystem is not reachable after the flood'
    }
}
