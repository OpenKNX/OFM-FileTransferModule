#!/usr/bin/env pwsh
<#
Open ■
┬────┴  Compare-Busmon
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: OFM-FileTransferModule/scripts/Compare-Busmon.ps1

.SYNOPSIS
    Dirty side-by-side check: capture RAW busmonitor LPDUs from TWO KNXnet/IP
    interfaces at the SAME TIME and report whether they differ.

.DESCRIPTION
    Both interfaces sit on the same TP1 line, so a faithful busmonitor must see
    the EXACT same raw traffic. This script opens a busmonitor tunnel to each
    (via the ftc-cli `bm` command, quiet mode -> `RAWHEX <tab> decode <tab> ACK`),
    runs BOTH captures concurrently for the same window, then does an EXACT
    multiset diff of the raw LPDUs (column 1, byte for byte):

      * total + distinct frames per side,
      * raw frames seen ONLY by one side,
      * raw frames both saw but a DIFFERENT number of times (count mismatch),
      * a verdict: EXACT match vs. differences found.

    Typical use: start this, then in another shell drive real traffic through
    ONE interface — e.g. `ftc -i <ip> <pa> send <file>` (an up/download). Both
    busmonitors must report the identical raw frames. Both raw captures are also
    written to files so you can inspect / re-diff them yourself.

    It is intentionally "dirty" — a quick conformance spot-check, not a rig test.
    Busmonitor is passive (no TX/ACK), so it does not disturb the bus. Each
    interface allows one busmonitor; the two targets are different devices, so
    both monitors run in parallel without fighting over the exclusive slot.
    Only the raw LPDU (column 1) is compared; the ACK/NAK column is derived from
    a separate deferred octet and is timing-sensitive, so it is left out.

.PARAMETER SiemensIp
    IP of the reference (Siemens) interface. Default 11.11.0.5.

.PARAMETER OurIp
    IP of our OpenKNX interface. Default 11.11.0.210 (the other one is 11.11.0.126).

.PARAMETER Seconds
    Capture window in seconds (both sides, concurrent). Default 20.

.PARAMETER FtcPath
    Path to the ftc-cli binary. Default: auto-detect the build for this OS/arch.

.PARAMETER ShowFrames
    List every differing raw frame (default: cap the list at 40 per side).

.PARAMETER Normalize
    Reassemble fragmented busmon indications back into whole telegrams before the
    diff, anchored on the TP1 FCS (per 03_06_03 §4.1.5.8.1 a canonical L_Busmon.ind
    carries one complete message with the FCS as its last octet). An interface with
    a smaller buffer/APDU may split one telegram into several "frame pieces" (a
    concept the spec explicitly allows via the busmon status Lost/Sequence flags) —
    this makes the two sides apples-to-apples. Frames whose FCS never checks out are
    kept and tagged (FCS?), never silently dropped.

.EXAMPLE
    ./Compare-Busmon.ps1
    ./Compare-Busmon.ps1 -OurIp 11.11.0.126 -Seconds 30 -ShowFrames
#>

[CmdletBinding()]
param(
    [string]$SiemensIp = '11.11.0.5',
    [string]$OurIp     = '11.11.0.210',
    [int]   $Seconds   = 20,
    [string]$FtcPath,
    [switch]$ShowFrames,
    [switch]$Normalize
)

$ErrorActionPreference = 'Stop'

# --- locate the ftc-cli binary for this OS/arch --------------------------------------------------
function Resolve-Ftc {
    param([string]$Explicit)
    if ($Explicit) {
        if (Test-Path $Explicit) { return (Resolve-Path $Explicit).Path }
        throw "ftc binary not found at -FtcPath '$Explicit'"
    }
    $os   = if ($IsWindows) { 'windows' } elseif ($IsMacOS) { 'macos' } else { 'linux' }
    $arch = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture) {
        'Arm64' { 'arm64' } 'X64' { 'x64' } 'X86' { 'x86' } default { 'x64' }
    }
    $exe  = if ($IsWindows) { 'ftc.exe' } else { 'ftc' }
    $base = Join-Path $PSScriptRoot '../ftc-cli/.pio/build'
    $cand = Join-Path $base "ftc-cli-$os-$arch/$exe"
    if (Test-Path $cand) { return (Resolve-Path $cand).Path }
    # fall back to any built binary, then PATH
    $any = Get-ChildItem -Path $base -Recurse -Filter $exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($any) { return $any.FullName }
    $onPath = Get-Command 'ftc' -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw "ftc binary not found — build it (pio run) or pass -FtcPath."
}

$ftc = Resolve-Ftc -Explicit $FtcPath

# --- capture one side in a background job (quiet busmon, bounded by --seconds) --------------------
$capture = {
    param($Ftc, $Ip, $Sec)
    # quiet -> "RAWHEX <tab> decode <tab> ACK/NAK"; --seconds makes it self-terminating (scriptable).
    & $Ftc -q -i $Ip bm --seconds $Sec 2>$null
}

Write-Host ""
Write-Host "  Open ■  Compare-Busmon" -ForegroundColor Green
Write-Host "  ┬────┴  Siemens $SiemensIp   vs   ours $OurIp   ($Seconds s, concurrent)" -ForegroundColor DarkGray
Write-Host "  ■ KNX" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  capturing…" -ForegroundColor DarkGray

# Start both near-simultaneously so they observe the same bus window.
$jS = Start-Job -ScriptBlock $capture -ArgumentList $ftc, $SiemensIp, $Seconds
$jO = Start-Job -ScriptBlock $capture -ArgumentList $ftc, $OurIp, $Seconds
$null = Wait-Job $jS, $jO -Timeout ($Seconds + 15)

$outS = @(Receive-Job $jS -ErrorAction SilentlyContinue)
$outO = @(Receive-Job $jO -ErrorAction SilentlyContinue)
Remove-Job $jS, $jO -Force -ErrorAction SilentlyContinue

# Persist the raw captures so they can be inspected / re-diffed by hand.
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$fileS   = Join-Path ([System.IO.Path]::GetTempPath()) "busmon_siemens_${SiemensIp}_$stamp.txt"
$fileO   = Join-Path ([System.IO.Path]::GetTempPath()) "busmon_ours_${OurIp}_$stamp.txt"
$outS | Set-Content -LiteralPath $fileS
$outO | Set-Content -LiteralPath $fileO

# --- extract raw LPDUs (column 1), normalised ----------------------------------------------------
function Get-Frames {
    param([string[]]$Lines)
    $frames = New-Object System.Collections.Generic.List[string]
    foreach ($ln in $Lines) {
        if ([string]::IsNullOrWhiteSpace($ln)) { continue }
        $raw = ($ln -split "`t")[0]
        if ([string]::IsNullOrWhiteSpace($raw)) { continue }
        # normalise: uppercase, single-spaced hex
        $norm = (($raw.Trim() -replace '\s+', ' ')).ToUpperInvariant()
        if ($norm -match '^[0-9A-F ]+$') { $frames.Add($norm) }
    }
    return $frames
}

# --- FCS-anchored reassembly (used only with -Normalize) -----------------------------------------
# TP1 frame check octet = bitwise complement of the XOR of all preceding octets. A frame is complete
# when its last octet equals that complement over the rest.
function Test-Fcs {
    param([byte[]]$Bytes)
    if ($Bytes.Count -lt 2) { return $false }
    $x = 0
    for ($i = 0; $i -lt $Bytes.Count - 1; $i++) { $x = $x -bxor $Bytes[$i] }
    return ((($x -bxor 0xFF) -band 0xFF) -eq $Bytes[$Bytes.Count - 1])
}
# Expected total length from the frame header: standard (ctrl bit7=1) = 8 + (len nibble);
# extended (ctrl bit7=0) = 8 + LG octet. 0 = not enough header bytes yet.
function Get-ExpectedLen {
    param([byte[]]$Acc)
    if ($Acc.Count -lt 1) { return 0 }
    if (($Acc[0] -band 0x80) -ne 0) {
        if ($Acc.Count -lt 6) { return 0 }
        return 8 + ($Acc[5] -band 0x0F)
    } else {
        if ($Acc.Count -lt 7) { return 0 }
        return 8 + $Acc[6]
    }
}
# Merge consecutive per-indication frames back into whole telegrams. Anchor: a valid TP1 FCS in a ±2
# window around the header-derived length (absorbs the LG off-by-one). If the header length is reached
# but no FCS matches, the front is a capture-start partial or a lost piece -> drop one byte and re-sync
# (byte-wise), so a leading fragment can never desync the whole stream. Returns the telegrams plus how
# many bytes were dropped (re-sync) and any incomplete tail — nothing is silently swallowed.
function Get-Telegrams {
    param([string[]]$Frames)
    $out = New-Object System.Collections.Generic.List[string]
    $acc = New-Object System.Collections.Generic.List[byte]
    $dropped = 0
    foreach ($f in $Frames) {
        foreach ($h in ($f -split ' ')) { if ($h -match '^[0-9A-Fa-f]{2}$') { $acc.Add([Convert]::ToByte($h, 16)) } }
        while ($acc.Count -gt 0) {
            $exp = Get-ExpectedLen $acc.ToArray()
            if ($exp -le 0 -or $acc.Count -lt ($exp - 2)) { break } # need more bytes
            $cut = -1
            foreach ($p in @($exp, ($exp - 1), ($exp + 1), ($exp - 2), ($exp + 2))) {
                if ($p -ge 8 -and $p -le $acc.Count) {
                    if (Test-Fcs $acc.GetRange(0, $p).ToArray()) { $cut = $p; break }
                }
            }
            if ($cut -ge 0) {
                $slice = $acc.GetRange(0, $cut).ToArray()
                $out.Add((($slice | ForEach-Object { $_.ToString('X2') }) -join ' '))
                $acc.RemoveRange(0, $cut)
            }
            elseif ($acc.Count -ge ($exp + 2)) { $acc.RemoveRange(0, 1); $dropped++ } # re-sync past a partial/lost byte
            else { break } # wait for more bytes
        }
    }
    return @{ T = $out; Dropped = $dropped; Tail = $acc.Count }
}

$framesS = Get-Frames $outS
$framesO = Get-Frames $outO

# FCS validity on the (whole) frames — a self-check: our whole-telegram side should be ~all valid.
function Get-FcsOkCount {
    param([string[]]$Frames)
    $ok = 0
    foreach ($f in $Frames) {
        $b = @()
        foreach ($h in ($f -split ' ')) { if ($h -match '^[0-9A-Fa-f]{2}$') { $b += [Convert]::ToByte($h, 16) } }
        if (Test-Fcs ([byte[]]$b)) { $ok++ }
    }
    return $ok
}
$fcsS_raw = Get-FcsOkCount $framesS
$fcsO_raw = Get-FcsOkCount $framesO
$rawCountS = $framesS.Count
$rawCountO = $framesO.Count

$normDropS = 0; $normDropO = 0; $normTailS = 0; $normTailO = 0
if ($Normalize) {
    $rS = Get-Telegrams $framesS; $framesS = @($rS.T); $normDropS = $rS.Dropped; $normTailS = $rS.Tail
    $rO = Get-Telegrams $framesO; $framesO = @($rO.T); $normDropO = $rO.Dropped; $normTailO = $rO.Tail
}

# distinct-frame multisets (count per unique raw LPDU)
function Get-Counts {
    param($Frames)
    $h = @{}
    foreach ($f in $Frames) { if ($h.ContainsKey($f)) { $h[$f]++ } else { $h[$f] = 1 } }
    return $h
}
$cntS = Get-Counts $framesS
$cntO = Get-Counts $framesO

$onlyS = $cntS.Keys | Where-Object { -not $cntO.ContainsKey($_) } | Sort-Object
$onlyO = $cntO.Keys | Where-Object { -not $cntS.ContainsKey($_) } | Sort-Object
$common = @($cntS.Keys | Where-Object { $cntO.ContainsKey($_) })
# common frames captured a DIFFERENT number of times on each side (exact-count mismatch)
$mismatch = @($common | Where-Object { $cntS[$_] -ne $cntO[$_] } | Sort-Object)

# --- report --------------------------------------------------------------------------------------
Write-Host ""
if ($Normalize) {
    Write-Host "  (normalised: fragmented indications reassembled into whole telegrams, FCS-anchored)" -ForegroundColor DarkGray
    Write-Host ("   re-sync dropped bytes  Siemens {0}  ours {1}   ·   incomplete tail  Siemens {2}  ours {3}" -f $normDropS, $normDropO, $normTailS, $normTailO) -ForegroundColor DarkGray
}
Write-Host ("  {0,-14} {1,7} frames   {2,5} distinct   (raw FCS-valid {3}/{4})" -f 'Siemens', $framesS.Count, $cntS.Count, $fcsS_raw, $rawCountS)
Write-Host ("  {0,-14} {1,7} frames   {2,5} distinct   (raw FCS-valid {3}/{4})" -f 'ours',    $framesO.Count, $cntO.Count, $fcsO_raw, $rawCountO)
Write-Host ("  {0,-14} {1,5} distinct" -f 'common', @($common).Count) -ForegroundColor DarkGray
Write-Host ""

if ($framesS.Count -eq 0) { Write-Host "  ! Siemens returned NO frames — unreachable, or its busmon slot is busy." -ForegroundColor Yellow }
if ($framesO.Count -eq 0) { Write-Host "  ! ours returned NO frames — unreachable, or its busmon slot is busy." -ForegroundColor Yellow }

$cap = if ($ShowFrames) { [int]::MaxValue } else { 40 }

function Show-Diff {
    param([string]$Title, [string[]]$Frames, [hashtable]$Counts, [int]$Cap, [ConsoleColor]$Color)
    if (-not $Frames -or $Frames.Count -eq 0) { return }
    Write-Host ("  {0}: {1} distinct raw frame(s)" -f $Title, $Frames.Count) -ForegroundColor $Color
    $shown = 0
    foreach ($f in $Frames) {
        if ($shown -ge $Cap) { Write-Host ("      … (+{0} more, use -ShowFrames)" -f ($Frames.Count - $Cap)) -ForegroundColor DarkGray; break }
        $n = if ($Counts.ContainsKey($f)) { $Counts[$f] } else { 0 }
        Write-Host ("      {0}   x{1}" -f $f, $n) -ForegroundColor $Color
        $shown++
    }
    Write-Host ""
}

Show-Diff -Title "ONLY Siemens saw" -Frames $onlyS -Counts $cntS -Cap $cap -Color Cyan
Show-Diff -Title "ONLY ours saw"    -Frames $onlyO -Counts $cntO -Cap $cap -Color Magenta

# common frames with a different capture count on each side (exact-fidelity mismatch)
if ($mismatch.Count -gt 0) {
    Write-Host ("  COUNT mismatch: {0} raw frame(s) seen a different number of times" -f $mismatch.Count) -ForegroundColor Yellow
    $shown = 0
    foreach ($f in $mismatch) {
        if ($shown -ge $cap) { Write-Host ("      … (+{0} more, use -ShowFrames)" -f ($mismatch.Count - $cap)) -ForegroundColor DarkGray; break }
        Write-Host ("      {0}   Siemens x{1}  ours x{2}" -f $f, $cntS[$f], $cntO[$f]) -ForegroundColor Yellow
        $shown++
    }
    Write-Host ""
}

Write-Host ("  raw captures: {0}" -f $fileS) -ForegroundColor DarkGray
Write-Host ("                {0}" -f $fileO) -ForegroundColor DarkGray
Write-Host ""

# --- verdict -------------------------------------------------------------------------------------
if ($framesS.Count -eq 0 -and $framesO.Count -eq 0) {
    Write-Host "  VERDICT: inconclusive — both sides captured nothing." -ForegroundColor Yellow
    exit 2
}
if (@($onlyS).Count -eq 0 -and @($onlyO).Count -eq 0 -and $mismatch.Count -eq 0) {
    Write-Host "  VERDICT: EXACT match — both busmonitors delivered byte-identical raw frames, same counts." -ForegroundColor Green
    exit 0
} else {
    Write-Host ("  VERDICT: DIFFERENCES — only-Siemens {0}, only-ours {1}, count-mismatch {2}." -f @($onlyS).Count, @($onlyO).Count, $mismatch.Count) -ForegroundColor Yellow
    if (-not $Normalize) {
        Write-Host "  If one side fragments long telegrams into 'frame pieces' (smaller buffer/APDU), re-run with" -ForegroundColor DarkGray
        Write-Host "  -Normalize to reassemble whole telegrams (FCS-anchored) and compare apples-to-apples." -ForegroundColor DarkGray
    }
    Write-Host "  (A few edge frames can differ if traffic straddled the start/stop; drive the transfer" -ForegroundColor DarkGray
    Write-Host "   fully inside the window and re-run. Inspect the raw capture files above to confirm.)" -ForegroundColor DarkGray
    exit 1
}
