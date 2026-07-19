#!/usr/bin/env pwsh
<#
Open ■
┬────┴  Compress-Firmware
■ KNX   2025 OpenKNX - Erkan Çolak

FILEPATH: OFM-FileTransferModule/scripts/Compress-Firmware.ps1

.SYNOPSIS
    Gzip a firmware image for FTC KNX-bus upload + on-target apply.

.DESCRIPTION
    The RP2040/RP2350 bootloader (PicoOTA / uzlib) ungzips a staged image on the
    next boot, so `ftc <pa> send ... apply` should push a .gz -- smaller on the
    slow KNX-TP wire. This is the platform-independent equivalent of what the PC
    KnxFileTransfer client does with a .bin (.bin -> gzip). Uses .NET GZipStream,
    no external tools; runs on Windows, macOS and Linux.

    NOTE: gzip + on-target ungzip is RP2040 / RP2350 only. An ESP32 target now
    self-applies over the bus too, but only a RAW .bin (no gzip) via Update/OTA.

.PARAMETER InputPath
    The firmware image to compress (e.g. firmware.bin). Mandatory.

.PARAMETER OutputPath
    Output path. Default: <InputPath>.gz

.PARAMETER Fast
    Quicker compression at a larger size (default = smallest).

.EXAMPLE
    ./Compress-Firmware.ps1 firmware.bin
    Compress to firmware.bin.gz (best compression).

.EXAMPLE
    ./Compress-Firmware.ps1 firmware.bin out.bin.gz -Fast
    Explicit output name, faster / less compression.
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $InputPath,
    [Parameter(Position = 1)]
    [string] $OutputPath,
    [switch] $Fast
)

# Platform detection (Windows PowerShell 5 has no $IsWindows/$IsMacOS/$IsLinux)
if ($null -eq (Get-Variable -Name 'IsMacOS' -ErrorAction SilentlyContinue)) {
    $IsMacOS = $false; $IsLinux = $false; $IsWindows = $true
}

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

OpenKNX_ShowLogo "Compress-Firmware"

if (-not (Test-Path -PathType Leaf -LiteralPath $InputPath)) {
    Write-Host "Error: input file not found: $InputPath" -ForegroundColor Red
    exit 2
}
$in = Get-Item -LiteralPath $InputPath
if ($in.Length -eq 0) {
    Write-Host "Error: input file is empty: $InputPath" -ForegroundColor Red
    exit 2
}
if (-not $OutputPath) { $OutputPath = "$($in.FullName).gz" }

# Best compression by default (the KNX-TP wire is the bottleneck); -Fast = quicker.
$levelName = if ($Fast) { 'Optimal' }
             elseif ([enum]::GetNames([System.IO.Compression.CompressionLevel]) -contains 'SmallestSize') { 'SmallestSize' }
             else { 'Optimal' }
$level = [System.IO.Compression.CompressionLevel]::$levelName

$raw = [System.IO.File]::ReadAllBytes($in.FullName)
$outFs = [System.IO.File]::Create($OutputPath)
try {
    $gz = New-Object System.IO.Compression.GZipStream($outFs, $level)
    try     { $gz.Write($raw, 0, $raw.Length) }
    finally { $gz.Dispose() }   # flush + gzip trailer (CRC32 + ISIZE)
}
finally { $outFs.Dispose() }

# Sanity: standard gzip magic 1f 8b (what the target's uzlib looks for).
$fs = [System.IO.File]::OpenRead($OutputPath)
$b0 = $fs.ReadByte(); $b1 = $fs.ReadByte(); $fs.Dispose()
if ($b0 -ne 0x1f -or $b1 -ne 0x8b) {
    Write-Host "Error: output is not a valid gzip stream" -ForegroundColor Red
    exit 1
}

$n = $in.Length
$comp = (Get-Item -LiteralPath $OutputPath).Length
$ratio = [math]::Round(100.0 * $comp / $n, 1)
$base = Split-Path -Leaf $OutputPath

Write-Host ("  in    {0}    {1} B" -f $in.FullName, $n)
Write-Host ("  out   {0}    {1} B    ({2}% of original, {3})" -f $OutputPath, $comp, $ratio, $levelName) -ForegroundColor Green
Write-Host ("  ISIZE (uncompressed length the bootloader flashes) = {0} B" -f $n)
Write-Host ""
Write-Host "  upload + auto-apply:"
Write-Host ("    ftc <pa> send sd/{0} apply" -f $base) -ForegroundColor Cyan
Write-Host "  or in two steps:"
Write-Host ("    ftc <pa> send sd/{0}" -f $base) -ForegroundColor Cyan
Write-Host ("    ftc <pa> fwupdate /{0}" -f $base) -ForegroundColor Cyan
