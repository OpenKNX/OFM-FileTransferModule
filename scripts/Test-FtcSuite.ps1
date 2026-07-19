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
    [Parameter(Mandatory = $true)] [string] $Port,
    [string] $Target = "5.0.3",
    [string] $Fw = "sd/KNeoPix-RP2350.bin.gz",
    [string] $ScanFrom = "5.0.1",
    [string] $ScanTo = "5.0.10",
    [string] $ReportPath,
    [int] $Baud = 115200,
    [switch] $SkipUpload,
    [switch] $Dtr    # RP2040/RP2350 native USB-CDC: pass -Dtr (assert DTR = host present, else no output). ESP/CH340: omit (DTR low = no reset).
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
    param([System.IO.Ports.SerialPort] $Sp, [string] $Cmd, [int] $TimeoutSec = 8, [string] $Until, [int] $IdleMs = 1500)
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
            if ($Until -and $sb.ToString() -match $Until) { Start-Sleep -Milliseconds 500; [void]$sb.Append($Sp.ReadExisting()); break }
        }
        elseif (-not $Until -and ((Get-Date) - $lastData).TotalMilliseconds -gt $IdleMs) { break }
    }
    return $sb.ToString()
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
    # 300s cap = validates start + two-line progress + no-test-pattern + no-block (passes on "B/s"); the full
    # multi-minute completion over TP is a separate run. Returns early if it does finish (Until = COMPLETE).
    $T += [pscustomobject]@{ Name = "upload-start"; Cmd = "ftc $Target upload $Fw fast verbose"; Sec = 300; Expect = "B/s|COMPLETE|verified OK|already up to date|nothing to send"; MustNot = "generated test pattern"; Until = "COMPLETE|verified OK|nothing to send|aborting" }
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
    Write-Host ("  {0,-16} " -f $t.Name) -NoNewline
    $raw = Invoke-Console -Sp $sp -Cmd $t.Cmd -TimeoutSec $t.Sec -Until $t.Until
    $clean = CleanConsole $raw
    $ok = ($clean -match $t.Expect)
    if ($ok -and $t.MustNot) { $ok = ($clean -notmatch $t.MustNot) }
    if ($ok) { Write-Host "PASS" -ForegroundColor Green } else { Write-Host "FAIL" -ForegroundColor Red }
    $results += [pscustomobject]@{ Name = $t.Name; Cmd = $t.Cmd; Pass = $ok; Output = $clean }
}
$sp.Close()

# ---- report ----------------------------------------------------------------
$pass = ($results | Where-Object Pass).Count
$total = $results.Count
$md = [System.Text.StringBuilder]::new()
[void]$md.AppendLine("# FTC Test Suite Report")
[void]$md.AppendLine("")
[void]$md.AppendLine("- Date: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
[void]$md.AppendLine("- Console: ``$Port`` @ $Baud   Target: **$Target**   Upload: ``$Fw``")
[void]$md.AppendLine("- Result: **$pass / $total passed**")
[void]$md.AppendLine("")
[void]$md.AppendLine("| Test | Result | Command |")
[void]$md.AppendLine("|---|---|---|")
foreach ($r in $results) {
    $mark = if ($r.Pass) { "PASS" } else { "FAIL" }
    [void]$md.AppendLine("| $($r.Name) | $mark | ``$($r.Cmd)`` |")
}
[void]$md.AppendLine("")
foreach ($r in $results) {
    [void]$md.AppendLine("## $($r.Name)  ($(if($r.Pass){'PASS'}else{'FAIL'}))")
    [void]$md.AppendLine('```')
    [void]$md.AppendLine($r.Output)
    [void]$md.AppendLine('```')
    [void]$md.AppendLine("")
}
Set-Content -LiteralPath $ReportPath -Value $md.ToString() -Encoding utf8

Write-Host ""
Write-Host ("  ===== $pass / $total passed =====") -ForegroundColor $(if ($pass -eq $total) { "Green" } else { "Yellow" })
Write-Host "  Report: $ReportPath" -ForegroundColor Cyan
Write-Host ""
