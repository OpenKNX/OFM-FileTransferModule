#!/usr/bin/env pwsh
<#
Open ■
┬────┴  New-DeltaTestPair
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: OFM-FileTransferModule/scripts/New-DeltaTestPair.ps1

.SYNOPSIS
    Builds and ARCHIVES firmware images so a delta test can be repeated later.

.DESCRIPTION
    A delta test needs two images: the one a device is running and the one it should end up with. Both
    have to be kept, because a build is NOT reproducible -- BUILD_DATETIME is regenerated every time, so
    "rebuild the old one" produces a third image, not the first one again.

    Each call builds one environment and stores the result under a label, together with its size and
    checksum. Two calls make a pair. With -Pair the script does both in one go, which is the sharpest
    case: identical source, so the patch contains only what the build itself varies.

    The deeper stages of the test plan (a changed string, a function inserted early in the link order)
    are made by editing the source BETWEEN two calls -- the script does not touch anyone's code.

.PARAMETER Environment
    PlatformIO environment to build, e.g. release_REG2_PICO_ETH_DD.

.PARAMETER ProjectDir
    Directory containing platformio.ini. Defaults to the current directory.

.PARAMETER OutDir
    Where the archive lives. Each label becomes one .bin plus one .txt with size and checksum.

.PARAMETER Label
    Name for this image, e.g. "a" or "1.1.0".

.PARAMETER Pair
    Build twice in a row and store both as <Label>-a and <Label>-b.

.EXAMPLE
    ./New-DeltaTestPair.ps1 -Environment release_REG2_PICO_ETH_DD -Label t-a -Pair

.EXAMPLE
    ./New-DeltaTestPair.ps1 -Environment release_REG2_PICO_ETH_DD -Label before
    # ... edit a source file ...
    ./New-DeltaTestPair.ps1 -Environment release_REG2_PICO_ETH_DD -Label after
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Environment,
    [string]$ProjectDir = (Get-Location).Path,
    [string]$OutDir = (Join-Path (Get-Location).Path 'delta-pairs'),
    [Parameter(Mandatory = $true)][string]$Label,
    [switch]$Pair
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-CrcPosix {
    param([byte[]]$Data)
    [uint32]$crc = 0
    foreach ($byte in $Data) {
        $crc = $crc -bxor ([uint32]$byte -shl 24)
        for ($b = 0; $b -lt 8; $b++) {
            if ($crc -band 0x80000000) { $crc = (($crc -shl 1) -bxor 0x04C11DB7) -band 0xFFFFFFFF }
            else { $crc = ($crc -shl 1) -band 0xFFFFFFFF }
        }
    }
    return ((-bnot $crc) -band 0xFFFFFFFF)
}

function Save-Image {
    param([string]$Tag)
    Push-Location $ProjectDir
    try {
        & pio run -e $Environment | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "build of $Environment failed" }
    }
    finally { Pop-Location }

    $built = Join-Path $ProjectDir ".pio/build/$Environment/firmware.bin"
    if (-not (Test-Path $built)) { throw "no firmware.bin for $Environment" }

    $dest = Join-Path $OutDir "$Tag.bin"
    Copy-Item $built $dest -Force
    $bytes = [System.IO.File]::ReadAllBytes($dest)

    # The checksum is slow in PowerShell, so it covers the first 64 KB only: enough to tell two archived
    # images apart, and the real verification happens on the device anyway.
    $head = if ($bytes.Length -gt 65536) { $bytes[0..65535] } else { $bytes }
    $crc = Get-CrcPosix -Data $head

    $meta = @(
        "environment : $Environment",
        "bytes       : $($bytes.Length)",
        "crc(64k)    : 0x{0:X8}" -f $crc,
        "built       : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    )
    $meta | Set-Content -Path (Join-Path $OutDir "$Tag.txt")
    Write-Host ("  archived  {0,-14} {1,9} B" -f $Tag, $bytes.Length) -ForegroundColor Green
    return $dest
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Write-Host ''
Write-Host "  building $Environment" -ForegroundColor Cyan

if ($Pair) {
    $a = Save-Image -Tag "$Label-a"
    $b = Save-Image -Tag "$Label-b"
    Write-Host ''
    Write-Host '  next:' -ForegroundColor Cyan
    Write-Host "    ./Invoke-DeltaSelfTest.ps1 -Old $a -New $b"
}
else {
    Save-Image -Tag $Label | Out-Null
}
Write-Host ''
