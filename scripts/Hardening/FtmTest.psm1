#!/usr/bin/env pwsh
<#
Open ■
┬────┴  FtmTest
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: scripts/Hardening/FtmTest.psm1

.SYNOPSIS
    Shared library for the FTC / FTM / console hardening suites.

.DESCRIPTION
    The FTC protocol is not KNX specified: it is an OpenKNX FunctionProperty RPC carried
    on two interface objects.

      * object 159 - the file transfer command table. The property id IS the command.
      * object 160 - the console tunnel. PID_IN (1) parks a line, PID_OUT (2) drains the
        bounded log ring.

    Every operation answers in at most one APDU, so a payload can never exceed 247 octets
    (APDU minus the 7 octet header). That single number bounds most of the protocol suite.

    This library provides:
      * command and result-code tables taken from doc/FTC-Reference.md and doc/errorcodes.txt
      * builders for A_FunctionProperty_Command frames, including deliberately malformed ones
      * a serial console driver, because the on-device ftc client is driven through it
      * the same PASS / FAIL / SKIP / N-A verdict engine and report format as the
        KNXnet/IP conformance suites, so both produce comparable artefacts

    A hardening suite must be able to observe the DEVICE STATE, not only the answer code:
    a write that is correctly refused but still executed is the worst possible outcome and
    is invisible to anything that only reads the return value.

.NOTES
    PowerShell 7 (macOS/Linux/Windows) and Windows PowerShell 5.1.
    Byte arrays are returned with the comma operator so PowerShell does not unroll them.
#>

Set-StrictMode -Version Latest

if ($null -eq (Get-Variable -Name 'IsWindows' -ErrorAction SilentlyContinue)) {
    $script:IsWindows = $true
}

# ─── Protocol constants ─────────────────────────────────────────────────────────

# Interface object indices used by the FTC server.
$script:FtmObject = @{
    DATA    = 159   # file transfer command table
    CONSOLE = 160   # console tunnel
}

# Property ids on object 159 - the property id IS the command (enum class FtmCommands).
$script:FtmCommand = @{
    Format           = 0
    Exists           = 1
    Rename           = 2
    FileUpload       = 40
    FileDownload     = 41
    FileDelete       = 42
    FileInfo         = 43
    FileUploadFast   = 44
    FileReport       = 45
    FilesystemInfo   = 46
    DirList          = 80
    DirCreate        = 81
    DirDelete        = 82
    Cancel           = 90
    ModuleVersion    = 100
    FwUpdate         = 101
    CheckFeatures    = 102
    AuthChallenge    = 103
    AuthResponse     = 104
    AuthLogout       = 105
}

# Console tunnel property ids on object 160.
$script:FtmConsole = @{
    PID_IN  = 1
    PID_OUT = 2
}

# FileInfo(43) status octets. The 0x02 "poll again" code is the one that broke five
# client states when it was introduced - every consumer must handle all four.
$script:FtmFileInfo = @{
    DONE_WITH_CRC = 0x00   # + size(4) + crc(4)
    SIZE_ONLY     = 0x01   # + size(4), SD/EFC
    CRC_PENDING   = 0x02   # + size(4), poll again
    NOT_FOUND     = 0x42   # single octet
}

# Result codes the server may answer with.
$script:FtmResult = @{
    OK                 = 0x00
    NOT_FOUND          = 0x42
    TOO_MANY_CHUNKS    = 0x4A
    AUTH_REQUIRED      = 0xA0
    AUTH_FAILED        = 0xA1
    WRITES_DISABLED    = 0xA2
}

# Hard limits of the transport, from the reference documentation.
$script:FtmLimit = @{
    APDU_PAYLOAD_MAX   = 247   # one APDU minus the 7 octet header
    REMOTE_PATH_MAX    = 235   # fast-open header + path + NUL + size must fit one APDU
    FAST_MAX_CHUNKS    = 8192  # above this the server answers 0x4A
    CONSOLE_RING       = 4096  # shared log ring; a bigger burst overflows it
    THROUGHPUT_CEILING = 408   # B/s, target-side chunk processing rate
    FLOOD_CLIFF        = 450   # B/s, above this the RP2040 has been observed to reboot
}

function Get-FtmConstants {
    <#
    .SYNOPSIS
        Returns every constant table in one object, for use inside suites.
    #>
    return [pscustomobject]@{
        Object   = $script:FtmObject
        Command  = $script:FtmCommand
        Console  = $script:FtmConsole
        FileInfo = $script:FtmFileInfo
        Result   = $script:FtmResult
        Limit    = $script:FtmLimit
    }
}

function Get-FtmCommandName {
    param([int]$PropertyId)
    foreach ($k in $script:FtmCommand.Keys) { if ($script:FtmCommand[$k] -eq $PropertyId) { return $k } }
    return ('PID {0}' -f $PropertyId)
}

function Get-FtmFileInfoName {
    param([int]$Status)
    foreach ($k in $script:FtmFileInfo.Keys) { if ($script:FtmFileInfo[$k] -eq $Status) { return $k } }
    return ('0x{0:X2}' -f $Status)
}

# ─── Payload builders ───────────────────────────────────────────────────────────

function ConvertTo-FtmHex {
    param([byte[]]$Bytes, [int]$MaxBytes = 0)
    if ($null -eq $Bytes -or $Bytes.Length -eq 0) { return '<empty>' }
    $take = $Bytes.Length; $trail = ''
    if ($MaxBytes -gt 0 -and $take -gt $MaxBytes) { $take = $MaxBytes; $trail = " ... (+$($Bytes.Length - $MaxBytes) B)" }
    $sb = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $take; $i++) {
        if ($i -gt 0) { [void]$sb.Append(' ') }
        [void]$sb.Append($Bytes[$i].ToString('X2'))
    }
    return $sb.ToString() + $trail
}

function New-FtmPath {
    <#
    .SYNOPSIS
        Encodes a remote path as the protocol expects it: ASCII, NUL terminated.
    .PARAMETER NoTerminator
        Omit the NUL - for the malformed-input cases that check the server's own bound.
    #>
    param([Parameter(Mandatory)][string]$Path, [switch]$NoTerminator)
    $b = [System.Text.Encoding]::ASCII.GetBytes($Path)
    if ($NoTerminator) { return , ([byte[]]$b) }
    return , ([byte[]]($b + [byte[]]@(0x00)))
}

function New-FtmOpenPayload {
    <#
    .SYNOPSIS
        Builds the payload of a FileUpload / FileUploadFast open request.
    .DESCRIPTION
        Layout: 6 octet header, NUL terminated path, then the exact size as a little
        endian u32 (used by the SD backend to pre-allocate). Fast open plus a 235 octet
        path plus NUL plus 4 size octets is exactly the 247 octet APDU limit - which is
        where REMOTE_PATH_MAX comes from.
    #>
    param(
        [Parameter(Mandatory)][string]$Path,
        [int]$ChunkSize = 240,
        [int]$Chunks = 1,
        [int]$SizeHint = 0
    )
    $hdr = [byte[]]@(
        ($ChunkSize -band 0xFF), (($ChunkSize -shr 8) -band 0xFF),
        ($Chunks -band 0xFF), (($Chunks -shr 8) -band 0xFF),
        0x00, 0x00
    )
    $size = [byte[]]@(
        ($SizeHint -band 0xFF), (($SizeHint -shr 8) -band 0xFF),
        (($SizeHint -shr 16) -band 0xFF), (($SizeHint -shr 24) -band 0xFF)
    )
    return , ([byte[]]($hdr + (New-FtmPath -Path $Path) + $size))
}

function New-FtmChunkPayload {
    <#
    .SYNOPSIS
        Builds a data chunk: sequence number (u16 little endian) followed by the data.
    .DESCRIPTION
        The write offset is derived from the sequence as (sequence-1)*chunkSize - it is
        NOT derived from the sender, so a wrong or repeated sequence writes to the wrong
        place in the one open sink. That is exactly what the state-machine suite probes.
    #>
    param([Parameter(Mandatory)][int]$Sequence, [byte[]]$Data = @())
    return , ([byte[]]@(($Sequence -band 0xFF), (($Sequence -shr 8) -band 0xFF)) + $Data)
}

function New-FtmOversizedPayload {
    <#
    .SYNOPSIS
        Builds a payload deliberately larger than one APDU can carry.
    #>
    param([int]$Length = 260)
    $b = New-Object byte[] $Length
    for ($i = 0; $i -lt $Length; $i++) { $b[$i] = [byte]($i -band 0xFF) }
    return , $b
}

function New-FtmTraversalPath {
    <#
    .SYNOPSIS
        Returns the path shapes a file server must refuse or normalise, never follow.
    #>
    return , @(
        '../../etc/passwd',
        '..',
        '../',
        'a/../../b',
        '/absolute/path',
        'sd/../../secret',
        './././x',
        'xy/unknown-drive-file'
    )
}

# ─── Serial console driver ──────────────────────────────────────────────────────

function Open-FtmConsole {
    <#
    .SYNOPSIS
        Opens a device console over a serial port without toggling DTR/RTS.
    .DESCRIPTION
        Toggling the control lines resets a CH340 or ESP board on connect, which would
        restart the very device under test. The existing Test-Ftc*.ps1 scripts use the
        same handling; this is the shared version of it.
    #>
    param(
        [Parameter(Mandatory)][string]$Port,
        [int]$Baud = 115200,
        [switch]$Dtr,
        [int]$ReadTimeoutMs = 1200
    )
    $sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $sp.ReadTimeout = $ReadTimeoutMs
    $sp.WriteTimeout = 2000
    $sp.NewLine = "`r`n"
    if ($Dtr) { $sp.DtrEnable = $true; $sp.RtsEnable = $true }
    else { $sp.DtrEnable = $false; $sp.RtsEnable = $false }
    $sp.Open()
    Start-Sleep -Milliseconds 300
    try { $sp.DiscardInBuffer() } catch { }
    return $sp
}

function Close-FtmConsole {
    param($Console)
    if ($null -eq $Console) { return }
    try { if ($Console.IsOpen) { $Console.Close() } } catch { }
    try { $Console.Dispose() } catch { }
}

function Invoke-FtmConsoleCommand {
    <#
    .SYNOPSIS
        Sends a console line and collects output until it goes quiet or a marker appears.
    .DESCRIPTION
        FTC operations have long silent gaps (a transfer can be quiet for seconds), so a
        plain "read until idle" would cut a transfer in half. The caller therefore passes
        the marker that ends the operation, and the idle window only applies when no
        marker is given.
    #>
    param(
        [Parameter(Mandatory)]$Console,
        [Parameter(Mandatory)][string]$Command,
        [string]$UntilMatch = '',
        [int]$TimeoutMs = 15000,
        [int]$QuietMs = 900
    )
    try { $Console.DiscardInBuffer() } catch { }
    $Console.WriteLine($Command)

    $sb = New-Object System.Text.StringBuilder
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $lastData = [DateTime]::UtcNow
    while ([DateTime]::UtcNow -lt $deadline) {
        $chunk = ''
        try { $chunk = $Console.ReadExisting() } catch { $chunk = '' }
        if ($chunk) {
            [void]$sb.Append($chunk)
            $lastData = [DateTime]::UtcNow
            if ($UntilMatch -and ($sb.ToString() -match $UntilMatch)) { break }
        }
        else {
            if (-not $UntilMatch -and ([DateTime]::UtcNow - $lastData).TotalMilliseconds -gt $QuietMs) { break }
            Start-Sleep -Milliseconds 60
        }
    }
    # The device console is a line editor: it echoes every keystroke and wraps output in
    # ANSI escapes. Matching a pattern against that raw stream matches the ECHO of the
    # command as often as the answer to it - so the noise is removed before any verdict
    # is derived from the text.
    $raw = $sb.ToString()
    $clean = [regex]::Replace($raw, "\x1b\[[0-9;]*[A-Za-z]", '')
    $clean = $clean -replace "\r", ''
    # Drop the echoed command line itself, and the timestamp prefix the console adds.
    $lines = @()
    foreach ($ln in ($clean -split "`n")) {
        $t = $ln -replace '^\s*\$\s*', '' -replace '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}:\s*', ''
        if (-not $t.Trim()) { continue }
        if ($t.Trim() -eq $Command.Trim()) { continue }
        $lines += $t
    }
    return ($lines -join "`n")
}

function Test-FtmConsoleAlive {
    <#
    .SYNOPSIS
        True when the device console still answers - the liveness probe after a stress case.
    #>
    param($Console, [int]$TimeoutMs = 5000)
    # 'ping' is a real ftc command; 'version' is not, and a console that answers
    # "command not found" to everything would otherwise look alive AND make every
    # pattern-matching case pass for the wrong reason.
    $out = Invoke-FtmConsoleCommand -Console $Console -Command 'help' -TimeoutMs $TimeoutMs -QuietMs 800
    if ($out -match '(?i)command not found') { return $false }
    return ($out.Trim().Length -gt 0)
}

# ─── Verdict engine ─────────────────────────────────────────────────────────────
#
# Deliberately the same shape as KnxTest.psm1: one report format for the whole test plan.

$script:FtmRun = $null
$script:FtmEvidence = $null

function Start-FtmTestRun {
    param(
        [Parameter(Mandatory)][string]$Product,
        [Parameter(Mandatory)][string]$Target,
        [hashtable]$Environment = @{},
        [string]$RunProfile = 'Full'
    )
    $script:FtmRun = [pscustomobject]@{
        Product     = $Product
        Target      = $Target
        Profile     = $RunProfile
        Environment = $Environment
        Started     = [DateTime]::Now
        Finished    = $null
        Results     = New-Object System.Collections.ArrayList
    }
    return $script:FtmRun
}

function Add-FtmEvidence {
    <#
    .SYNOPSIS
        Attaches evidence to the running case. Call it BEFORE the assertion - a throwing
        assert never returns, and evidence recorded after it would be lost.
    #>
    param([byte[]]$Sent, [byte[]]$Received, [string]$Note, [string]$Output)
    if ($null -eq $script:FtmEvidence) { return }
    if ($PSBoundParameters.ContainsKey('Sent')) { $script:FtmEvidence.Sent = $Sent }
    if ($PSBoundParameters.ContainsKey('Received')) { $script:FtmEvidence.Received = $Received }
    if ($PSBoundParameters.ContainsKey('Note') -and $Note) { [void]$script:FtmEvidence.Notes.Add($Note) }
    if ($PSBoundParameters.ContainsKey('Output') -and $Output) {
        $trimmed = $Output.Trim()
        if ($trimmed.Length -gt 600) { $trimmed = $trimmed.Substring(0, 600) + ' ...' }
        [void]$script:FtmEvidence.Notes.Add("console: $trimmed")
    }
}

function Assert-FtmTrue {
    param([Parameter(Mandatory)][bool]$Condition, [Parameter(Mandatory)][string]$Message)
    if (-not $Condition) { throw "FTMTEST_FAIL::$Message" }
}

function Assert-FtmEqual {
    param($Expected, $Actual, [Parameter(Mandatory)][string]$Message)
    if ($Expected -ne $Actual) {
        Add-FtmEvidence -Note "expected '$Expected', got '$Actual'"
        throw "FTMTEST_FAIL::$Message (expected '$Expected', got '$Actual')"
    }
}

function Assert-FtmMatch {
    <#
    .SYNOPSIS
        Fails unless console output matches a pattern, recording the output as evidence.
    #>
    param([Parameter(Mandatory)][string]$Output, [Parameter(Mandatory)][string]$Pattern, [Parameter(Mandatory)][string]$Message)
    if ($Output -notmatch $Pattern) {
        Add-FtmEvidence -Output $Output
        throw "FTMTEST_FAIL::$Message (no match for /$Pattern/)"
    }
}

function Assert-FtmNotMatch {
    param([Parameter(Mandatory)][string]$Output, [Parameter(Mandatory)][string]$Pattern, [Parameter(Mandatory)][string]$Message)
    if ($Output -match $Pattern) {
        Add-FtmEvidence -Output $Output
        throw "FTMTEST_FAIL::$Message (unexpected match for /$Pattern/)"
    }
}

function Set-FtmTestSkip {
    param([Parameter(Mandatory)][string]$Reason)
    throw "FTMTEST_SKIP::$Reason"
}

function Set-FtmTestNotApplicable {
    param([Parameter(Mandatory)][string]$Reason)
    throw "FTMTEST_NA::$Reason"
}

function Invoke-FtmTestCase {
    <#
    .SYNOPSIS
        Runs one hardening case, captures its verdict and evidence, and records it.
    .PARAMETER Reference
        Where the expectation comes from - a section of doc/FTC-Reference.md, an error code
        from doc/errorcodes.txt, or a named constraint. Mandatory for the same reason the
        KNXnet/IP suites demand a clause: an expectation nobody can look up cannot be argued
        about, and an unarguable FAIL is worthless.
    #>
    param(
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$Title,
        [Parameter(Mandatory)][string]$Reference,
        [Parameter(Mandatory)][scriptblock]$Body,
        [string]$Suite = ''
    )
    if ($null -eq $script:FtmRun) { throw 'Start-FtmTestRun must be called before Invoke-FtmTestCase' }

    $script:FtmEvidence = [pscustomobject]@{
        Sent = $null; Received = $null
        Notes = (New-Object System.Collections.ArrayList)
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $verdict = 'PASS'; $reason = ''
    try { & $Body }
    catch {
        $msg = "$($_.Exception.Message)"
        if     ($msg -like 'FTMTEST_SKIP::*') { $verdict = 'SKIP'; $reason = $msg.Substring(14) }
        elseif ($msg -like 'FTMTEST_NA::*')   { $verdict = 'N-A';  $reason = $msg.Substring(12) }
        elseif ($msg -like 'FTMTEST_FAIL::*') { $verdict = 'FAIL'; $reason = $msg.Substring(14) }
        else                                  { $verdict = 'FAIL'; $reason = "unhandled error: $msg" }
    }
    $sw.Stop()

    $evSent = ''; $evRecv = ''
    if ($null -ne $script:FtmEvidence.Sent) { $evSent = ConvertTo-FtmHex -Bytes $script:FtmEvidence.Sent -MaxBytes 64 }
    if ($null -ne $script:FtmEvidence.Received) { $evRecv = ConvertTo-FtmHex -Bytes $script:FtmEvidence.Received -MaxBytes 64 }

    $res = [pscustomobject]@{
        Id        = $Id
        Suite     = $Suite
        Title     = $Title
        Reference = $Reference
        Verdict   = $verdict
        Reason    = $reason
        Ms        = [int]$sw.ElapsedMilliseconds
        Sent      = $evSent
        Received  = $evRecv
        Notes     = @($script:FtmEvidence.Notes)
    }
    [void]$script:FtmRun.Results.Add($res)
    Write-FtmVerdictLine -Result $res
    $script:FtmEvidence = $null
    # No return value on purpose - see the same note in KnxTest.psm1.
}

function Write-FtmVerdictLine {
    param([Parameter(Mandatory)]$Result)
    $col = switch ($Result.Verdict) {
        'PASS' { 'Green' } 'FAIL' { 'Red' } 'SKIP' { 'Yellow' } 'N-A' { 'DarkGray' } default { 'Gray' }
    }
    Write-Host ("  {0} {1} " -f $Result.Id.PadRight(10), $Result.Verdict.PadRight(4)) -ForegroundColor $col -NoNewline
    Write-Host $Result.Title -ForegroundColor Gray -NoNewline
    if ($Result.Reason) { Write-Host ("  -- " + $Result.Reason) -ForegroundColor $col } else { Write-Host '' }
}

function Get-FtmRunSummary {
    param($Run = $null)
    if ($null -eq $Run) { $Run = $script:FtmRun }
    $c = @{ PASS = 0; FAIL = 0; SKIP = 0; 'N-A' = 0 }
    foreach ($r in $Run.Results) { $c[$r.Verdict] = $c[$r.Verdict] + 1 }
    return [pscustomobject]@{ Total = $Run.Results.Count; Pass = $c.PASS; Fail = $c.FAIL; Skip = $c.SKIP; NA = $c.'N-A' }
}

function Export-FtmTestReport {
    param($Run = $null, [Parameter(Mandatory)][string]$Directory)
    if ($null -eq $Run) { $Run = $script:FtmRun }
    if ($null -eq $Run) { throw 'no run to export' }
    $Run.Finished = [DateTime]::Now
    if (-not (Test-Path $Directory)) { [void](New-Item -ItemType Directory -Path $Directory -Force) }

    $stamp = $Run.Started.ToString('yyyyMMdd-HHmmss')
    $base = Join-Path $Directory ("{0}_{1}" -f $Run.Product, $stamp)
    $sum = Get-FtmRunSummary -Run $Run

    $md = New-Object System.Text.StringBuilder
    [void]$md.AppendLine("# FTC / FTM hardening report - $($Run.Product)")
    [void]$md.AppendLine('')
    [void]$md.AppendLine("Run started: $($Run.Started.ToString('yyyy-MM-dd HH:mm:ss'))  ")
    [void]$md.AppendLine("Run finished: $($Run.Finished.ToString('yyyy-MM-dd HH:mm:ss'))  ")
    [void]$md.AppendLine("Target: $($Run.Target)  |  profile: $($Run.Profile)")
    [void]$md.AppendLine('')
    [void]$md.AppendLine('## Environment')
    [void]$md.AppendLine('')
    [void]$md.AppendLine('| Key | Value |')
    [void]$md.AppendLine('|---|---|')
    foreach ($k in ($Run.Environment.Keys | Sort-Object)) { [void]$md.AppendLine("| $k | $($Run.Environment[$k]) |") }
    [void]$md.AppendLine('')
    [void]$md.AppendLine('## Summary')
    [void]$md.AppendLine('')
    [void]$md.AppendLine('| Total | PASS | FAIL | SKIP | N-A |')
    [void]$md.AppendLine('|---|---|---|---|---|')
    [void]$md.AppendLine("| $($sum.Total) | $($sum.Pass) | $($sum.Fail) | $($sum.Skip) | $($sum.NA) |")
    [void]$md.AppendLine('')

    foreach ($s in ($Run.Results | Group-Object -Property Suite)) {
        [void]$md.AppendLine("## $($s.Name)")
        [void]$md.AppendLine('')
        [void]$md.AppendLine('| ID | Verdict | Case | Reference | Reason | ms |')
        [void]$md.AppendLine('|---|---|---|---|---|---|')
        foreach ($r in $s.Group) {
            $reason = ($r.Reason -replace '\|', '\|')
            [void]$md.AppendLine("| ``$($r.Id)`` | **$($r.Verdict)** | $($r.Title) | $($r.Reference) | $reason | $($r.Ms) |")
        }
        [void]$md.AppendLine('')
    }

    $fails = @($Run.Results | Where-Object { $_.Verdict -eq 'FAIL' })
    if ($fails.Count -gt 0) {
        [void]$md.AppendLine('## Failure evidence')
        [void]$md.AppendLine('')
        foreach ($r in $fails) {
            [void]$md.AppendLine("### $($r.Id) - $($r.Title)")
            [void]$md.AppendLine('')
            [void]$md.AppendLine("* Reference: $($r.Reference)")
            [void]$md.AppendLine("* Reason: $($r.Reason)")
            if ($r.Sent) { [void]$md.AppendLine("* Sent: ``$($r.Sent)``") }
            if ($r.Received) { [void]$md.AppendLine("* Received: ``$($r.Received)``") }
            foreach ($n in $r.Notes) { [void]$md.AppendLine("* Note: $n") }
            [void]$md.AppendLine('')
        }
    }

    $mdPath = "$base.md"; $jsonPath = "$base.json"
    [System.IO.File]::WriteAllText($mdPath, $md.ToString())
    ([pscustomobject]@{
            product = $Run.Product; target = $Run.Target; profile = $Run.Profile
            started = $Run.Started.ToString('o'); finished = $Run.Finished.ToString('o')
            environment = $Run.Environment; summary = $sum; results = @($Run.Results)
        }) | ConvertTo-Json -Depth 6 | Set-Content -Path $jsonPath -Encoding UTF8

    return [pscustomobject]@{ Markdown = $mdPath; Json = $jsonPath; Summary = $sum }
}

# ─── Self-test ──────────────────────────────────────────────────────────────────

function Invoke-FtmSelfTest {
    <#
    .SYNOPSIS
        Verifies the payload builders and the documented limits offline.
    .DESCRIPTION
        The FTC protocol has no external reference implementation to check against, so
        these vectors pin the invariants that the documentation states: the APDU bound,
        how the remote-path maximum is derived from it, and the sequence encoding.
    #>
    param([switch]$Quiet)
    $cases = New-Object System.Collections.ArrayList
    function Add-Case([string]$name, [bool]$ok, [string]$expected, [string]$actual) {
        [void]$cases.Add([pscustomobject]@{ Name = $name; Ok = $ok; Expected = $expected; Actual = $actual })
    }

    $L = $script:FtmLimit

    # A path is ASCII plus a NUL terminator.
    $p = New-FtmPath -Path 'sd/test.bin'
    Add-Case 'path is NUL terminated' ($p[$p.Length - 1] -eq 0x00) '00' ('{0:X2}' -f $p[$p.Length - 1])
    Add-Case 'path length is name + 1' ($p.Length -eq 12) '12' "$($p.Length)"
    $pn = New-FtmPath -Path 'sd/test.bin' -NoTerminator
    Add-Case 'unterminated path has no NUL' ($pn.Length -eq 11 -and $pn[$pn.Length - 1] -ne 0x00) '11' "$($pn.Length)"

    # The remote-path maximum must fit one APDU in a fast open: 6 header + path + NUL + 4
    # size octets. At 235 that is 246, so the constant sits ONE octet below the arithmetic
    # bound - deliberately, not accidentally. The vectors pin both facts, because asserting
    # "one more does not fit" would be false and would have made this self-test lie.
    $maxOpen = New-FtmOpenPayload -Path ('a' * $L.REMOTE_PATH_MAX) -SizeHint 1234
    Add-Case 'fast open with the maximum path fits one APDU' ($maxOpen.Length -le $L.APDU_PAYLOAD_MAX) "<= $($L.APDU_PAYLOAD_MAX)" "$($maxOpen.Length)"
    Add-Case 'the maximum path leaves exactly one octet of headroom' ($maxOpen.Length -eq ($L.APDU_PAYLOAD_MAX - 1)) "$($L.APDU_PAYLOAD_MAX - 1)" "$($maxOpen.Length)"
    $overOpen = New-FtmOpenPayload -Path ('a' * ($L.REMOTE_PATH_MAX + 2)) -SizeHint 1234
    Add-Case 'two octets more no longer fit' ($overOpen.Length -gt $L.APDU_PAYLOAD_MAX) "> $($L.APDU_PAYLOAD_MAX)" "$($overOpen.Length)"

    # Sequence numbers are little endian u16.
    $c = New-FtmChunkPayload -Sequence 0x1234 -Data ([byte[]]@(0xAA, 0xBB))
    Add-Case 'chunk sequence is little endian' ((ConvertTo-FtmHex -Bytes $c) -eq '34 12 AA BB') '34 12 AA BB' (ConvertTo-FtmHex -Bytes $c)
    $c0 = New-FtmChunkPayload -Sequence 0
    Add-Case 'sequence 0 encodes as 00 00' ((ConvertTo-FtmHex -Bytes $c0) -eq '00 00') '00 00' (ConvertTo-FtmHex -Bytes $c0)

    # Size hint is little endian u32 and sits after the NUL.
    $o = New-FtmOpenPayload -Path 'x' -ChunkSize 240 -Chunks 1 -SizeHint 0x01020304
    $tail = ConvertTo-FtmHex -Bytes ([byte[]]$o[($o.Length - 4)..($o.Length - 1)])
    Add-Case 'size hint is little endian u32' ($tail -eq '04 03 02 01') '04 03 02 01' $tail

    $ov = New-FtmOversizedPayload -Length 260
    Add-Case 'oversized payload exceeds the APDU bound' ($ov.Length -gt $L.APDU_PAYLOAD_MAX) "> $($L.APDU_PAYLOAD_MAX)" "$($ov.Length)"

    # Constant tables must be self consistent.
    Add-Case 'FileInfo has all four status codes' ($script:FtmFileInfo.Count -eq 4) '4' "$($script:FtmFileInfo.Count)"
    Add-Case 'command name lookup resolves FileInfo' ((Get-FtmCommandName -PropertyId 43) -eq 'FileInfo') 'FileInfo' (Get-FtmCommandName -PropertyId 43)
    Add-Case 'FileInfo name lookup resolves the poll-again code' ((Get-FtmFileInfoName -Status 0x02) -eq 'CRC_PENDING') 'CRC_PENDING' (Get-FtmFileInfoName -Status 0x02)
    Add-Case 'flood cliff is above the throughput ceiling' ($L.FLOOD_CLIFF -gt $L.THROUGHPUT_CEILING) 'true' "$($L.FLOOD_CLIFF) > $($L.THROUGHPUT_CEILING)"

    Add-Case 'traversal shapes are non empty' ((New-FtmTraversalPath).Count -ge 5) '>= 5' "$((New-FtmTraversalPath).Count)"

    # Negative control: the comparison must be able to fail.
    Add-Case 'negative control (deliberate mismatch is detected)' ((ConvertTo-FtmHex -Bytes ([byte[]]@(1))) -ne (ConvertTo-FtmHex -Bytes ([byte[]]@(2)))) 'detected' 'detected'

    $failed = @($cases | Where-Object { -not $_.Ok })
    if (-not $Quiet) {
        Write-Host ''
        Write-Host '  Self-test - FTC payload builders and documented limits' -ForegroundColor Cyan
        foreach ($c in $cases) {
            if ($c.Ok) { Write-Host ('    PASS  ' + $c.Name) -ForegroundColor Green }
            else {
                Write-Host ('    FAIL  ' + $c.Name) -ForegroundColor Red
                Write-Host ('          expected ' + $c.Expected) -ForegroundColor DarkGray
                Write-Host ('          actual   ' + $c.Actual) -ForegroundColor DarkGray
            }
        }
        Write-Host ''
        $col = if ($failed.Count -eq 0) { 'Green' } else { 'Red' }
        Write-Host ("  {0}/{1} vectors passed" -f ($cases.Count - $failed.Count), $cases.Count) -ForegroundColor $col
        Write-Host ''
    }
    return [pscustomobject]@{ Total = $cases.Count; Failed = $failed.Count; Ok = ($failed.Count -eq 0); Cases = @($cases) }
}

Export-ModuleMember -Function *-* -Variable @()
