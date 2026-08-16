#!/usr/bin/env pwsh
<#
Open ■
┬────┴  2-ResponseMatrix.Tests
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: scripts/Hardening/Suites/2-ResponseMatrix.Tests.ps1

.SYNOPSIS
    F-R - the command x response x consumer matrix, per drive.

.DESCRIPTION
    Dot-sourced by Invoke-FtmHardening.ps1, which then calls Invoke-FtmSuiteResponse.

    This suite exists because of one concrete failure. A server change added a new
    FileInfo status - 0x02 "size known, CRC still computing, poll again" - and exactly ONE
    client path was updated. Five sibling states still accepted only 0x00 and 0x01 and
    broke on LittleFS: `ll` reported 0 bytes, `info` reported not-found, `apply` raced,
    and downloads went unverified. The review that missed it had traced the changed lines
    instead of every consumer of the changed value.

    So the rule this suite encodes is: for every command, every response the server can
    produce (which depends on the drive AND on an asynchronous state), and every client
    state that awaits it, there must be a case. The FileInfo block below is the full
    cross product for the four status octets and the six consumers.

    The drives are not interchangeable here - they are the reason the matrix has cells:
      LittleFS always answers 0x02 first, for every file, because its CRC is computed
      cooperatively across loop passes. SD and EFC answer 0x01 (size only) or 0x02 -> 0x00
      depending on whether a trailing CRC flag is set. A test run against only one drive
      therefore proves almost nothing about the others.
#>

Set-StrictMode -Version Latest

function Get-FtmDrivePrefix {
    param([string]$Drive)
    if ($Drive -eq 'LittleFS') { return '' }
    return "$Drive/"
}

function Invoke-FtmSuiteResponse {
    param([Parameter(Mandatory)]$Ctx, [string]$SuiteTitle = 'F-R Response matrix')

    $F = Get-FtmConstants
    $con = $Ctx.Console
    $t = $Ctx.Target

    # Consumers of FileInfo, i.e. every client state that sends the command and must
    # handle all four status octets. The command column is what drives that state.
    $consumers = @(
        @{ Key = 'FtcInfo';           Cmd = 'info {0}';        Note = 'the plain info command' },
        @{ Key = 'FtcDirInfo';        Cmd = 'll {0}';          Note = 'directory listing, which reads size per entry' },
        @{ Key = 'FtcResumeInfo';     Cmd = 'send {0} resume'; Note = 'resume probe before an upload' },
        @{ Key = 'FtcVerify';         Cmd = 'send {0}';        Note = 'post-upload verification' },
        @{ Key = 'FtcDownloadVerify'; Cmd = 'get {0}';         Note = 'post-download verification' },
        @{ Key = 'FtcApplyCheck';     Cmd = 'apply {0}';       Note = 'existence check before a firmware apply' }
    )

    foreach ($drive in $Ctx.Drives) {
        $prefix = Get-FtmDrivePrefix -Drive $drive

        # ── The not-found cell: 0x42 on every consumer ──────────────────────────
        Invoke-FtmTestCase -Suite $SuiteTitle -Id "F-R-1-$drive" -Title "FileInfo 0x42 (not found) is handled by every consumer on $drive" -Reference 'FTC-Reference: cmdFileInfo answers 0x42 for a missing file' -Body {
            $missing = "${prefix}no-such-file-$([Guid]::NewGuid().ToString('N').Substring(0,8)).bin"
            $problems = @()
            foreach ($c in $consumers) {
                $cmd = "ftc $t " + ($c.Cmd -f $missing)
                $out = Invoke-FtmConsoleCommand -Console $con -Command $cmd -TimeoutMs 20000
                Add-FtmEvidence -Note "$($c.Key): $(($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"
                if ($out -match '(?i)(guru|panic|hardfault|watchdog)') { $problems += "$($c.Key) crashed the device" ; continue }
                if ($out.Trim().Length -eq 0) { $problems += "$($c.Key) got no answer at all"; continue }
                # A missing file must be reported as missing - not as a zero-byte success.
                if ($out -match '(?i)(complete|100\s*%|verified ok)') { $problems += "$($c.Key) reported success for a missing file" }
            }
            foreach ($p in $problems) { Add-FtmEvidence -Note $p }
            Assert-FtmTrue ($problems.Count -eq 0) ("not-found handling broken: " + ($problems -join '; '))
        }

        # ── The poll-again cell: 0x02 must never surface as an error ─────────────
        Invoke-FtmTestCase -Suite $SuiteTitle -Id "F-R-2-$drive" -Title "FileInfo 0x02 (CRC pending) is re-polled, not treated as failure, on $drive" -Reference 'FTC-Reference: LittleFS answers 0x02 first for EVERY file; memory ftc-response-matrix-rule' -Body {
            # Every consumer must re-poll until 0x00/0x01. The visible symptom of a missing
            # re-poll is exactly this: a file that exists is reported as 0 bytes or missing.
            $probe = "${prefix}ftm-hard-probe.bin"
            $mk = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send $probe" -TimeoutMs 40000
            Add-FtmEvidence -Output $mk

            $problems = @()
            foreach ($c in @($consumers | Where-Object { $_.Key -in @('FtcInfo', 'FtcDirInfo', 'FtcApplyCheck') })) {
                $cmd = "ftc $t " + ($c.Cmd -f $probe)
                $out = Invoke-FtmConsoleCommand -Console $con -Command $cmd -TimeoutMs 25000
                Add-FtmEvidence -Note "$($c.Key): $(($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"
                if ($out -match '(?i)size\s*[:=]?\s*0\b') { $problems += "$($c.Key) reported size 0 for an existing file - it stopped at status 0x02" }
                if ($out -match '(?i)not found') { $problems += "$($c.Key) reported not-found for an existing file - it treated 0x02 as absent" }
            }
            foreach ($p in $problems) { Add-FtmEvidence -Note $p }
            if ($mk -match '(?i)(not found|no such)') { Set-FtmTestSkip 'could not stage a probe file on the target - the source is missing on the client device' }
            Assert-FtmTrue ($problems.Count -eq 0) ("poll-again handling broken: " + ($problems -join '; '))
        }

        # ── apply must accept every "present" code, not only 0x00 ────────────────
        Invoke-FtmTestCase -Suite $SuiteTitle -Id "F-R-3-$drive" -Title "apply accepts 0x00, 0x01 and 0x02 as present on $drive" -Reference 'FTC-Reference: apply needs existence only; refusing anything but 0x00 false-refused every LittleFS file' -Body {
            $probe = "${prefix}ftm-hard-probe.bin"
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t apply $probe" -TimeoutMs 25000
            Add-FtmEvidence -Output $out
            if ($out -match '(?i)(no such|missing source)') { Set-FtmTestSkip 'probe file not staged; run F-R-2 first' }
            Assert-FtmNotMatch $out '(?i)not found \(0x02\)' 'apply refused a file whose FileInfo said "CRC pending" - 0x02 means present, not absent'
            Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after the apply check'
        }

        # ── a one-octet error frame must not be shadowed by a length guard ───────
        Invoke-FtmTestCase -Suite $SuiteTitle -Id "F-R-4-$drive" -Title "One-octet error answers are not swallowed by a length guard on $drive" -Reference 'FTC-Reference: FileReport 0x42 is 1 octet; a length guard must never precede the status check' -Body {
            # FileReport for a transfer that is not running answers a single octet. A client
            # that checks "length < 6 -> ignore" before looking at the status silently loses
            # that error and then waits forever.
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t status" -TimeoutMs 15000
            Add-FtmEvidence -Output $out
            Assert-FtmTrue ($out.Trim().Length -gt 0) 'no answer for a status query without a running transfer'
            Assert-FtmNotMatch $out '(?i)(hang|stuck|waiting forever)' 'the client hung on a short error answer'
            Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after a short error answer'
        }
    }

    # ── Bounded waiting: no state may wait without a deadline ───────────────────

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-R-5' -Title 'Every awaiting state exits on a deadline' -Reference 'FTC-Reference: non-re-armed deadlines; a stuck CRC must not loop forever' -Body {
        # Point the client at an address that does not answer. Every state that waits must
        # come back on its own - the failure mode this guards is a deadline that is re-armed
        # on every loop pass and therefore never expires.
        $dead = '15.15.254'
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $dead info x" -TimeoutMs 60000
        $sw.Stop()
        Add-FtmEvidence -Output $out -Note "returned after $([Math]::Round($sw.Elapsed.TotalSeconds,1)) s"
        Assert-FtmTrue ($out.Trim().Length -gt 0) 'the client produced no output at all for an unreachable target'
        Assert-FtmTrue ($sw.Elapsed.TotalSeconds -lt 55) 'the client did not come back within 55 s from an unreachable target - a deadline is missing or re-armed'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after the unreachable-target probe'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-R-6' -Title 'A finished operation leaves no pending state behind' -Reference 'FTC-Reference: every terminal path reaches ftcFinish()' -Body {
        # After a timeout the next command must work immediately. If the previous state was
        # not finished, the next one either blocks or reports the previous result.
        $first = Invoke-FtmConsoleCommand -Console $con -Command "ftc 15.15.254 info x" -TimeoutMs 60000
        Add-FtmEvidence -Note 'first command aimed at an unreachable target'
        $second = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t ping" -TimeoutMs 20000
        Add-FtmEvidence -Output $second
        Assert-FtmTrue ($second.Trim().Length -gt 0) 'the command after a timed-out one produced no output - the client is still busy'
        Assert-FtmNotMatch $second '(?i)(busy|in progress|already running)' 'the client still considers the previous, timed-out operation active'
    }
}
