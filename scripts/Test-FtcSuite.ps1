#!/usr/bin/env pwsh
<#
Open ■
┬────┴  Test-FtcSuite
■ KNX   2025 OpenKNX - Erkan Çolak

FILEPATH: OFM-FileTransferModule/scripts/Test-FtcSuite.ps1

.SYNOPSIS
    FTC console regression suite: drive an OpenKNX device's USB console, run the
    ftc command battery against a target device, and write a pass/fail report.

.DESCRIPTION
    Opens the device console over serial WITHOUT toggling DTR/RTS (so a CH340/ESP
    is not reset on connect), sends each ftc test command, captures the console
    output, checks it against an expected pattern, and produces:
      * a live PASS/FAIL summary on screen, and
      * a Markdown report (raw console output per test) at -ReportPath.

    The suite exercises the whole client: help, ping, info (+ ga), df, ll, the
    connection-oriented scan (ets) with the openknx / info / save-CSV inventory,
    the speed test (cooperative CRC + two-line progress), and a real file upload
    (upload-only -- the apply/fwupdate trigger is intentionally NOT sent).

    Cross-platform (Windows / macOS / Linux) via .NET System.IO.Ports.SerialPort.

.PARAMETER Port
    Serial port of the DRIVING device's console (the ftc client), e.g.
    /dev/cu.wchusbserial57640270261 (macOS) or COM5 (Windows). Mandatory.

.PARAMETER Target
    KNX PA of the TARGET device the ftc commands act on. Default 5.0.3.

.PARAMETER Fw
    Source path (on the driver's filesystem) uploaded in the upload test.
    Default sd/KNeoPix-RP2350.bin.gz. Upload only -- never applied.

.PARAMETER ScanFrom / .PARAMETER ScanTo
    PA range for the scan tests. Default 5.0.1 .. 5.0.10 (small = fast).

.PARAMETER ReportPath
    Markdown report output. Default ./ftc-testreport-<timestamp>.md next to the script.

.PARAMETER Baud
    Console baud. Default 115200.

.PARAMETER SkipUpload
    Skip the (slow) real upload test.

.EXAMPLE
    ./Test-FtcSuite.ps1 -Port /dev/cu.wchusbserial57640270261 -Target 5.0.3
    Run the full suite against 5.0.3 and write the report.

.EXAMPLE
    ./Test-FtcSuite.ps1 -Port COM5 -Target 5.0.3 -SkipUpload
    Fast pass without the multi-minute upload.
#>
param(
    [string] $Port,   # required -- checked in the body so -Help works without it
    [string] $Target = "5.0.3",
    [string] $Fw = "sd/KNeoPix-RP2350.bin.gz",
    [string] $ScanFrom = "5.0.1",
    [string] $ScanTo = "5.0.10",
    [string] $ReportPath,
    [int] $Baud = 115200,
    [switch] $SkipUpload,
    [switch] $Dtr,   # RP2040/RP2350 native USB-CDC: pass -Dtr (assert DTR = host present, else no output). ESP/CH340: omit (DTR low = no reset).
    [switch] $Help,    # print usage + REG1/REG2 + ESP/RP examples and exit
    [switch] $Verbose, # stream the live console output of each test (watch long perf/upload runs)
    [switch] $FullUpload # run the upload to COMPLETION ("verified OK") instead of the quick start-smoke
)

function OpenKNX_ShowLogo($AddCustomText = $null) {
    Write-Host ""
    Write-Host "Open " -NoNewline
    Write-Host "$( [char]::ConvertFromUtf32(0x25A0) )" -ForegroundColor Green
    $u = "$( [char]::ConvertFromUtf32(0x252C) )$( [char]::ConvertFromUtf32(0x2500) )$( [char]::ConvertFromUtf32(0x2500) )$( [char]::ConvertFromUtf32(0x2500) )$( [char]::ConvertFromUtf32(0x2500) )$( [char]::ConvertFromUtf32(0x2534) ) "
    if ($AddCustomText) { Write-Host "$u $AddCustomText" -ForegroundColor Green }
    else                { Write-Host $u                  -ForegroundColor Green }
    Write-Host "$( [char]::ConvertFromUtf32(0x25A0) )" -NoNewline -ForegroundColor Green
    Write-Host " KNX"
    Write-Host ""
}

# Strip ANSI CSI + the console's "[2K$ cmd" line-redraw echo; keep only the timestamped log lines.
function CleanConsole([string] $raw) {
    $noAnsi = [regex]::Replace($raw, "`e\[[0-9;]*[A-Za-z]", "")
    $noAnsi = $noAnsi -replace "`r", ""
    $lines = $noAnsi -split "`n"
    $out = foreach ($ln in $lines) {
        $m = [regex]::Match($ln, '(\d{2}:\d{2}:\d{2}:\s.*)$')
        if ($m.Success) { $m.Groups[1].Value }
    }
    return ($out -join "`n")
}

# Send one command and read the reply. With -Until (a regex) read until that completion marker appears
# (ignoring the natural gaps of a scan/transfer); otherwise read until the console is idle for $IdleMs.
# Both are capped by $TimeoutSec.
function Invoke-Console {
    param([System.IO.Ports.SerialPort] $Sp, [string] $Cmd, [int] $TimeoutSec = 8, [string] $Until, [int] $IdleMs = 1500, [switch] $Live)
    $Sp.DiscardInBuffer()
    $Sp.WriteLine($Cmd)
    $sb = [System.Text.StringBuilder]::new()
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $lastData = Get-Date
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 120
        $chunk = $Sp.ReadExisting()
        if ($chunk.Length -gt 0) {
            [void]$sb.Append($chunk); $lastData = Get-Date
            if ($Live) { Write-Host $chunk -NoNewline }   # -Verbose: echo the device output live
            if ($Until -and $sb.ToString() -match $Until) { Start-Sleep -Milliseconds 500; $tail = $Sp.ReadExisting(); [void]$sb.Append($tail); if ($Live) { Write-Host $tail -NoNewline }; break }
        }
        elseif (-not $Until -and ((Get-Date) - $lastData).TotalMilliseconds -gt $IdleMs) { break }
    }
    return $sb.ToString()
}

# ---- help / arg check ------------------------------------------------------
if ($Help) {
    OpenKNX_ShowLogo "FTC Test Suite"
    @'
  Drive an OpenKNX device's USB console, run the ftc command battery against a
  target device, and write a PASS/FAIL Markdown report.

  USAGE
    ./Test-FtcSuite.ps1 -Port <console> [-Target <pa>] [-Fw <src>] [-Dtr]
                        [-Verbose] [-FullUpload] [-SkipUpload] [-ReportPath <file.md>] [-Help]

  PARAMETERS
    -Port        Console serial of the DRIVING device (the ftc client). Required.
                   ESP / CH340    -> /dev/cu.wchusbserial*   (NO -Dtr)
                   RP native CDC  -> /dev/cu.usbmodem*       (needs -Dtr, else silent)
    -Target      KNX PA of the target device. Default 5.0.3.
    -Fw          Upload source ON THE DRIVER's filesystem. Default sd/KNeoPix-RP2350.bin.gz.
                   REG2 (has SD)  -> sd/<file>
                   REG1 (no SD)   -> fs root, e.g. /<file>
    -Dtr         Assert DTR for RP2040/RP2350 native USB-CDC. Omit for ESP/CH340.
    -SkipUpload  Skip the upload test entirely.
    -FullUpload  Run the upload to COMPLETION and require "verified OK" (~20 min over TP). Default is a
                 quick start-smoke reported as STARTED (the device keeps transferring in the background).
    -ReportPath  Markdown report path. A relative path resolves from the current dir;
                 if that dir is missing it falls back to the script dir.
    -Verbose     Stream the live console output of each test (watch the long perf / upload runs).
    -Help        This text.

  IMPORTANT -- the firmware IMAGE format depends on the TARGET device:
    * RP2040 / RP2350 target -> upload a COMPRESSED image (.bin.gz); the target ungzips + applies.
    * ESP32 target           -> upload a RAW image (.bin); NOT gzipped.

  EXAMPLES
    # REG2 (SD) driving an RP target (KNeoPix RP2350) -> compressed .gz from SD:
    ./Test-FtcSuite.ps1 -Port /dev/cu.wchusbserialXXXX -Target 5.0.3 -Fw sd/KNeoPix-RP2350.bin.gz

    # REG1 (no SD, ESP driver), firmware on the router's fs, RP target -> compressed .gz:
    ./Test-FtcSuite.ps1 -Port /dev/cu.wchusbserial8430 -Target 5.0.3 -Fw /KNeoPix-RP2350.bin.gz

    # RP driver (native CDC) -> add -Dtr:
    ./Test-FtcSuite.ps1 -Port /dev/cu.usbmodemXXXX -Target 5.0.3 -Fw sd/KNeoPix-RP2350.bin.gz -Dtr

    # ESP32 TARGET -> upload a RAW .bin (NOT gzipped):
    ./Test-FtcSuite.ps1 -Port /dev/cu.wchusbserialXXXX -Target 5.0.7 -Fw sd/MyEspApp.bin

    # Windows: use the COM port instead of a /dev path (everything else identical):
    ./Test-FtcSuite.ps1 -Port COM5 -Target 5.0.3 -Fw sd/KNeoPix-RP2350.bin.gz

    # Fast pass without the multi-minute upload:
    ./Test-FtcSuite.ps1 -Port <port> -Target 5.0.3 -SkipUpload
'@ | Write-Host
    exit 0
}
if (-not $Port) {
    Write-Host "  -Port is required (console of the driving device). Run with -Help for usage + examples." -ForegroundColor Red
    exit 2
}

# ---- test battery ----------------------------------------------------------
$T = @(
    [pscustomobject]@{ Name = "help";           Cmd = "ftc ?";                                              Sec = 6;   Expect = "send \| upload|Transfer" }
    [pscustomobject]@{ Name = "ping";           Cmd = "ftc $Target ping";                                   Sec = 8;   Expect = "result=0x0|Response.*prop|module|version|alive" }
    [pscustomobject]@{ Name = "info";           Cmd = "ftc $Target info";                                   Sec = 12;  Expect = "Mask|Manufacturer|class" }
    [pscustomobject]@{ Name = "info-ga";        Cmd = "ftc $Target info ga";                                Sec = 15;  Expect = "GA|group|assoc|table|com" }
    [pscustomobject]@{ Name = "df";             Cmd = "ftc $Target df";                                     Sec = 8;   Expect = "total|free|used" }
    [pscustomobject]@{ Name = "ll";             Cmd = "ftc $Target ll";                                     Sec = 12;  Expect = "CRC|dir|Name|total|B" }
    [pscustomobject]@{ Name = "scan-ets";       Cmd = "ftc scan $ScanFrom $ScanTo ets";                     Sec = 90;  Expect = "device.s. found|probed"; Until = "device.s. found|probed" }
    [pscustomobject]@{ Name = "scan-openknx";   Cmd = "ftc scan $ScanFrom $ScanTo ets openknx";             Sec = 150; Expect = "OpenKNX|device.s. found"; Until = "device.s. found|probed" }
    [pscustomobject]@{ Name = "scan-info-save"; Cmd = "ftc scan $ScanFrom $ScanTo ets info save sd/inv.csv"; Sec = 240; Expect = "scan saved|device.s. found"; Until = "scan saved|device.s. found" }
    [pscustomobject]@{ Name = "perf";           Cmd = "ftc $Target perf 50 253 fast";                       Sec = 300; Expect = "Speed test|B/s|COMPLETE"; Until = "COMPLETE|nothing to send|avg .*B/s|aborting" }
)
if (-not $SkipUpload) {
    # Clear any stale copy on the target FIRST, so the upload is a REAL transfer and not skipped as
    # "already up to date -- nothing to send" (idempotent when size+CRC match). A not-found rm is fine.
    $fwLeaf = ($Fw -split '/')[-1]   # device path is always '/'-delimited -> host-independent leaf (Split-Path uses '\' on Windows)
    $T += [pscustomobject]@{ Name = "clear-target";  Cmd = "ftc $Target rm /$fwLeaf";                    Sec = 20;  Expect = "rm:.*(ok|failed)"; Until = "rm:.*(ok|failed)" }
    # A full firmware upload over TP is ~20 min. DEFAULT = a quick SMOKE check: confirm the transfer
    # starts and real chunks flow -> status STARTED (NOT a green "done"; the DEVICE keeps transferring in
    # the background after the suite moves on). -FullUpload instead WAITS for the real "verified OK".
    $upSec   = if ($FullUpload) { 2400 } else { 90 }
    $upUntil = if ($FullUpload) { "verified OK|COMPLETE|nothing to send|aborting" } else { "[0-9]+ B/s" }
    $T += [pscustomobject]@{ Name = "upload"; Cmd = "ftc $Target upload $Fw fast verbose"; Sec = $upSec; Expect = "verified OK|COMPLETE|already up to date|nothing to send"; Started = "[0-9]+ B/s"; MustNot = "generated test pattern"; Until = $upUntil }
}

# ---- open the console (NO reset: DTR/RTS stay deasserted) -------------------
OpenKNX_ShowLogo "FTC Test Suite"
Write-Host "  Console : $Port @ $Baud" -ForegroundColor Cyan
Write-Host "  Target  : $Target        Upload src: $Fw" -ForegroundColor Cyan
Write-Host ""

if (-not $ReportPath) {
    $ts = Get-Date -Format "yyyyMMdd-HHmmss"
    $ReportPath = Join-Path $PSScriptRoot "ftc-testreport-$ts.md"
}

$sp = [System.IO.Ports.SerialPort]::new($Port, $Baud)
$sp.DtrEnable = [bool]$Dtr  # ESP/CH340: false (DTR low -> no auto-reset). RP native CDC: -Dtr true (host present, else silent).
$sp.RtsEnable = $false
$sp.ReadTimeout = 500
$sp.NewLine = "`n"
try { $sp.Open() } catch { Write-Host "  Cannot open $Port : $_" -ForegroundColor Red; exit 2 }
Start-Sleep -Milliseconds 400
$sp.DiscardInBuffer()

$results = @()
foreach ($t in $T) {
    if ($Verbose) {
        Write-Host ""
        Write-Host ("  >> {0}" -f $t.Name) -ForegroundColor Cyan
        Write-Host ("     cmd: {0}" -f $t.Cmd) -ForegroundColor DarkGray
    }
    else {
        Write-Host ("  {0,-16} " -f $t.Name) -NoNewline
    }
    $raw = Invoke-Console -Sp $sp -Cmd $t.Cmd -TimeoutSec $t.Sec -Until $t.Until -Live:$Verbose
    $clean = CleanConsole $raw
    # Tri-state: PASS = Expect matched and MustNot clear. A test carrying a .Started marker that only
    # started + progressed but never completed -> STARTED (yellow, NOT counted as passed; e.g. the upload
    # smoke, where the device keeps transferring in the background). Otherwise FAIL.
    $status = 'FAIL'
    if ($t.MustNot -and ($clean -match $t.MustNot)) { $status = 'FAIL' }   # a forbidden marker present -> hard fail (never STARTED/PASS)
    elseif ($clean -match $t.Expect) { $status = 'PASS' }
    elseif ($t.Started -and ($clean -match $t.Started)) { $status = 'STARTED' }
    $color = switch ($status) { 'PASS' { 'Green' } 'STARTED' { 'Yellow' } default { 'Red' } }
    if ($Verbose) { Write-Host ("     -> {0}" -f $status) -ForegroundColor $color }
    else { Write-Host $status -ForegroundColor $color }
    $results += [pscustomobject]@{ Name = $t.Name; Cmd = $t.Cmd; Status = $status; Pass = ($status -eq 'PASS'); Output = $clean }
}
$sp.Close()

# ---- report ----------------------------------------------------------------
$pass    = ($results | Where-Object { $_.Status -eq 'PASS' }).Count
$started = ($results | Where-Object { $_.Status -eq 'STARTED' }).Count
$failed  = ($results | Where-Object { $_.Status -eq 'FAIL' }).Count
$total = $results.Count
$resultLine = "**$pass / $total passed**" + $(if ($started) { "  --  $started STARTED (capped, device still transferring)" }) + $(if ($failed) { "  --  $failed FAILED" })
$md = [System.Text.StringBuilder]::new()
[void]$md.AppendLine("# FTC Test Suite Report")
[void]$md.AppendLine("")
[void]$md.AppendLine("- Date: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
[void]$md.AppendLine("- Console: ``$Port`` @ $Baud   Target: **$Target**   Upload: ``$Fw``")
[void]$md.AppendLine("- Result: $resultLine")
[void]$md.AppendLine("")
[void]$md.AppendLine("| Test | Result | Command |")
[void]$md.AppendLine("|---|---|---|")
foreach ($r in $results) {
    [void]$md.AppendLine("| $($r.Name) | $($r.Status) | ``$($r.Cmd)`` |")
}
[void]$md.AppendLine("")
foreach ($r in $results) {
    [void]$md.AppendLine("## $($r.Name)  ($($r.Status))")
    [void]$md.AppendLine('```')
    [void]$md.AppendLine($r.Output)
    [void]$md.AppendLine('```')
    [void]$md.AppendLine("")
}
# Write robustly: resolve a relative -ReportPath against the invocation dir; if that directory does
# not exist (e.g. a redundant relative path), fall back to the script dir so the report is never lost.
if (-not [System.IO.Path]::IsPathRooted($ReportPath)) {
    $ReportPath = Join-Path (Get-Location).Path $ReportPath
}
try {
    Set-Content -LiteralPath $ReportPath -Value $md.ToString() -Encoding utf8 -ErrorAction Stop
} catch {
    $ReportPath = Join-Path $PSScriptRoot (Split-Path -Leaf $ReportPath)
    Set-Content -LiteralPath $ReportPath -Value $md.ToString() -Encoding utf8
    Write-Host "  (note: -ReportPath directory not found -- wrote the report next to the script instead)" -ForegroundColor DarkYellow
}

Write-Host ""
$summaryLine = "  ===== $pass / $total passed" + $(if ($started) { " -- $started STARTED (device still transferring in background)" }) + $(if ($failed) { " -- $failed FAILED" }) + " ====="
Write-Host $summaryLine -ForegroundColor $(if ($failed) { "Red" } elseif ($started) { "Yellow" } else { "Green" })
Write-Host "  Report: $ReportPath" -ForegroundColor Cyan
Write-Host ""
