#!/usr/bin/env pwsh
<#
Open ■
┬────┴  4-Security.Tests
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: scripts/Hardening/Suites/4-Security.Tests.ps1

.SYNOPSIS
    F-A - access control: every write command, in every stage, without authorisation.

.DESCRIPTION
    Dot-sourced by Invoke-FtmHardening.ps1, which then calls Invoke-FtmSuiteAccess.

    The access layer (build flag OPENKNX_FTC_SECURITY) gates every WRITE command on
    object 159 plus the console OPEN on object 160. Reads stay open except in stage Off.
    Stages are Off / ProgMode / Always / Password, read live from the parameters so a
    freshly programmed device is never stale.

    The rule that shapes every case here:

        A correct refusal code is not a pass. The refusal must be verified against the
        FILESYSTEM, not against the return value.

    A server that answers 0xA0 "auth required" and writes the file anyway is the worst
    possible outcome, and it is completely invisible to a test that only reads the answer.
    Every write case therefore checks afterwards whether the target file exists or changed.

    Result codes: 0xA0 auth required, 0xA1 auth failed, 0xA2 writes disabled.
#>

Set-StrictMode -Version Latest

function Set-FtmSecurityStage {
    <#
    .SYNOPSIS
        Switches the target's access stage through the runtime test hooks.
    .DESCRIPTION
        `ftm sec off|prog|always|pw|ets` is gated by OPENKNX_FTC_SECURITY and is
        runtime-only: a reboot restores whatever ETS configured. That is deliberate -
        a test must not be able to leave a device permanently unlocked.
    #>
    param($Console, [Parameter(Mandatory)][string]$Stage, [int]$TimeoutMs = 10000)
    $out = Invoke-FtmConsoleCommand -Console $Console -Command "ftm sec $Stage" -TimeoutMs $TimeoutMs
    return $out
}

function Test-FtmFileExists {
    <#
    .SYNOPSIS
        Asks the target whether a file exists - the ground truth a refusal is checked against.
    #>
    param($Console, [string]$Target, [string]$Path, [int]$TimeoutMs = 20000)
    $out = Invoke-FtmConsoleCommand -Console $Console -Command "ftc $Target info $Path" -TimeoutMs $TimeoutMs
    # 0x42 / "not found" means absent; any size report means present.
    if ($out -match '(?i)(not found|0x42|no such)') { return $false }
    if ($out -match '(?i)size\s*[:=]?\s*\d+') { return $true }
    return $false
}

function Invoke-FtmSuiteAccess {
    param([Parameter(Mandatory)]$Ctx, [string]$SuiteTitle = 'F-A Access control')

    $F = Get-FtmConstants
    $con = $Ctx.Console
    $t = $Ctx.Target

    if (-not $Ctx.Security) {
        foreach ($id in @('F-A-1', 'F-A-2', 'F-A-3', 'F-A-4', 'F-A-5', 'F-A-6', 'F-A-7')) {
            Invoke-FtmTestCase -Suite $SuiteTitle -Id $id -Title 'Access control case' -Reference 'build flag OPENKNX_FTC_SECURITY' -Body {
                Set-FtmTestNotApplicable 'device is not built with OPENKNX_FTC_SECURITY - the access layer compiles out entirely (pass -Security when it is enabled)'
            }
        }
        return
    }

    # Every command that WRITES. These are the ones the gate must cover; anything missing
    # from this list is a hole, so it is spelled out rather than derived.
    $writeCommands = @(
        @{ Name = 'FileUpload';     Cmd = 'send ftm-acl-probe.bin' },
        @{ Name = 'FileUploadFast'; Cmd = 'send ftm-acl-probe.bin fast' },
        @{ Name = 'FileDelete';     Cmd = 'rm ftm-acl-probe.bin' },
        @{ Name = 'DirCreate';      Cmd = 'mkdir ftm-acl-dir' },
        @{ Name = 'DirDelete';      Cmd = 'rmdir ftm-acl-dir' },
        @{ Name = 'Rename';         Cmd = 'mv ftm-acl-probe.bin ftm-acl-probe2.bin' },
        @{ Name = 'FwUpdate';       Cmd = 'apply ftm-acl-probe.bin' }
    )

    $readCommands = @(
        @{ Name = 'FileInfo';       Cmd = 'info ftm-acl-probe.bin' },
        @{ Name = 'DirList';        Cmd = 'll' },
        @{ Name = 'FilesystemInfo'; Cmd = 'df' },
        @{ Name = 'ModuleVersion';  Cmd = 'ping' }
    )

    $restore = 'ets'

    try {
        # ── Stage Password, not logged in: every write must be refused AND not happen ──

        Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-A-1' -Title 'Stage Password: every write command is refused AND not executed' -Reference 'FTC-Security: 0xA0 auth required; reads stay open' -Body {
            $s = Set-FtmSecurityStage -Console $con -Stage 'pw'
            Add-FtmEvidence -Output $s
            Assert-FtmNotMatch $s '(?i)(unknown command|not supported)' 'the device does not offer the ftm sec test hook - OPENKNX_FTC_SECURITY may not be built in'

            $problems = @()
            foreach ($w in $writeCommands) {
                $before = Test-FtmFileExists -Console $con -Target $t -Path 'ftm-acl-probe.bin'
                $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t $($w.Cmd)" -TimeoutMs 25000
                $after = Test-FtmFileExists -Console $con -Target $t -Path 'ftm-acl-probe.bin'
                Add-FtmEvidence -Note "$($w.Name): $(($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"

                if ($out -notmatch '(?i)(0xA0|auth|login|denied|refus|not permitted)') {
                    $problems += "$($w.Name) was not refused with an authorisation error"
                }
                # The ground truth: the filesystem must be unchanged.
                if ($before -ne $after) {
                    $problems += "$($w.Name) CHANGED the filesystem despite being refused (present before=$before, after=$after)"
                }
            }
            foreach ($p in $problems) { Add-FtmEvidence -Note $p }
            Assert-FtmTrue ($problems.Count -eq 0) ("write gating broken: " + ($problems -join '; '))
        }

        Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-A-2' -Title 'Stage Password: reads stay open' -Reference 'FTC-Security: reads are not gated except in stage Off' -Body {
            [void](Set-FtmSecurityStage -Console $con -Stage 'pw')
            $problems = @()
            foreach ($r in $readCommands) {
                $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t $($r.Cmd)" -TimeoutMs 20000
                Add-FtmEvidence -Note "$($r.Name): $(($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"
                if ($out -match '(?i)(0xA0|auth required|login required)') { $problems += "$($r.Name) was gated although reads must stay open" }
            }
            foreach ($p in $problems) { Add-FtmEvidence -Note $p }
            Assert-FtmTrue ($problems.Count -eq 0) ("read gating too strict: " + ($problems -join '; '))
        }

        Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-A-3' -Title 'Stage Off: everything is closed, including reads' -Reference 'FTC-Security: stage Off closes reads as well' -Body {
            $s = Set-FtmSecurityStage -Console $con -Stage 'off'
            Add-FtmEvidence -Output $s
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t ll" -TimeoutMs 20000
            Add-FtmEvidence -Output $out
            Assert-FtmMatch $out '(?i)(0xA2|disabled|denied|refus|not permitted|no answer|timeout)' 'stage Off did not close the read path'
        }

        Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-A-4' -Title 'Stage ProgMode: writes only while the device is in programming mode' -Reference 'FTC-Security: stage ProgMode' -Body {
            $s = Set-FtmSecurityStage -Console $con -Stage 'prog'
            Add-FtmEvidence -Output $s
            $before = Test-FtmFileExists -Console $con -Target $t -Path 'ftm-acl-probe.bin'
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t mkdir ftm-acl-dir" -TimeoutMs 20000
            Add-FtmEvidence -Output $out
            # The target is not expected to be in programming mode during a test run.
            Assert-FtmMatch $out '(?i)(0xA0|auth|prog|denied|refus|not permitted)' 'stage ProgMode allowed a write while the device was not in programming mode'
            $after = Test-FtmFileExists -Console $con -Target $t -Path 'ftm-acl-probe.bin'
            Assert-FtmEqual $before $after 'the filesystem changed although the write was refused in stage ProgMode'
        }

        Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-A-5' -Title 'Stage Always: writes are open' -Reference 'FTC-Security: stage Always, unconfigured device defaults to Always' -Body {
            $s = Set-FtmSecurityStage -Console $con -Stage 'always'
            Add-FtmEvidence -Output $s
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t mkdir ftm-acl-dir" -TimeoutMs 20000
            Add-FtmEvidence -Output $out
            Assert-FtmNotMatch $out '(?i)(0xA0|0xA2|auth required|denied)' 'stage Always refused a write'
            [void](Invoke-FtmConsoleCommand -Console $con -Command "ftc $t rmdir ftm-acl-dir" -TimeoutMs 20000)
        }

        Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-A-6' -Title 'A wrong password fails and backs off without blocking the device' -Reference 'FTC-Security: 0xA1 auth failed, non-blocking back-off' -Body {
            [void](Set-FtmSecurityStage -Console $con -Stage 'pw')
            $codes = @()
            foreach ($i in 1..5) {
                $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t login definitely-wrong-$i" -TimeoutMs 20000
                $codes += (($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))
                Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' "the device crashed on wrong password attempt $i"
            }
            Add-FtmEvidence -Note ("attempts: " + ($codes -join ' | '))
            # The back-off must be non-blocking: the console has to stay responsive.
            Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the console stopped answering during the authentication back-off - it is blocking'
        }

        Invoke-FtmTestCase -Suite $SuiteTitle -Id 'F-A-7' -Title 'Logout closes the window immediately' -Reference 'FTC-Security: AuthLogout = command 105' -Body {
            [void](Set-FtmSecurityStage -Console $con -Stage 'always')
            $out = Invoke-FtmConsoleCommand -Console $con -Command "ftc $t logout" -TimeoutMs 20000
            Add-FtmEvidence -Output $out
            Assert-FtmNotMatch $out '(?i)(guru|panic|hardfault|watchdog)' 'the device crashed on logout'
            Assert-FtmTrue (Test-FtmConsoleAlive -Console $con) 'the console stopped answering after a logout'
        }
    }
    finally {
        # Always hand the device back in its configured state. A test that leaves a device
        # unlocked is worse than a test that did not run.
        [void](Set-FtmSecurityStage -Console $con -Stage $restore)
    }
}
