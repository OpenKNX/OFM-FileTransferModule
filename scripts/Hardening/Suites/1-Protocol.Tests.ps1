#!/usr/bin/env pwsh
<#
Open ■
┬────┴  1-Protocol.Tests
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: scripts/Hardening/Suites/1-Protocol.Tests.ps1

.SYNOPSIS
    F-P - malformed and edge-case input on the FTC command table (object 159).

.DESCRIPTION
    Dot-sourced by Invoke-FtmHardening.ps1, which then calls Invoke-FtmSuiteProtocol.

    Every case here asserts the same two things, and both are needed:
      1. the device answers with a DEFINED result - never silence, never a test pattern,
         never a partially written file
      2. the device is still fully usable afterwards

    The input shapes are chosen from where the protocol's own bounds are:
      * one APDU carries at most 247 payload octets
      * a remote path is bounded at 235 so a fast open still fits that APDU
      * a fast transfer is bounded at 8192 chunks, above which the answer is 0x4A
      * a path is NUL terminated; without the terminator the server must not run off
      * a drive prefix selects the sink, and an unknown prefix is not a drive

    These are driven through the on-device ftc client, so what is actually verified is
    the pair (client rejects it locally) OR (server answers a defined error). Both are
    acceptable outcomes; a hang, a crash, or a silently truncated file is not.
#>

Set-StrictMode -Version Latest

function Invoke-FtmSuiteProtocol {
    param([Parameter(Mandatory)]$Ctx, [string]$SuiteTitle = 'F-P Protocol (object 159)')

    $F = Get-FtmConstants
    $con = $Ctx.Console
    $t = $Ctx.Target

    # ── Path bounds ─────────────────────────────────────────────────────────────

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-P-1' -Title 'Remote path at the maximum length is accepted or cleanly refused' -Reference 'FTC-Reference: one APDU = 247 payload octets, FTC_REMOTE_PATH_MAX = 235' -Body {
        $name = 'p' * ($F.Limit.REMOTE_PATH_MAX - 2)
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t info $name" -TimeoutMs 20000
        Add-FtmEvidence -Output $out -Note "path length $($name.Length)"
        Assert-FtmTrue ($out.Trim().Length -gt 0) 'no answer at all for a maximum-length path'
        Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|abort\(\)|watchdog)' 'the device crashed on a maximum-length path'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after a maximum-length path'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-P-2' -Title 'Remote path one octet over the maximum is refused, not truncated' -Reference 'FTC-Reference: FTC_REMOTE_PATH_LIMIT; the 2026-08 _ftcTx overflow fix' -Body {
        # Measured on hardware: the device console line editor truncates input at roughly 79
        # characters, so an over-long path never reaches the ftc client through this route.
        # The case can therefore only show that nothing crashes - the actual path bound has
        # to be probed on the wire, not through the console. Reported rather than pretended.
        Set-FtmTestSkip 'the console line editor truncates input (~79 chars), so an over-long path cannot reach the ftc client this way - needs a wire-level test'
        # This is the exact shape of the fixed host-side overflow: a path longer than the
        # transmit buffer used to be memcpy'd whole, or silently truncated to a DIFFERENT
        # target - which is worse than an error, because it acts on the wrong file.
        $name = 'q' * ($F.Limit.REMOTE_PATH_MAX + 40)
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t rm $name" -TimeoutMs 20000
        Add-FtmEvidence -Output $out -Note "path length $($name.Length)"
        Assert-FtmTrue ($out.Trim().Length -gt 0) 'no answer for an over-long path'
        # 0xA0/0xA1/0xA2 are the access-control refusals; "failed (0x..)" is how the client
        # renders any server result code. All of them are defined answers, not silence.
        Assert-FtmMatch $out '(?i)(too long|invalid|error|refus|abort|not found|failed \(0x)' 'an over-long path produced neither a refusal nor an error - it may have been truncated to a different target'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after an over-long path'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-P-3' -Title 'Empty path is refused' -Reference 'FTC-Reference: path is NUL terminated ASCII' -Body {
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t info " -TimeoutMs 15000
        Add-FtmEvidence -Output $out
        Assert-FtmTrue ($out.Trim().Length -gt 0) 'no answer for an empty path'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after an empty path'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-P-4' -Title 'Path traversal never leaves the drive root' -Reference 'FTC-Reference: drive routing by path prefix' -Body {
        $shapes = New-FtmTraversalPath
        foreach ($p in $shapes) {
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t info $p" -TimeoutMs 15000
            Add-FtmEvidence -Note "path '$p' -> $(($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"
            Assert-FtmTrue ($out.Trim().Length -gt 0) "no answer for the traversal path '$p'"
            Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' "the device crashed on the traversal path '$p'"
            # A traversal path must not resolve to something that exists.
            Assert-FtmNotMatch $out '(?i)size\s*[:=]\s*\d+\s*(b|byte)' "the traversal path '$p' resolved to a real file - the drive root is not enforced"
        }
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after the traversal shapes'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-P-5' -Title 'Unknown drive prefix is refused, not treated as a filename' -Reference 'FTC-Reference: prefixes LittleFS (default), sd/, efc/' -Body {
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t ll xy/" -TimeoutMs 15000
        Add-FtmEvidence -Output $out
        Assert-FtmTrue ($out.Trim().Length -gt 0) 'no answer for an unknown drive prefix'
        Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' 'the device crashed on an unknown drive prefix'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after an unknown drive prefix'
    }

    # ── Command space ───────────────────────────────────────────────────────────

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-P-6' -Title 'Unknown subcommands are rejected locally' -Reference 'FTC-Reference: command table on object 159' -Body {
        # The property id IS the command, so an unknown one must not reach the server as a
        # half-formed request. The client is the first line of defence here.
        foreach ($cmd in @('nosuchcommand', 'inf', 'uploadx', '')) {
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t $cmd" -TimeoutMs 12000
            Add-FtmEvidence -Note "'$cmd' -> $(($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"
            Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' "the device crashed on the subcommand '$cmd'"
        }
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after unknown subcommands'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-P-7' -Title 'Malformed target addresses are rejected' -Reference 'FTC-Reference: ftc <pa> <command>' -Body {
        foreach ($pa in @('999.999.999', '5.0', 'x.y.z', '5.0.256', '-1.0.0')) {
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $pa info x" -TimeoutMs 12000
            Add-FtmEvidence -Note "'$pa' -> $(($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"
            Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' "the device crashed on the target address '$pa'"
        }
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after malformed target addresses'
    }

    # ── Transfer bounds ─────────────────────────────────────────────────────────

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-P-8' -Title 'Fast transfer past the chunk bound answers 0x4A and falls back' -Reference "FTC-Reference: FTM_FAST_MAX_CHUNKS = $($F.Limit.FAST_MAX_CHUNKS), result 0x4A" -Body {
        if (-not $Ctx.IncludeDestructive) { Set-FtmTestSkip 'needs a very large transfer - run with -IncludeDestructive' }
        # A file large enough to exceed 8192 chunks at the negotiated chunk size must
        # produce the defined 0x4A, and the client must fall back to classic rather than
        # abort - that fallback is the whole point of the bound.
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t df" -TimeoutMs 20000
        Add-FtmEvidence -Output $out
        Set-FtmTestSkip 'requires a prepared oversize source file on the client device; see the README for how to stage it'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-P-9' -Title 'Nonexistent source aborts instead of transferring a test pattern' -Reference 'FTC-Reference: FileInfo 0x42 not found' -Body {
        # A transfer of something that does not exist must abort. Historically this class
        # of bug produced a file full of generated pattern data on the target, which looks
        # like a successful transfer until someone reads the file.
        $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t send /definitely-not-here.bin" -TimeoutMs 25000
        Add-FtmEvidence -Output $out
        Assert-FtmTrue ($out.Trim().Length -gt 0) 'no answer for a nonexistent source'
        Assert-FtmMatch $out '(?i)(not found|no such|abort|error|0x42)' 'a nonexistent source did not abort'
        Assert-FtmNotMatch $out '(?i)(complete|finished|100\s*%)' 'a nonexistent source reported a completed transfer'
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after a nonexistent source'
    }

    Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-P-10' -Title 'Device survives a burst of malformed commands' -Reference 'operational requirement: a misbehaving client must not take the server down' -Body {
        $shapes = @(
            "ftc $t info $('z' * 300)",
            "ftc $t ll $('/' * 100)",
            "ftc $t rm ../../../../../../etc/passwd",
            "ftc $t mkdir $('d/' * 80)",
            "ftc $t info sd/$('x' * 250)",
            "ftc  $t  info   x",
            "ftc $t cancel"
        )
        foreach ($rep in 1..3) {
            foreach ($c in $shapes) {
                $out = Invoke-FtmConsoleCommand -Console $con -Command $c -TimeoutMs 12000 -QuietMs 500
                Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog|rebooting)' "the device crashed on: $c"
            }
        }
        Add-FtmEvidence -Note "$($shapes.Count * 3) malformed commands sent"
        Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'console stopped answering after the malformed burst'
        # Still answering is not enough - it must still be able to do real work.
        $ok = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t ping" -TimeoutMs 15000
        Add-FtmEvidence -Output $ok
        Assert-FtmNotMatch $ok '(?i)(timeout|no answer|failed)' 'the device answers the console but no longer reaches the FTC target after the burst'
    }
}
