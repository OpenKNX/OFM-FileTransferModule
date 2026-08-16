#!/usr/bin/env pwsh
<#
Open ■
┬────┴  Test-FtcStress
■ KNX   2025 OpenKNX - Erkan Çolak

FILEPATH: OFM-FileTransferModule/scripts/Test-FtcStress.ps1

.SYNOPSIS
    Full FTC stress + regression suite: drives one OpenKNX device's console and exercises the
    whole ftc client against a target -- presence/info, deliberate error inputs, file ops, all
    transfer modes, downloads to SD and LittleFS, and the interactive robustness paths (status
    during a transfer, cancel mid-transfer, TP-loss via `bcu rst` -> resume). Optionally finishes
    a real upload and triggers the on-target firmware update. Writes a Markdown report + verdict.

.DESCRIPTION
    Phases: A presence/info . B error handling (nonexistent file / bad backend, MUST abort, never
    a test pattern) . C file ops (mkdir/ll/rmdir) . D perf safe/fast/forget/keep . E1 downloads to
    SD + LittleFS (small file) . F interactive: status-during, cancel, bcu-rst resume . E2 real fw
    upload (fills the FS) . H space guard (evil fill -> request > free -> MUST abort cleanly) . G
    (opt) fwupdate. Frees the target FS first + cleans its junk between phases. Opens the console
    reset-free; reads until a completion marker (transfers have gaps). Cross-platform via .NET SerialPort.

.PARAMETER Port     Console serial port of the ftc CLIENT. Mandatory.
.PARAMETER Target   KNX PA of the target. Default 5.0.3.
.PARAMETER Fw       Client source path for the upload/download tests. Default sd/KNeoPix-RP2350.bin.gz.
.PARAMETER Dtr      Pass for RP native USB-CDC (assert DTR); omit for ESP/CH340.
.PARAMETER DoFwUpdate  Run phase G (complete upload + fwupdate -> target reboots + reflashes). Default ON.
.PARAMETER DoFormat    Include a `format yes` test (ERASES the target filesystem!). Default OFF.
.PARAMETER NoFile      Client has no SD/efc and no real firmware (e.g. REG1): run the firmware-less subset --
                       perf (RAM pattern = the throughput A/B), downloads to LittleFS, status/cancel/resume.
                       Skips the real upload, SD download and fwupdate. No source file needed.
.PARAMETER SeedFromTarget  Opt-in extra: first DOWNLOAD the fw from the target into the client's LittleFS, then
                       run upload-from-LittleFS + fwupdate from that copy. Lets a diskless client (REG1) still do
                       the full end-to-end + a big-download sample. Needs the client LittleFS to fit the fw.
.PARAMETER ReportPath  Markdown report. Default ./ftc-stressreport-<timestamp>.md.
.PARAMETER Baud     Default 115200.

.EXAMPLE
    ./Test-FtcStress.ps1 -Port /dev/cu.usbmodem84101 -Dtr -Target 5.0.3
.EXAMPLE
    # REG1 (no SD): firmware-less perf run for the 38400-BCU throughput A/B
    ./Test-FtcStress.ps1 -Port /dev/cu.wchusbserial8430 -Dtr -Target 5.0.3 -NoFile
.EXAMPLE
    # REG1 full end-to-end: seed the fw from the target into LittleFS, then upload-back + fwupdate
    ./Test-FtcStress.ps1 -Port /dev/cu.wchusbserial8430 -Dtr -Target 5.0.3 -NoFile -SeedFromTarget
#>
param(
    [Parameter(Mandatory = $true)] [string] $Port,
    [string] $Target = "5.0.3",
    [string] $Fw = "sd/KNeoPix-RP2350.bin.gz",
    [switch] $Dtr,
    [bool] $DoFwUpdate = $true,
    [switch] $DoFormat,
    [switch] $NoFile,          # client has NO SD/efc + no real fw (e.g. REG1): run the firmware-less subset via perf (RAM pattern)
    [switch] $SeedFromTarget,  # (opt-in) first DOWNLOAD the fw from the target into the client's LittleFS, then run
                               # upload-from-LittleFS + fwupdate from that copy -- a big-download test on a diskless client
    [string] $ReportPath,
    [int] $Baud = 115200
)

function OpenKNX_ShowLogo($t = $null) {
    Write-Host ""; Write-Host "Open " -NoNewline; Write-Host "$([char]::ConvertFromUtf32(0x25A0))" -ForegroundColor Green
    $u = "$([char]::ConvertFromUtf32(0x252C))$([char]::ConvertFromUtf32(0x2500))$([char]::ConvertFromUtf32(0x2500))$([char]::ConvertFromUtf32(0x2500))$([char]::ConvertFromUtf32(0x2500))$([char]::ConvertFromUtf32(0x2534)) "
    if ($t) { Write-Host "$u $t" -ForegroundColor Green } else { Write-Host $u -ForegroundColor Green }
    Write-Host "$([char]::ConvertFromUtf32(0x25A0))" -NoNewline -ForegroundColor Green; Write-Host " KNX"; Write-Host ""
}
function CleanConsole([string] $raw) {
    $t = ([regex]::Replace($raw, "`e\[[0-9;]*[A-Za-z]", "")) -replace "`r", ""
    (($t -split "`n") | ForEach-Object { $m = [regex]::Match($_, '(\d{2}:\d{2}:\d{2}:\s.*)$'); if ($m.Success) { $m.Groups[1].Value } }) -join "`n"
}
# send a command, read until -Until (regex) or idle (no -Until) or -Sec cap
function Ask([System.IO.Ports.SerialPort]$sp, [string]$cmd, [int]$sec = 8, [string]$until, [int]$idle = 1500) {
    $sp.DiscardInBuffer(); $sp.WriteLine($cmd)
    $sb = [System.Text.StringBuilder]::new(); $end = (Get-Date).AddSeconds($sec); $last = Get-Date
    while ((Get-Date) -lt $end) {
        Start-Sleep -Milliseconds 130; $c = $sp.ReadExisting()
        if ($c) { [void]$sb.Append($c); $last = Get-Date; if ($until -and $sb.ToString() -match $until) { Start-Sleep -Milliseconds 500; [void]$sb.Append($sp.ReadExisting()); break } }
        elseif (-not $until -and ((Get-Date) - $last).TotalMilliseconds -gt $idle) { break }
    }
    return $sb.ToString()
}
# start $startCmd, pump $injectAt seconds, send $injectCmd, pump until $until or $maxSec
function Interact([System.IO.Ports.SerialPort]$sp, [string]$startCmd, [int]$injectAt, [string]$injectCmd, [string]$until, [int]$maxSec) {
    $sp.DiscardInBuffer(); $sp.WriteLine($startCmd)
    $sb = [System.Text.StringBuilder]::new(); $t0 = Get-Date; $end = $t0.AddSeconds($maxSec); $done = $false
    while ((Get-Date) -lt $end) {
        Start-Sleep -Milliseconds 130; $c = $sp.ReadExisting(); if ($c) { [void]$sb.Append($c) }
        $el = ((Get-Date) - $t0).TotalSeconds
        if (-not $done -and $el -ge $injectAt) { [void]$sb.AppendLine("===INJECT '$injectCmd' @$([int]$el)s==="); $sp.WriteLine($injectCmd); $done = $true }
        if ($done -and $until -and $sb.ToString() -match $until) { Start-Sleep -Milliseconds 700; [void]$sb.Append($sp.ReadExisting()); break }
    }
    return $sb.ToString()
}
# poll `ftc s` until the client is not running a job (phase idle/done/failed) -> avoids "busy" collisions between tests
function WaitIdle([System.IO.Ports.SerialPort]$sp, [int]$sec = 30) {
    $end = (Get-Date).AddSeconds($sec)
    while ((Get-Date) -lt $end) {
        $r = Ask $sp "ftc s" 4 "status:"
        if ($r -match "status:\s+(idle|done|failed)") { return $true }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

$remote = $Fw -replace '^(sd|efc)/', ''; if (-not $remote.StartsWith('/')) { $remote = "/$remote" }
# mode flags: -SeedFromTarget provides a LittleFS source by downloading the fw from the target first.
$noSD = $NoFile -or $SeedFromTarget                    # this client has no SD/efc backend
$fileBound = (-not $NoFile) -or $SeedFromTarget        # run the real upload + fwupdate (need a source file)?
$srcFw = if ($SeedFromTarget) { $remote } else { $Fw } # source for the real upload (LittleFS copy when seeding)
if (-not $ReportPath) { $ReportPath = Join-Path $PSScriptRoot ("ftc-stressreport-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".md") }

OpenKNX_ShowLogo "FTC STRESS Suite"
$mode = if ($SeedFromTarget) { "NoSD+seed-from-target" } elseif ($NoFile) { "NoFile (perf-only)" } else { "full (SD source)" }
Write-Host "  Console $Port (Dtr=$([bool]$Dtr))  Target $Target  mode $mode  src $srcFw  remote $remote  fwupdate=$DoFwUpdate format=$([bool]$DoFormat)" -ForegroundColor Cyan
Write-Host ""

$sp = [System.IO.Ports.SerialPort]::new($Port, $Baud)
$sp.DtrEnable = [bool]$Dtr; $sp.RtsEnable = $false; $sp.NewLine = "`n"; $sp.ReadTimeout = 500
try { $sp.Open() } catch { Write-Host "  Cannot open $Port : $_" -ForegroundColor Red; exit 2 }
Start-Sleep -Milliseconds 400; $sp.DiscardInBuffer()

$results = @()
function Test($name, $raw, $pass, $note = "") {
    $clean = CleanConsole $raw
    # loop warnings, split by magnitude: >100ms = a REAL block (VORGABE); <=100ms = platform floor
    # (RP2040/RP2350 mDNS 30s re-announce via W5500/lwIP async IRQ + USB-CDC verbose backpressure, ~50-60ms).
    $warns = [regex]::Matches($clean, "took longer than usual \((\d+)")
    $lw = $warns.Count; $lwBig = 0
    foreach ($w in $warns) { if ([int]$w.Groups[1].Value -gt 100) { $lwBig++ } }
    # speed metrics from the result panel / throughput line (empty if not a transfer)
    $avg = ""; $peak = ""; $bytes = ""; $tm = ""
    $m = [regex]::Match($clean, "Throughput\s+(\d+)\s*B/s\s+\((\d+)\s*B\s+in\s+([\d.]+)\s*s")
    if ($m.Success) { $avg = $m.Groups[1].Value; $bytes = $m.Groups[2].Value; $tm = $m.Groups[3].Value + "s" }
    if (-not $avg) { $m = [regex]::Match($clean, "avg\s+(\d+)\s*B/s"); if ($m.Success) { $avg = $m.Groups[1].Value } }
    $m = [regex]::Match($clean, "peak\s+(\d+)"); if ($m.Success) { $peak = $m.Groups[1].Value }
    if (-not $bytes) { $m = [regex]::Match($clean, "Bytes\s+(\d+)"); if ($m.Success) { $bytes = $m.Groups[1].Value } }
    if (-not $tm) { $m = [regex]::Match($clean, "Time\s+(\d+m\d+s|\d+\.\d+\s*s)"); if ($m.Success) { $tm = ($m.Groups[1].Value -replace '\s','') } }
    $script:results += [pscustomobject]@{ Name = $name; Pass = $pass; Note = $note; LoopWarn = $lw; LoopBig = $lwBig; Avg = $avg; Peak = $peak; Bytes = $bytes; Time = $tm; Out = $clean }
    $wtag = if ($lwBig -gt 0) { "  [BLOCK x$lwBig >100ms]" } elseif ($lw -gt 0) { "  [~50ms noise x$lw]" } else { "" }
    $stag = if ($avg) { "  ${avg} B/s" } else { "" }
    Write-Host ("  {0,-24} {1}{2}{3}{4}" -f $name, $(if ($pass) { "PASS" } else { "FAIL" }), $(if ($note) { "  ($note)" } else { "" }), $stag, $wtag) -ForegroundColor $(if ($pass) { "Green" } else { "Red" })
}

# ---- Preconditions: target reachable + FS ok + SOURCE FILE present -> else do NOT run the suite ----
Write-Host "-- Preconditions (check everything is there first)" -ForegroundColor Yellow
$pre = @()
$rp = Ask $sp "ftc $Target ping" 8
if ($rp -match "result=0x0|Response.*prop|alive|module") { Write-Host "  target $Target responds        ok" -ForegroundColor DarkGray } else { $pre += "target $Target does not respond" }
$rd = Ask $sp "ftc $Target df" 8
if ($rd -match "total|free") { Write-Host "  target filesystem              ok" -ForegroundColor DarkGray } else { $pre += "target df failed" }
if ($fileBound -and -not $SeedFromTarget) {                                         # default (SD): the source file must be present
    $rf = Ask $sp "ftc $Target upload $Fw fast" 10 "Source|cannot open|not available"   # probe the config box
    $null = Ask $sp "ftc c" 6                                                       # global cancel (`ftc c`, NOT `ftc <pa> cancel`) -- stop the probe upload
    if (($rf -match "Size\s+\d+" -and $rf -notmatch "generated test pattern") -or $rf -match "already up to date") {
        Write-Host "  source $Fw present     ok" -ForegroundColor DarkGray
    } else { $pre += "source '$Fw' not found on the client -- put it on SD/efc, or use -NoFile (perf-only) / -SeedFromTarget" }
} elseif ($SeedFromTarget) {
    Write-Host "  source: will SEED from target ($remote -> client LittleFS)" -ForegroundColor DarkGray
} else {
    Write-Host "  source: none needed (-NoFile perf run)" -ForegroundColor DarkGray
}
if ($pre.Count -gt 0) {
    $sp.Close(); Write-Host ""; Write-Host "  PRECONDITION FAILED -- suite NOT run:" -ForegroundColor Red
    $pre | ForEach-Object { Write-Host "    - $_" -ForegroundColor Red }; Write-Host ""; exit 3
}
Write-Host ""

# ---- Setup: make the target idle + free its FS (a leftover fw fill from a prior run breaks mkdir/perf) ----
Write-Host "-- Setup: idle + free target FS (prior-run junk breaks mkdir/perf)" -ForegroundColor Yellow
$null = Ask $sp "ftc c" 4                                                     # cancel anything the precondition probe left running
if (-not $SeedFromTarget) { $null = Ask $sp "ftc $Target rm $remote" 12 "ok|removed|deleted|not found|done" }  # keep it if we seed from it
$null = Ask $sp "ftc $Target rm /ftcperf.bin" 6
$null = Ask $sp "ftc $Target rmdir /ftcstress" 6
Write-Host ""

# ---- Seed (opt-in): copy the fw off the target into the client LittleFS, then free the target for the rest ----
# result: a real 608KB DOWNLOAD (target->client) now + a real 608KB UPLOAD-back from LittleFS in E2 = full round-trip.
if ($SeedFromTarget) {
    Write-Host "-- Seed: download the fw from the target into the client LittleFS (big-download test)" -ForegroundColor Yellow
    [void](WaitIdle $sp 20)
    $rs = Ask $sp "ftc $Target receive $remote $remote" 2400 "DOWNLOAD COMPLETE|verified OK|not enough space|abort|fail|error"
    Test "seed-download-to-LittleFS" $rs ($rs -match "DOWNLOAD COMPLETE|verified OK") "target $remote -> client LittleFS"
    [void](WaitIdle $sp 20)
    if ($rs -match "DOWNLOAD COMPLETE|verified OK") {
        $null = Ask $sp "ftc $Target rm $remote" 12 "ok|removed|not found|error"   # free the target -> E2 re-uploads from LittleFS (real 608KB)
    } else {
        $fileBound = $false                                                        # client LittleFS too small / seed failed -> skip file-bound phases
        Write-Host "  seed FAILED (client LittleFS too small for the fw?) -> skipping real-upload + fwupdate" -ForegroundColor Red
    }
    Write-Host ""
}

# ---- Phase A: presence & info ----
Write-Host "-- Phase A: presence & info" -ForegroundColor Yellow
$r = Ask $sp "ftc ?" 6;                 Test "help" $r ($r -match "send \| upload|Transfer")
$r = Ask $sp "ftc $Target ping" 8;      Test "ping" $r ($r -match "result=0x0|Response.*prop|alive|module")
$r = Ask $sp "ftc $Target info" 12;     Test "info" $r ($r -match "Mask|Manufacturer|class")
$r = Ask $sp "ftc $Target info ga" 15;  Test "info-ga" $r ($r -match "GA|group|assoc|table|com")
$r = Ask $sp "ftc $Target df" 8;        Test "df" $r ($r -match "total|free|used")
$r = Ask $sp "ftc $Target ll" 12 "total|---|CRC"; Test "ll" $r ($r -match "CRC|dir|total|B")
$r = Ask $sp "ftc $Target ls" 10;       Test "ls" $r ($r -match "\d|total|Name|dir")

# ---- Phase B: deliberate error inputs (MUST abort cleanly, NEVER a test pattern) ----
Write-Host "-- Phase B: error handling (deliberate wrong inputs)" -ForegroundColor Yellow
$r = Ask $sp "ftc $Target upload sd/__nope__.bin fast" 10 "abort|cannot open|not available|backend"
Test "err-upload-missing" $r (($r -match "abort|cannot open|not available|backend") -and ($r -notmatch "generated test pattern")) "no pattern on a missing/absent-backend source"
$r = Ask $sp "ftc $Target upload zzz/foo.bin fast" 10 "abort|unknown backend|cannot"
Test "err-bad-backend" $r (($r -match "abort|unknown backend") -and ($r -notmatch "generated test pattern"))
$r = Ask $sp "ftc $Target info /__nope__.bin" 10 "not found|0x42|no such|error|does not"
Test "err-info-missing" $r ($r -match "not found|0x42|no such|error|does not|fail")
$r = Ask $sp "ftc $Target receive /__nope__.bin /x.bin" 12 "abort|not found|fail|error|0x42"   # /-sink works on any client
Test "err-download-missing" $r ($r -match "abort|not found|fail|error|0x42")

# ---- Phase C: file ops ----
Write-Host "-- Phase C: file ops (mkdir/ll/rmdir)" -ForegroundColor Yellow
$r = Ask $sp "ftc $Target mkdir /ftcstress" 8 "ok|created|done|exists";  Test "mkdir" $r ($r -match "ok|created|done|exists")
$r = Ask $sp "ftc $Target ll" 12 "total|---";                            Test "ll-sees-dir" $r ($r -match "ftcstress")
$r = Ask $sp "ftc $Target rmdir /ftcstress" 8 "ok|removed|done";         Test "rmdir" $r ($r -match "ok|removed|done")

# ---- Phase D: perf modes (safe/fast/forget/keep) -- target has room now ----
Write-Host "-- Phase D: transfer modes (perf safe/fast/forget/keep)" -ForegroundColor Yellow
$r = Ask $sp "ftc $Target perf 30" 180 "COMPLETE|nothing to send|avg .*B/s|aborting";              Test "perf-safe" $r ($r -match "B/s|COMPLETE")
$r = Ask $sp "ftc $Target perf 50 253 fast" 240 "COMPLETE|nothing to send|avg .*B/s|aborting";     Test "perf-fast" $r ($r -match "B/s|COMPLETE")
$r = Ask $sp "ftc $Target perf 50 253 forget" 240 "COMPLETE|nothing to send|avg .*B/s|aborting";   Test "perf-forget" $r ($r -match "B/s|COMPLETE")
$rk = Ask $sp "ftc $Target perf 20 253 fast keep" 180 "COMPLETE|kept|already up|avg .*B/s|aborting"; Test "perf-keep" $rk ($rk -match "B/s|COMPLETE|kept|already up")
$keepFile = ""; $mk = [regex]::Match((CleanConsole $rk), "(/ftcperf_[0-9A-Fa-f]+\.bin)"); if ($mk.Success) { $keepFile = $mk.Groups[1].Value }

# ---- Phase E1: downloads to SD + LittleFS (pull the SMALL perf-keep file, not the 622KB fw -> fast) ----
# each download MUST run to completion (terminal marker, NOT "B/s" which matches the first progress line
# and would let the next command collide -> "busy"); WaitIdle guarantees the job is done before the next.
Write-Host "-- Phase E1: downloads (SD + LittleFS) from the small perf file" -ForegroundColor Yellow
[void](WaitIdle $sp 20)
$dlsrc = if ($keepFile) { $keepFile } else { "/ftcperf.bin" }
if (-not $noSD) {
    $r = Ask $sp "ftc $Target receive $dlsrc sd/ftcdl.bin" 150 "DOWNLOAD COMPLETE|verified OK|abort|fail|error"
    Test "download-to-SD" $r ($r -match "DOWNLOAD COMPLETE|verified OK") "src $dlsrc"
    [void](WaitIdle $sp 20)
} else { Write-Host "  download-to-SD SKIPPED (this client has no SD)" -ForegroundColor DarkGray }
$r = Ask $sp "ftc $Target receive $dlsrc /ftcdl.bin" 120 "DOWNLOAD COMPLETE|verified OK|not enough space|space|abort|fail|error"
Test "download-to-LittleFS" $r ($r -match "DOWNLOAD COMPLETE|verified OK|not enough space|space") "success or client-space-gate"
[void](WaitIdle $sp 20)

# ---- Phase F: interactive robustness (perf 100 so it's interruptible; cancel is GLOBAL `ftc c`) ----
Write-Host "-- Phase F: robustness (status / cancel / bcu-rst resume)" -ForegroundColor Yellow
[void](WaitIdle $sp 20)
$r = Interact $sp "ftc $Target perf 100 253 fast" 10 "ftc s" "status:\s+\w" 180   # `ftc s` line, not the perf progress
Test "status-during" $r (($r -match "status:\s+(upload|verify|done)") -and ($r -match "%|chunk"))
$rc = Ask $sp "ftc c" 8 "cancel|idle|nothing"                              # GLOBAL cancel (NOT `ftc <pa> cancel`)
Test "cancel-after-status" $rc ($rc -match "cancel|idle|nothing")
[void](WaitIdle $sp 20)
$r = Interact $sp "ftc $Target perf 100 253 fast" 12 "ftc c" "cancel|idle|abort|nothing" 120
Test "cancel-mid-transfer" $r ($r -match "cancel|idle|abort")
$null = Ask $sp "ftc c" 4; [void](WaitIdle $sp 20)                          # settle before rm (rm is itself a job -> would be "busy")
$null = Ask $sp "ftc $Target rm /ftcperf.bin" 10 "ok|removed|not found|error"
[void](WaitIdle $sp 20)                                                     # <- the missing guard: perf must not start while rm still runs
# TP loss via bcu rst -> resume: fresh perf, reset the client BCU mid-transfer, expect resume + finish, 0 REAL blocks
$r = Interact $sp "ftc $Target perf 80 253 fast" 20 "bcu rst" "verified OK|COMPLETE|aborting" 360
$resumed = ($r -match "resuming|matching \d+ B partial")
$bwarns = [regex]::Matches((CleanConsole $r), "took longer than usual \((\d+)"); $blocked = 0
foreach ($w in $bwarns) { if ([int]$w.Groups[1].Value -gt 100) { $blocked++ } }   # >100ms only; ~50ms platform floor is ignored
Test "bcu-rst-resume" $r (($r -match "verified OK|COMPLETE") -and $resumed -and ($blocked -eq 0)) "resumed=$resumed FTC-blocks(>100ms)=$blocked"
$null = Ask $sp "ftc c" 4; [void](WaitIdle $sp 20)

# ---- clear all perf junk so the target has room for the full fw fill ----
Write-Host "-- cleanup: remove perf junk before the fill" -ForegroundColor DarkYellow
$null = Ask $sp "ftc $Target rm /ftcperf.bin" 6
if ($keepFile) { $null = Ask $sp "ftc $Target rm $keepFile" 6 }

# ---- Phase E2: real fw upload (fills the FS) -- the real end-to-end AND the fill for the space-guard ----
# skipped in -NoFile (no source); in -SeedFromTarget the source is the LittleFS copy -> a real 608KB upload-back.
if ($fileBound) {
    Write-Host "-- Phase E2: real fw upload from $srcFw (fills the target FS, slow over TP)" -ForegroundColor Yellow
    $r = Ask $sp "ftc $Target upload $srcFw fast verbose" 2400 "COMPLETE|verified OK|already up to date|nothing to send|aborting"
    Test "upload-real-fast" $r (($r -match "COMPLETE|verified OK|already up to date") -and ($r -notmatch "generated test pattern")) "src $srcFw, no pattern"
} else { Write-Host "-- Phase E2 SKIPPED (no source file -- -NoFile perf run)" -ForegroundColor DarkGray }

# ---- Phase H: SPACE GUARD (evil-but-necessary) -- only meaningful when the target is nearly full, so a
# client-RAM-safe perf (<=200KB) can over-ask its free space. Auto-skips when the target has room (e.g. -NoFile
# run with no fill). Ran for real in full/seed runs (E2 filled the FS).
$rdf = Ask $sp "ftc $Target df" 8
$freeB = 0; $mf = [regex]::Match((CleanConsole $rdf), "free\D*?(\d+)"); if ($mf.Success) { $freeB = [int64]$mf.Groups[1].Value }
if ($freeB -gt 0 -and $freeB -lt 140 * 1024) {
    Write-Host "-- Phase H: space guard (target nearly full -> request > free -> must abort, no partial)" -ForegroundColor Yellow
    $needKB = [math]::Min(200, [math]::Floor($freeB / 1024) + 64)                 # over free, but client-RAM-safe
    $r = Ask $sp "ftc $Target perf $needKB 253 fast" 40 "not enough space|NOT ENOUGH SPACE|aborting|COMPLETE|verified OK"
    Test "space-guard-aborts" $r (($r -match "not enough space") -and ($r -notmatch "COMPLETE|verified OK")) "free=${freeB}B asked=${needKB}KB -> clean abort, no partial"
    $null = Ask $sp "ftc c" 4; $null = Ask $sp "ftc $Target rm /ftcperf.bin" 6   # drop any partial the guard test left
} else {
    Write-Host "-- Phase H SKIPPED: target free=$freeB B too large to over-ask within client RAM (needs a full-FS fill)" -ForegroundColor DarkGray
}

# ---- optional: format (ERASES target FS) ----
if ($DoFormat) {
    Write-Host "-- format (DESTRUCTIVE)" -ForegroundColor Red
    $r = Ask $sp "ftc $Target format yes" 60 "ok|formatted|done|erased|abort"; Test "format" $r ($r -match "ok|formatted|done|erased")
}

# ---- Phase G: fwupdate (fw is on the target from Phase E2 -> up to date, then trigger the apply) ----
if ($DoFwUpdate -and $fileBound) {
    Write-Host "-- Phase G: fwupdate (target reboots + reflashes)" -ForegroundColor Yellow
    $r = Ask $sp "ftc $Target upload $srcFw fast verbose" 300 "COMPLETE|verified OK|already up to date|nothing to send|aborting"
    Test "upload-complete" $r ($r -match "COMPLETE|verified OK|already up to date")
    $r = Ask $sp "ftc $Target fwupdate $remote" 30 "armed|reboot|restart|apply|initiated|Firmware"
    Test "fwupdate-trigger" $r ($r -match "armed|reboot|restart|apply|initiated|Firmware")
} elseif ($fileBound) {
    # no fwupdate -> don't leave the big test fw filling the target FS
    Write-Host "-- cleanup: remove the test fw from the target (no fwupdate)" -ForegroundColor DarkYellow
    $null = Ask $sp "ftc $Target rm $remote" 12
} else {
    Write-Host "-- Phase G SKIPPED (no source file -- -NoFile perf run)" -ForegroundColor DarkGray
}

$sp.Close()

# ---- report ----
# global non-blocking verdict (VORGABE): only a REAL block fails. block = loop-warning >100ms (a blocking
# CRC/IO pass is 100s-of-ms..seconds; the old resume-CRC bug was 1660ms). Warnings <=100ms are the RP2040/
# RP2350 platform floor (mDNS 30s re-announce via W5500/lwIP async IRQ + USB-CDC verbose backpressure) that
# fire ~every 30s regardless of FTC -> reported as noise, NOT a suite failure.
$xf = $results | Where-Object { $_.Name -match 'perf|upload|download|resume|guard|status|cancel' }
$xw = ($xf | Measure-Object -Property LoopWarn -Sum).Sum; if (-not $xw) { $xw = 0 }
$xb = ($xf | Measure-Object -Property LoopBig -Sum).Sum; if (-not $xb) { $xb = 0 }
$noise = $xw - $xb
$results += [pscustomobject]@{ Name = "NON-BLOCKING (FTC blocks >100ms)"; Pass = ($xb -eq 0); Note = "$xb real FTC block(s); $noise platform ~50ms warnings (mDNS/USB, not FTC)"; LoopWarn = $xw; LoopBig = $xb; Avg = ""; Peak = ""; Bytes = ""; Time = ""; Out = "" }

$pass = ($results | Where-Object Pass).Count; $total = $results.Count
$md = @("# FTC Stress Suite Report", "",
    "- Date: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')   Console: ``$Port`` (Dtr=$([bool]$Dtr))   Target: **$Target**",
    "- src ``$Fw``   remote ``$remote``   fwupdate=$DoFwUpdate  format=$([bool]$DoFormat)",
    "- Result: **$pass / $total passed**   Non-blocking: **$(if($xb -eq 0){'CLEAN (0 FTC blocks >100ms)'}else{"$xb FTC block(s)!"})**   platform noise: $noise x ~50ms (mDNS/USB, not FTC)",
    "", "| Test | Result | Warn | Block>100ms | Note |", "|---|---|---|---|---|")
foreach ($x in $results) { $md += "| $($x.Name) | $(if($x.Pass){'PASS'}else{'FAIL'}) | $($x.LoopWarn) | $($x.LoopBig) | $($x.Note) |" }
$md += @("", "## Speed Report (throughput per transfer -- for comparison)", "", "| Test | Bytes | Time | avg B/s | peak B/s |", "|---|---|---|---|---|")
foreach ($x in ($results | Where-Object { $_.Avg })) { $md += "| $($x.Name) | $($x.Bytes) | $($x.Time) | **$($x.Avg)** | $($x.Peak) |" }
$md += ""
foreach ($x in $results) { $md += "## $($x.Name)  ($(if($x.Pass){'PASS'}else{'FAIL'}))"; $md += '```'; $md += $x.Out; $md += '```'; $md += "" }
Set-Content -LiteralPath $ReportPath -Value ($md -join "`n") -Encoding utf8

Write-Host ""; Write-Host "  --- Speed (B/s) ---" -ForegroundColor Cyan
foreach ($x in ($results | Where-Object { $_.Avg })) { Write-Host ("    {0,-24} {1,7} B/s  peak {2,-6} {3} B  {4}" -f $x.Name, $x.Avg, $x.Peak, $x.Bytes, $x.Time) }
Write-Host ""
Write-Host ("  ===== $pass / $total passed   non-blocking: $(if($xb -eq 0){'CLEAN (0 FTC blocks)'}else{"$xb FTC block(s)!"})   [platform noise $noise x ~50ms mDNS/USB] =====") -ForegroundColor $(if ($pass -eq $total -and $xb -eq 0) { "Green" } else { "Yellow" })
Write-Host "  Report: $ReportPath" -ForegroundColor Cyan; Write-Host ""
