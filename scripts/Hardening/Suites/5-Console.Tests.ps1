#!/usr/bin/env pwsh
<#
Open ■
┬────┴  5-Console.Tests
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: scripts/Hardening/Suites/5-Console.Tests.ps1

.SYNOPSIS
    F-C - the console tunnel on object 160.

.DESCRIPTION
    Dot-sourced by Invoke-FtmHardening.ps1, which then calls Invoke-FtmSuiteConsole.

    Object 160 is a separate session from the file transfer: PID_IN (1) parks a line or a
    control code, PID_OUT (2) drains a bounded log ring. The dispatch only parks and
    copies at most 247 octets; the command itself runs later in conLoop() under
    freeLoopTime(), which is what keeps it non-blocking.

    Two known properties are pinned here rather than left to chance:

      * The log ring is 4096 octets and is SHARED with normal device logging. A single
        `help` output larger than that overflows it. The device reports the overflow once
        via _conOverflow. Silent truncation is the failure: a user reading a truncated
        help has no way to know something is missing.

      * Serial and the web console coexist through Console::submitLine(). Both must be
        able to drive the same console without one starving or corrupting the other.
#>

Set-StrictMode -Version Latest

function Invoke-FtmSuiteConsole {
    param([Parameter(Mandatory)]$Ctx, [string]$SuiteTitle = 'F-C Console tunnel (object 160)')

    $F = Get-FtmConstants
    $con = $Ctx.Console
    $t = $Ctx.Target

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-C-1' -Title 'A line longer than one APDU is bounded, not overrun' -Reference "FTC-Console: PID_IN copies at most $($F.Limit.APDU_PAYLOAD_MAX) octets" -Body {
        # The dispatch copies into a fixed buffer, so an over-long line must be cut or
        # refused at a defined boundary - never written past the buffer.
        $long = 'x' * 400
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t con $long" -TimeoutMs 20000
        Add-FtmEvidence -Output $out -Note "line length $($long.Length)"
        Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog|stack)' 'the device crashed on an over-long console line'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the console stopped answering after an over-long line'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-C-2' -Title 'Exactly one APDU of console input is handled' -Reference "FTC-Console: the boundary case at $($F.Limit.APDU_PAYLOAD_MAX) octets" -Body {
        # The boundary itself, not just over it - an off-by-one here is a real overrun.
        $exact = 'y' * ($F.Limit.APDU_PAYLOAD_MAX - 8)
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t con $exact" -TimeoutMs 20000
        Add-FtmEvidence -Note "line length $($exact.Length)"
        Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog|stack)' 'the device crashed at the APDU boundary'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the console stopped answering at the APDU boundary'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-C-3' -Title 'Ring overflow is reported, not silently truncated' -Reference "FTC-Console: shared $($F.Limit.CONSOLE_RING) octet log ring, _conOverflow reports once" -Body {
        # `help` is the documented way to exceed the ring. What must NOT happen is output
        # that simply stops with no indication - a reader cannot tell that from the end.
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t con help" -TimeoutMs 40000 -QuietMs 2500
        Add-FtmEvidence -Output $out -Note "captured $($out.Length) characters"
        Assert-FtmTrue ($out.Trim().Length -gt 0) 'no console output at all for help over the tunnel'
        if ($out.Length -ge $F.Limit.CONSOLE_RING) {
            Assert-FtmMatch $out '(?i)(overflow|truncat|lost|dropped)' 'the output exceeded the log ring but nothing reported the overflow - truncation is invisible to the reader'
        }
        else {
            Add-FtmEvidence -Note 'output stayed below the ring size; the overflow path was not exercised'
        }
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the console stopped answering after the help burst'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-C-4' -Title 'Draining output without an open session is harmless' -Reference 'FTC-Console: PID_OUT drains the ring' -Body {
        foreach ($i in 1..5) {
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t con" -TimeoutMs 12000 -QuietMs 500
            Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' "the device crashed draining output without a session (attempt $i)"
        }
        Add-FtmEvidence -Note '5 drain attempts without an open session'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the console stopped answering after drains without a session'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-C-5' -Title 'Unknown control codes on PID_IN are ignored' -Reference 'FTC-Console: PID_IN parks a line or a control code' -Body {
        foreach ($c in @('con `e[999~', "con `t`t`t", 'con ' + [char]0x01 + [char]0x02, 'con --')) {
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t $c" -TimeoutMs 12000 -QuietMs 500
            Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' "the device crashed on the control sequence: $c"
        }
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the console stopped answering after unknown control codes'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-C-6' -Title 'A remote console session ends cleanly and releases the tunnel' -Reference 'FTC-Console: the session is separate from the file transfer' -Body {
        $open = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t con version" -TimeoutMs 25000
        Add-FtmEvidence -Output $open
        # After a console session the FILE transfer path must still work - the two sessions
        # are separate, and one must not consume the other's state.
        $df = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t df" -TimeoutMs 20000
        Add-FtmEvidence -Note "df after console: $(($df -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"
        Assert-FtmTrue ($df.Trim().Length -gt 0) 'the file transfer path stopped working after a console session'
        Assert-FtmNotMatch $df '(?i)(busy|in progress)' 'the console session left the file transfer path marked busy'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-C-7' -Title 'Rapid console commands do not starve the device' -Reference 'FTC-Console: conLoop() runs under freeLoopTime()' -Body {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        foreach ($i in 1..15) {
            [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t con version" -TimeoutMs 8000 -QuietMs 300)
        }
        $sw.Stop()
        Add-FtmEvidence -Note "15 console round trips in $([Math]::Round($sw.Elapsed.TotalSeconds,1)) s"
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the device stopped answering during rapid console commands'
        $df = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t df" -TimeoutMs 20000
        Assert-FtmTrue ($df.Trim().Length -gt 0) 'the device no longer serves file commands after rapid console traffic'
    }
}
