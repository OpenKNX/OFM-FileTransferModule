#!/usr/bin/env pwsh
<#
Open ■
┬────┴  Invoke-DeltaSelfTest
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: OFM-FileTransferModule/scripts/Invoke-DeltaSelfTest.ps1

.SYNOPSIS
    Proves the delta encoder and the firmware interpreter against each other, without a device.

.DESCRIPTION
    Two things have to hold before delta firmware updates may touch hardware:

      1. A patch built from two images rebuilds the second one BYTE FOR BYTE.
      2. Every malformed patch is refused, with the reason it deserves, and nothing is produced.

    Both are checked here, on the host, in seconds. The interpreter under test is the very source the
    firmware compiles (src/FirmwarePatch.cpp) -- `ftc delta apply` drives it -- so a green run says something
    about the device path and not about a host-only lookalike.

    Each malformed case mutates ONE thing in an otherwise valid patch and expects ONE reason. Where a
    mutation can legitimately trip two guards, both are accepted; anything else is a failure, including
    "refused for a different reason", because a wrong reason means the wrong guard fired.

    `ftc delta apply` exits with 10 + reason, so the exit code identifies the guard that stopped it.

.PARAMETER Ftc
    The ftc binary. Defaults to the macOS arm64 build inside this repository.

.PARAMETER Old
    Image the patch is built against. Any two firmware images of the same target will do; two builds
    of the same source are the sharpest case, because then the patch is a few hundred bytes.

.PARAMETER New
    Image the patch has to rebuild.

.PARAMETER WorkDir
    Scratch directory for the generated patches. Created if missing, contents overwritten.

.EXAMPLE
    ./Invoke-DeltaSelfTest.ps1 -Old old.bin -New new.bin

.EXAMPLE
    ./Invoke-DeltaSelfTest.ps1 -Ftc ~/bin/ftc -Old a.bin -New b.bin -WorkDir /tmp/delta
#>

[CmdletBinding()]
param(
    [string]$Ftc = (Join-Path $PSScriptRoot '../ftc-cli/.pio/build/ftc-cli-macos-arm64/ftc'),
    [Parameter(Mandatory = $true)][string]$Old,
    [Parameter(Mandatory = $true)][string]$New,
    [string]$WorkDir = (Join-Path ([System.IO.Path]::GetTempPath()) 'ftc-delta-selftest')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ─── Reasons, mirroring FirmwarePatch::Error. The exit code is 10 + reason. ──────────────────────────────
$R = @{
    NONE = 0; MAGIC = 1; VERSION = 2; FLAGS = 3; HEADER_CRC = 4; SIZE = 5; SRC_RANGE = 6
    SRC_CRC = 7; VARINT = 8; OPCODE = 9; ZERO_LEN = 10; COPY_RANGE = 11; LIT_RANGE = 12
    DST_RANGE = 13; TRUNCATED = 14; TRAILING = 15; DST_CRC = 16; READ = 17; WRITE = 18
}
$HDR = 36

function Get-CrcPosix {
    <# CRC-32/POSIX, the variant the module uses. Verified against the standard check value. #>
    param([byte[]]$Data, [int]$Count = -1)
    if ($Count -lt 0) { $Count = $Data.Length }
    [uint32]$crc = 0
    for ($i = 0; $i -lt $Count; $i++) {
        $crc = $crc -bxor ([uint32]$Data[$i] -shl 24)
        for ($b = 0; $b -lt 8; $b++) {
            if ($crc -band 0x80000000) { $crc = (($crc -shl 1) -bxor 0x04C11DB7) -band 0xFFFFFFFF }
            else { $crc = ($crc -shl 1) -band 0xFFFFFFFF }
        }
    }
    return ((-bnot $crc) -band 0xFFFFFFFF)
}

function Set-U32 {
    param([byte[]]$Buf, [int]$Offset, [uint32]$Value)
    $Buf[$Offset] = [byte]($Value -band 0xFF)
    $Buf[$Offset + 1] = [byte](($Value -shr 8) -band 0xFF)
    $Buf[$Offset + 2] = [byte](($Value -shr 16) -band 0xFF)
    $Buf[$Offset + 3] = [byte](($Value -shr 24) -band 0xFF)
}

function Repair-Header {
    <# Re-stamps the header checksum so a mutated field is judged on its own merit, not on the checksum. #>
    param([byte[]]$Buf)
    Set-U32 -Buf $Buf -Offset 32 -Value (Get-CrcPosix -Data $Buf -Count 32)
}

$script:pass = 0
$script:fail = 0

function Assert-Reason {
    param([string]$Name, [byte[]]$Patch, [int[]]$Expect, [string]$Limit = '')
    $file = Join-Path $WorkDir 'case.okd'
    [System.IO.File]::WriteAllBytes($file, $Patch)
    $out = Join-Path $WorkDir 'case.out'
    if (Test-Path $out) { Remove-Item $out -Force }
    $callArgs = @('delta', 'apply', $Old, $file, $out)
    if ($Limit -ne '') { $callArgs += @('--limit', $Limit) }
    & $Ftc @callArgs *> $null
    $code = $LASTEXITCODE
    $want = $Expect | ForEach-Object { 10 + $_ }
    if ($want -contains $code) {
        $script:pass++
        Write-Host ("  PASS  {0,-22} refused, reason {1}" -f $Name, ($code - 10)) -ForegroundColor Green
    }
    else {
        $script:fail++
        $wantTxt = ($Expect -join '/')
        Write-Host ("  FAIL  {0,-22} exit {1} (reason {2}), expected reason {3}" -f $Name, $code, ($code - 10), $wantTxt) -ForegroundColor Red
    }
    if (Test-Path $out) {
        $script:fail++
        Write-Host ("  FAIL  {0,-22} produced output despite refusing" -f $Name) -ForegroundColor Red
    }
}

function Assert-True {
    param([string]$Name, [bool]$Condition, [string]$Detail = '')
    if ($Condition) {
        $script:pass++
        Write-Host ("  PASS  {0,-22} {1}" -f $Name, $Detail) -ForegroundColor Green
    }
    else {
        $script:fail++
        Write-Host ("  FAIL  {0,-22} {1}" -f $Name, $Detail) -ForegroundColor Red
    }
}

# ─── Setup ────────────────────────────────────────────────────────────────────────────────────────
if (-not (Test-Path $Ftc)) { throw "ftc binary not found: $Ftc" }
if (-not (Test-Path $Old)) { throw "source image not found: $Old" }
if (-not (Test-Path $New)) { throw "target image not found: $New" }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

Write-Host ''
Write-Host '  delta self-test' -ForegroundColor Cyan
Write-Host ('  source {0}' -f (Resolve-Path $Old)) -ForegroundColor DarkGray
Write-Host ('  target {0}' -f (Resolve-Path $New)) -ForegroundColor DarkGray
Write-Host ''

# The checksum implementation used to mutate headers has to be the right one, or every header case
# below would be judged by a broken yardstick.
$check = Get-CrcPosix -Data ([System.Text.Encoding]::ASCII.GetBytes('123456789'))
Assert-True 'crc implementation' ($check -eq 0x765E7680) ('check value 0x{0:X8}' -f $check)

# ─── H-1 round trip ───────────────────────────────────────────────────────────────────────────────
$good = Join-Path $WorkDir 'good.okd'
$rebuilt = Join-Path $WorkDir 'rebuilt.bin'
& $Ftc delta make $Old $New $good *> $null
Assert-True 'patch built' ($LASTEXITCODE -eq 0 -and (Test-Path $good)) ("{0} B" -f (Get-Item $good).Length)
& $Ftc delta apply $Old $good $rebuilt *> $null
Assert-True 'patch applied' ($LASTEXITCODE -eq 0) ''
$a = [System.IO.File]::ReadAllBytes($New)
$b = [System.IO.File]::ReadAllBytes($rebuilt)
$same = $a.Length -eq $b.Length
if ($same) { for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { $same = $false; break } } }
Assert-True 'rebuilt image' $same 'byte for byte identical to the target'

# ─── H-2 reproducible encoder ─────────────────────────────────────────────────────────────────────
$twice = Join-Path $WorkDir 'twice.okd'
& $Ftc delta make $Old $New $twice *> $null
$p1 = [System.IO.File]::ReadAllBytes($good)
$p2 = [System.IO.File]::ReadAllBytes($twice)
$rep = $p1.Length -eq $p2.Length
if ($rep) { for ($i = 0; $i -lt $p1.Length; $i++) { if ($p1[$i] -ne $p2[$i]) { $rep = $false; break } } }
Assert-True 'encoder reproducible' $rep 'same inputs produce the same patch'

# ─── Packed patch: same round trip, and smaller ───────────────────────────────────────────────────
$packed = Join-Path $WorkDir 'good.okdz'
$rebuiltZ = Join-Path $WorkDir 'rebuiltz.bin'
& $Ftc delta make $Old $New $packed --pack *> $null
Assert-True 'packed patch built' ($LASTEXITCODE -eq 0 -and (Test-Path $packed)) `
    ("{0} B vs {1} B plain" -f (Get-Item $packed).Length, (Get-Item $good).Length)
& $Ftc delta apply $Old $packed $rebuiltZ *> $null
$c = [System.IO.File]::ReadAllBytes($rebuiltZ)
$sameZ = $a.Length -eq $c.Length
if ($sameZ) { for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $c[$i]) { $sameZ = $false; break } } }
Assert-True 'packed patch applied' $sameZ 'unpacked and rebuilt byte for byte'

# The header of a packed patch stays readable -- that is what lets a device forward one without
# unpacking it. If this ever stops holding, device-to-device updates break silently.
$ph = [System.IO.File]::ReadAllBytes($packed)
$hdrPlain = ($ph[0] -eq 0x4F -and $ph[1] -eq 0x4B -and $ph[2] -eq 0x44 -and $ph[3] -eq 0x31 -and ($ph[5] -band 0x01) -eq 1)
$srcLenP = [BitConverter]::ToUInt32($ph, 8)
$srcLenR = [BitConverter]::ToUInt32($p1, 8)
Assert-True 'packed header readable' ($hdrPlain -and $srcLenP -eq $srcLenR) 'magic, flag and base survive packing'

# ─── H-3 malformed input: one mutation, one expected reason ───────────────────────────────────────
Write-Host ''
Write-Host '  malformed patches' -ForegroundColor Cyan

$srcLen = [System.IO.File]::ReadAllBytes($Old).Length

function New-Case { return , ([byte[]]$p1.Clone()) }

$p = New-Case; $p[0] = 0x58
Assert-Reason 'magic' $p @($R.MAGIC)

$p = New-Case; $p[4] = 9; Repair-Header $p
Assert-Reason 'version' $p @($R.VERSION)

# Bit 0 means "packed" and is defined, so an UNKNOWN flag has to be another bit.
$p = New-Case; $p[5] = 0x02; Repair-Header $p
Assert-Reason 'undefined flag' $p @($R.FLAGS)

# Claims to be packed but the body is not a compressed stream -- must be refused, not misread.
$p = New-Case; $p[5] = 0x01; Repair-Header $p
Assert-Reason 'packed flag, plain body' $p @($R.TRUNCATED, $R.SIZE, $R.HEADER_CRC)

$p = New-Case; $p[32] = $p[32] -bxor 0x01
Assert-Reason 'header checksum' $p @($R.HEADER_CRC)

$p = New-Case; Set-U32 -Buf $p -Offset 8 -Value ([uint32]$srcLen); Repair-Header $p
Assert-Reason 'source beyond limit' $p @($R.SRC_RANGE) -Limit ([string]($srcLen - 1))

$p = New-Case; Set-U32 -Buf $p -Offset 12 -Value ([uint32]3735928559); Repair-Header $p
Assert-Reason 'wrong base image' $p @($R.SRC_CRC)

$p = New-Case; Set-U32 -Buf $p -Offset 20 -Value ([uint32]3735928559); Repair-Header $p
Assert-Reason 'target checksum' $p @($R.DST_CRC)

$opsLen = [BitConverter]::ToUInt32($p1, 24)
$p = New-Case; Set-U32 -Buf $p -Offset 24 -Value ([uint32]($opsLen + 1)); Repair-Header $p
Assert-Reason 'ops length' $p @($R.SIZE)

$litLen = [BitConverter]::ToUInt32($p1, 28)
$p = New-Case; Set-U32 -Buf $p -Offset 28 -Value ([uint32]($litLen + 1)); Repair-Header $p
Assert-Reason 'literal length' $p @($R.SIZE)

$p = New-Case; $p = $p[0..($p.Length - 2)]
Assert-Reason 'truncated file' $p @($R.SIZE)

$p = New-Case; $p[$HDR] = 0x02
Assert-Reason 'unknown opcode' $p @($R.OPCODE)

$p = New-Case; $p[$HDR + 1] = 0x00
Assert-Reason 'zero length op' $p @($R.ZERO_LEN)

$p = New-Case; for ($i = 1; $i -le 5; $i++) { $p[$HDR + $i] = 0x80 }
Assert-Reason 'runaway varint' $p @($R.VARINT)

$dstLen = [BitConverter]::ToUInt32($p1, 16)
$p = New-Case; Set-U32 -Buf $p -Offset 16 -Value ([uint32]($dstLen - 64)); Repair-Header $p
Assert-Reason 'target too short' $p @($R.DST_RANGE, $R.TRAILING, $R.DST_CRC)

$p = New-Case; $p[$p.Length - 1] = $p[$p.Length - 1] -bxor 0xFF
Assert-Reason 'literal damaged' $p @($R.DST_CRC)

$p = New-Case; $p[$HDR + 2] = $p[$HDR + 2] -bxor 0x40
Assert-Reason 'copy offset damaged' $p @($R.COPY_RANGE, $R.DST_RANGE, $R.DST_CRC, $R.TRAILING, $R.TRUNCATED)

# ─── Result ───────────────────────────────────────────────────────────────────────────────────────
Write-Host ''
if ($script:fail -eq 0) {
    Write-Host ("  {0} passed, 0 failed" -f $script:pass) -ForegroundColor Green
    exit 0
}
Write-Host ("  {0} passed, {1} FAILED" -f $script:pass, $script:fail) -ForegroundColor Red
exit 1
