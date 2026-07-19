#!/usr/bin/env pwsh
<#
Open ■
┬────┴  Test-FtcResume
■ KNX   2025 OpenKNX - Erkan Çolak

FILEPATH: OFM-FileTransferModule/scripts/Test-FtcResume.ps1

.SYNOPSIS
    FTC resume-robustness test: start a real upload, reset the client's BCU mid-transfer,
    and verify the transfer AUTO-RESUMES from the target's partial (not a restart) and finishes.

.DESCRIPTION
    The plain suite only validates the upload START. This drives the real robustness path:
      1. delete the target file so the upload is a genuine fresh transfer,
      2. start `ftc <pa> send <fw> fast verbose`,
      3. after -InjectSec, inject `bcu rst` on the client console (interrupts the KNX transceiver
         mid-transfer -> a transient failure),
      4. keep reading and assert the client emits "resuming at chunk N/M" (RESUME, not restart),
         then "verified OK / COMPLETE", with NO "loop took longer" (the cooperative CRC must not block).
    Writes a Markdown report + a PASS/FAIL verdict. Cross-platform via .NET SerialPort.

.PARAMETER Port    Console serial port of the ftc CLIENT (the device doing the upload). Mandatory.
.PARAMETER Target  KNX PA of the upload target. Default 5.0.3.
.PARAMETER Fw      Source path on the client (its basename is the remote target file). Default sd/KNeoPix-RP2350.bin.gz.
.PARAMETER Dtr     Pass for RP2040/RP2350 native USB-CDC (assert DTR); omit for ESP/CH340.
.PARAMETER InjectSec  Seconds into the transfer to inject `bcu rst`. Default 45.
.PARAMETER MaxSec  Overall cap. Default 1200.
.PARAMETER ReportPath  Markdown report. Default ./ftc-resumereport-<timestamp>.md.
.PARAMETER Baud    Default 115200.

.EXAMPLE
    ./Test-FtcResume.ps1 -Port /dev/cu.usbmodem84101 -Dtr -Target 5.0.3
#>
param(
    [Parameter(Mandatory = $true)] [string] $Port,
    [string] $Target = "5.0.3",
    [string] $Fw = "sd/KNeoPix-RP2350.bin.gz",
    [switch] $Dtr,
    [int] $InjectSec = 45,
    [int] $MaxSec = 1200,
    [string] $ReportPath,
    [int] $Baud = 115200,
    [int] $Perf = 0    # >0 = use a `perf <Perf>kb` RAM transfer instead of a file upload (no SD source needed; same resume path)
)

function OpenKNX_ShowLogo($AddCustomText = $null) {
    Write-Host ""
    Write-Host "Open " -NoNewline; Write-Host "$( [char]::ConvertFromUtf32(0x25A0) )" -ForegroundColor Green
    $u = "$( [char]::ConvertFromUtf32(0x252C) )$( [char]::ConvertFromUtf32(0x2500) )$( [char]::ConvertFromUtf32(0x2500) )$( [char]::ConvertFromUtf32(0x2500) )$( [char]::ConvertFromUtf32(0x2500) )$( [char]::ConvertFromUtf32(0x2534) ) "
    if ($AddCustomText) { Write-Host "$u $AddCustomText" -ForegroundColor Green } else { Write-Host $u -ForegroundColor Green }
    Write-Host "$( [char]::ConvertFromUtf32(0x25A0) )" -NoNewline -ForegroundColor Green; Write-Host " KNX"; Write-Host ""
}
function CleanConsole([string] $raw) {
    $t = ([regex]::Replace($raw, "`e\[[0-9;]*[A-Za-z]", "")) -replace "`r", ""
    (($t -split "`n") | ForEach-Object { $m = [regex]::Match($_, '(\d{2}:\d{2}:\d{2}:\s.*)$'); if ($m.Success) { $m.Groups[1].Value } }) -join "`n"
}

if ($Perf -gt 0) {
    $remote = "/ftcperf.bin"; $xfer = "ftc $Target perf $Perf 253 fast"   # RAM pattern -> no SD source needed, same resume path
} else {
    $remote = $Fw -replace '^(sd|efc)/', ''            # strip the backend prefix (sd/ efc/); keep any subpath
    if (-not $remote.StartsWith('/')) { $remote = "/$remote" }
    $xfer = "ftc $Target send $Fw fast verbose"        # real file upload -> the send writes $remote
}

OpenKNX_ShowLogo "FTC Resume Test (bcu rst mid-upload)"
Write-Host "  Console : $Port @ $Baud   (Dtr=$([bool]$Dtr))" -ForegroundColor Cyan
Write-Host "  Target  : $Target   src $Fw   remote $remote   inject bcu rst @ ${InjectSec}s" -ForegroundColor Cyan
Write-Host ""
if (-not $ReportPath) { $ReportPath = Join-Path $PSScriptRoot ("ftc-resumereport-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".md") }

$sp = [System.IO.Ports.SerialPort]::new($Port, $Baud)
$sp.DtrEnable = [bool]$Dtr; $sp.RtsEnable = $false; $sp.NewLine = "`n"; $sp.ReadTimeout = 500
try { $sp.Open() } catch { Write-Host "  Cannot open $Port : $_" -ForegroundColor Red; exit 2 }
Start-Sleep -Milliseconds 400; $sp.DiscardInBuffer()

$log = [System.Text.StringBuilder]::new()
function Send($c) { $sp.DiscardInBuffer(); $sp.WriteLine($c) }
function Pump($sec) { $end = (Get-Date).AddSeconds($sec); while ((Get-Date) -lt $end) { Start-Sleep -Milliseconds 150; $d = $sp.ReadExisting(); if ($d) { [void]$log.Append($d) } } }

# 1) delete target -> force a genuine fresh transfer
Write-Host "  1) rm $remote (fresh start) ..." -ForegroundColor DarkGray
Send "ftc $Target rm $remote"; Pump 6

# 2) start the transfer (file upload, or -Perf RAM pattern)
Write-Host "  2) start transfer: $xfer ..." -ForegroundColor DarkGray
[void]$log.AppendLine("===TRANSFER-START===")
Send $xfer

# 3) read; inject `bcu rst` once after InjectSec; stop on completion or MaxSec
$injected = $false; $injAt = $null
$deadline = (Get-Date).AddSeconds($MaxSec); $t0 = Get-Date
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 150
    $d = $sp.ReadExisting(); if ($d) { [void]$log.Append($d) }
    $el = ((Get-Date) - $t0).TotalSeconds
    if (-not $injected -and $el -ge $InjectSec) {
        Write-Host "  3) INJECT 'bcu rst' at ${InjectSec}s ..." -ForegroundColor Yellow
        [void]$log.AppendLine("===INJECT bcu rst @ $([int]$el)s ===")
        Send "bcu rst"; $injected = $true; $injAt = $el
    }
    $cur = $log.ToString()
    if ($injected -and ($cur -match "verified OK|COMPLETE|nothing to send|aborting|Verify\s+OK|Verify\s+FAILED")) {
        Start-Sleep -Milliseconds 800; [void]$log.Append($sp.ReadExisting()); break
    }
}
$sp.Close()

# 4) verdict
$clean = CleanConsole $log.ToString()
$after = $clean; $ix = $clean.IndexOf("INJECT bcu rst"); if ($ix -ge 0) { $after = $clean.Substring($ix) }
$resumed   = ($after -match "resuming at chunk|matching \d+ B partial|resuming")
$completed = ($clean -match "verified OK|COMPLETE|Verify\s+OK")
$blocked   = ([regex]::Matches($clean, "took longer")).Count
$restarted = ($after -match "0% .*seq 1/|chunk 1/|from scratch|truncat")
$verdict = ($injected -and $resumed -and $completed -and -not $restarted)

$md = @()
$md += "# FTC Resume-under-bcu-rst Report"
$md += ""
$md += "- Date: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')   Console: ``$Port`` (Dtr=$([bool]$Dtr))"
$md += "- Target: **$Target**   src ``$Fw``   remote ``$remote``   injected bcu rst @ $([int]$injAt)s"
$md += "- **Verdict: $(if($verdict){'PASS'}else{'FAIL'})**  (resumed=$resumed  completed=$completed  restarted=$restarted  loop-warnings=$blocked)"
$md += ""
$md += '```'
$md += $clean
$md += '```'
Set-Content -LiteralPath $ReportPath -Value ($md -join "`n") -Encoding utf8

Write-Host ""
Write-Host ("  resumed={0}  completed={1}  restarted={2}  loop-warnings={3}" -f $resumed,$completed,$restarted,$blocked)
Write-Host ("  ===== VERDICT: {0} =====" -f $(if($verdict){'PASS'}else{'FAIL'})) -ForegroundColor $(if($verdict){'Green'}else{'Red'})
Write-Host "  Report: $ReportPath" -ForegroundColor Cyan
Write-Host ""
