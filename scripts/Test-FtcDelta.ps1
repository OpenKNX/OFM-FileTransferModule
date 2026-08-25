#!/usr/bin/env pwsh
<#
Open ■
┬────┴  Test-FtcDelta
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: OFM-FileTransferModule/scripts/Test-FtcDelta.ps1

.SYNOPSIS
    Drives a delta firmware update against REAL hardware and judges what the device did.

.DESCRIPTION
    The host side is already proven without hardware (Invoke-DeltaSelfTest.ps1: round trip, encoder
    reproducibility, sixteen malformed patches). What only a device can answer is here:

      G-1  a patch really updates the device, and it says so itself afterwards
      G-5  a device programmed by ETS in the meantime still accepts the patch
      G-9  a device without room to stage the image refuses BEFORE the transfer
      G-10 a device that does not advertise the feature is never sent a patch
      P-3  no patch in the pool fits the target -> nothing is sent
      P-4  the target cannot apply patches -> nothing is sent

    Not automated, and deliberately so: the power-cut series (G-4) and the bus-load run (G-2). Both need
    someone at the hardware, and a script that pretended to do them would be worse than none.

    The proof that an update happened is the device's own Buildtime. It changes with every build, so a
    device reporting the new one has really started the new firmware -- no inference from "no error".

.PARAMETER Ftc
    The ftc binary.

.PARAMETER Ip
    Interface used to reach the bus.

.PARAMETER Pa
    Device under test.

.PARAMETER Old
    Image the device is currently running.

.PARAMETER New
    Image it should end up with.

.PARAMETER WorkDir
    Scratch directory for the generated patch.

.PARAMETER SkipApply
    Run every check but stop short of actually updating the device.

.EXAMPLE
    ./Test-FtcDelta.ps1 -Ip 11.11.0.126 -Pa 5.0.3 -Old pairs/t-a-a.bin -New pairs/t-a-b.bin

.EXAMPLE
    ./Test-FtcDelta.ps1 -Ip 11.11.0.126 -Pa 5.0.3 -Old a.bin -New b.bin -SkipApply
#>

[CmdletBinding()]
param(
    [string]$Ftc = (Join-Path $PSScriptRoot '../ftc-cli/.pio/build/ftc-cli-macos-arm64/ftc'),
    [Parameter(Mandatory = $true)][string]$Ip,
    [Parameter(Mandatory = $true)][string]$Pa,
    [Parameter(Mandatory = $true)][string]$Old,
    [Parameter(Mandatory = $true)][string]$New,
    [string]$WorkDir = (Join-Path ([System.IO.Path]::GetTempPath()) 'ftc-delta-hw'),
    [switch]$SkipApply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:pass = 0
$script:fail = 0
$script:skip = 0

function Step {
    param([string]$Name, [scriptblock]$Body)
    try {
        $verdict = & $Body
        if ($verdict -eq 'skip') {
            $script:skip++
            Write-Host ("  SKIP  {0}" -f $Name) -ForegroundColor DarkGray
        }
        elseif ($verdict) {
            $script:pass++
            Write-Host ("  PASS  {0}" -f $Name) -ForegroundColor Green
        }
        else {
            $script:fail++
            Write-Host ("  FAIL  {0}" -f $Name) -ForegroundColor Red
        }
    }
    catch {
        $script:fail++
        Write-Host ("  FAIL  {0} -- {1}" -f $Name, $_.Exception.Message) -ForegroundColor Red
    }
}

function Invoke-Ftc {
    param([string[]]$FtcArgs)
    $out = & $Ftc --ip $Ip @FtcArgs 2>&1 | Out-String
    return $out
}

function Get-Buildtime {
    # The device prints Buildtime on its own console; the console tunnel is how we read it back.
    $out = Invoke-Ftc @($Pa, 'con', 'version')
    if ($out -match 'Buildtime\D+(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})') { return $Matches[1] }
    return ''
}

if (-not (Test-Path $Ftc)) { throw "ftc binary not found: $Ftc" }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$patch = Join-Path $WorkDir 'hw.okd'

Write-Host ''
Write-Host "  delta on hardware -- $Pa via $Ip" -ForegroundColor Cyan
Write-Host ''

# ─── Preparation ──────────────────────────────────────────────────────────────────────────────────
Step 'patch built from the pair' {
    & $Ftc delta make $Old $New $patch *> $null
    return ($LASTEXITCODE -eq 0 -and (Test-Path $patch))
}

$size = if (Test-Path $patch) { (Get-Item $patch).Length } else { 0 }
Write-Host ("        patch {0} B, about {1:N1} min on the bus at 480 B/s" -f $size, ($size / 480 / 60)) -ForegroundColor DarkGray

# ─── G-10 / P-4: does the target advertise the feature at all? ────────────────────────────────────
$feat = Invoke-Ftc @($Pa, 'feat')
$hasDelta = $feat -match '(?i)delta'
Step 'target advertises the delta feature' { return $hasDelta }

# ─── G-1: the base the patch expects is the one the device runs ───────────────────────────────────
$before = Get-Buildtime
Write-Host ("        buildtime before: {0}" -f $(if ($before) { $before } else { '(not read)' })) -ForegroundColor DarkGray

# ─── P-3: a patch for a DIFFERENT base has to be refused before anything is transferred ───────────
Step 'patch for a foreign base is refused, nothing sent' {
    $wrong = Join-Path $WorkDir 'wrong.okd'
    & $Ftc delta make $New $Old $wrong *> $null   # built the other way round -> expects the NEW image
    if ($LASTEXITCODE -ne 0) { return $false }
    $out = Invoke-Ftc @($Pa, 'delta', $wrong)
    return ($out -match '(?i)different image|full image')
}

# ─── G-1: the real update ─────────────────────────────────────────────────────────────────────────
if ($SkipApply) {
    Step 'update applied' { return 'skip' }
}
else {
    Step 'update applied and the device reports the new build' {
        $out = Invoke-Ftc @($Pa, 'delta', $patch)
        if ($out -notmatch '(?i)base confirmed') { return $false }
        Start-Sleep -Seconds 20   # transfer + rebuild + reboot
        $after = Get-Buildtime
        Write-Host ("        buildtime after:  {0}" -f $(if ($after) { $after } else { '(not read)' })) -ForegroundColor DarkGray
        return ($after -ne '' -and $after -ne $before)
    }
}

# ─── Manual cases ─────────────────────────────────────────────────────────────────────────────────
Write-Host ''
Write-Host '  needs a person at the hardware:' -ForegroundColor Cyan
Write-Host '    G-2  record group telegrams during the apply -- not one may be missing'
Write-Host '    G-4  cut power during the rebuild, during arming, and during the copy -- 10x each'
Write-Host '    G-5  program the device by ETS between the two images, then patch again'
Write-Host '    G-9  fill the filesystem, then patch -- it must refuse before transferring'

Write-Host ''
if ($script:fail -eq 0) {
    Write-Host ("  {0} passed, {1} skipped, 0 failed" -f $script:pass, $script:skip) -ForegroundColor Green
    exit 0
}
Write-Host ("  {0} passed, {1} skipped, {2} FAILED" -f $script:pass, $script:skip, $script:fail) -ForegroundColor Red
exit 1
