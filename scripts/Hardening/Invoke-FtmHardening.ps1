#!/usr/bin/env pwsh
<#
Open ■
┬────┴  Invoke-FtmHardening
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: scripts/Hardening/Invoke-FtmHardening.ps1

.SYNOPSIS
    Runs the adversarial FTC / FTM / console hardening suites and writes a Markdown +
    JSON report.

.DESCRIPTION
    The existing Test-Ftc*.ps1 scripts cover FUNCTION: does an upload work, does resume
    work, does the access-control UX look right. This runner covers the opposite: what
    happens when the input is wrong, out of order, oversized, unauthorised, or simply too
    fast. Those are the cases a field failure comes from.

    Suites:
      F-P  Protocol      malformed and edge-case commands on object 159
      F-R  Response      the command x response x client-state matrix, per drive
      F-S  State machine commands out of order, cancel in every phase, handle leaks
      F-A  Access        every write command in every security stage, without authorisation
      F-C  Console       object 160: oversized lines, ring overflow, parallel sessions
      F-L  Limits        space guard, chunk bound, filesystem full, the flood cliff
      F-N  Non-blocking  loop jitter during CRC, transfer and console drain

    Everything is driven through a device console over serial, because that is how the
    on-device ftc client is operated. -Port is therefore the CLIENT device's console.

.PARAMETER Port
    Serial port of the DRIVING device's console (the ftc client). Mandatory unless -SelfTest.

.PARAMETER Target
    Individual address of the FTC target device, e.g. 5.0.3.

.PARAMETER Suite
    Protocol, Response, State, Access, Console, Limits, NonBlocking, All. Default All.

.PARAMETER Dtr
    Assert DTR/RTS on connect. Leave off for CH340/ESP boards - it resets them.

.PARAMETER Drive
    Which drives to exercise in the response matrix: LittleFS, sd, efc. Default LittleFS.

.PARAMETER IncludeDestructive
    Allow cases that fill the filesystem, exhaust space, or push past the flood cliff.
    The flood case has been observed to REBOOT an RP2040 - that is the point of it, but
    it is never run by accident.

.PARAMETER Security
    The device is built with OPENKNX_FTC_SECURITY, so the access-control suite applies.

.PARAMETER ReportDir
    Output directory. Default: scripts/Hardening/Reports.

.PARAMETER SelfTest
    Verify the library's payload builders offline and exit. No device needed.

.EXAMPLE
    ./Invoke-FtmHardening.ps1 -SelfTest
.EXAMPLE
    ./Invoke-FtmHardening.ps1 -Port /dev/cu.usbmodem84101 -Target 5.0.3 -Suite Protocol,Response
.EXAMPLE
    ./Invoke-FtmHardening.ps1 -Port COM5 -Target 5.0.3 -Security -Suite Access
#>

[CmdletBinding()]
param(
    [string]$Port = '',
    [int]$Baud = 115200,
    [string]$Target = '5.0.3',
        [string[]]$Suite = @('All'),
    [switch]$Dtr,
        [string[]]$Drive = @('LittleFS'),
    [switch]$IncludeDestructive,
    [switch]$Security,
    [string]$ReportDir = '',
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Import-Module (Join-Path $here 'FtmTest.psm1') -Force

if ($IsWindows) { try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { } }

function Expand-ListArgument {
    <#
    .SYNOPSIS
        Normalises a multi-value argument and validates it against the allowed set.
    .DESCRIPTION
        "pwsh script.ps1 -X a,b" hands the script the single STRING "a,b", so a
        [ValidateSet] on a [string[]] parameter rejects correct-looking input.
    #>
    param([string[]]$Value, [string[]]$Allowed, [string]$Name)
    $out = @()
    foreach ($v in $Value) {
        if ($null -eq $v) { continue }
        foreach ($part in ($v -split '[,;\s]+')) {
            if (-not $part) { continue }
            $match = $Allowed | Where-Object { $_ -ieq $part }
            if (-not $match) { throw "-$Name : '$part' is not one of: $($Allowed -join ', ')" }
            if ($out -notcontains $match) { $out += $match }
        }
    }
    if ($out.Count -eq 0) { throw "-$Name : no value given" }
    return , $out
}

function Show-Logo {
    Write-Host ''
    Write-Host '  Open ■' -ForegroundColor Green
    Write-Host '  ┬────┴  FTC / FTM hardening' -ForegroundColor Green
    Write-Host '  ■ KNX   2026 OpenKNX - Erkan Çolak' -ForegroundColor DarkGray
    Write-Host ''
}

function Write-Section {
    param([string]$Text)
    Write-Host ''
    Write-Host "  $Text" -ForegroundColor Cyan
    Write-Host ('  ' + ('-' * $Text.Length)) -ForegroundColor DarkGray
}

# ─── Self-test ──────────────────────────────────────────────────────────────────

if ($SelfTest) {
    Show-Logo
    $st = Invoke-FtmSelfTest
    if (-not $st.Ok) {
        Write-Host "  ABORT: $($st.Failed)/$($st.Total) vectors are wrong - the suites would judge wrongly." -ForegroundColor Red
        Write-Host ''
        exit 2
    }
    Write-Host '  Library verified.' -ForegroundColor Green
    Write-Host ''
    exit 0
}

if (-not $Port) {
    Show-Logo
    Write-Host '  -Port is required (or use -SelfTest). See: Get-Help ./Invoke-FtmHardening.ps1 -Detailed' -ForegroundColor Yellow
    Write-Host ''
    exit 1
}

Show-Logo

Write-Section 'Pre-flight'
$st = Invoke-FtmSelfTest -Quiet
if (-not $st.Ok) {
    Write-Host "  ABORT: self-test failed ($($st.Failed)/$($st.Total)) - refusing to judge a device with a broken library." -ForegroundColor Red
    exit 2
}
Write-Host "  Library self-test: $($st.Total)/$($st.Total) vectors OK" -ForegroundColor Green

if (-not $ReportDir) { $ReportDir = Join-Path $here 'Reports' }

$console = $null
try { $console = Open-FtmConsole -Port $Port -Baud $Baud -Dtr:$Dtr }
catch {
    Write-Host "  ABORT: cannot open the console on $Port - $($_.Exception.Message)" -ForegroundColor Red
    exit 3
}

if (-not (Test-FtmConsoleAlive -Console $console)) {
    Write-Host "  ABORT: the console on $Port does not answer. Fix the rig before judging the device." -ForegroundColor Red
    Close-FtmConsole -Console $console
    exit 3
}
Write-Host "  Console on $Port answers" -ForegroundColor Green

$ver = Invoke-FtmConsoleCommand -Console $console -Command 'version' -TimeoutMs 6000
$firmware = ($ver -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 3) -join ' / '
Write-Host "  Firmware      : $firmware"
Write-Host "  FTC target    : $Target"
Write-Host "  Drives        : $($Drive -join ', ')"
if ($Security) { Write-Host '  Security      : OPENKNX_FTC_SECURITY expected' -ForegroundColor Yellow }
if ($IncludeDestructive) { Write-Host '  Destructive   : ENABLED (may fill the filesystem and may REBOOT the device)' -ForegroundColor Red }

# ─── Context ────────────────────────────────────────────────────────────────────

$driveList = Expand-ListArgument -Value $Drive -Allowed @('LittleFS','sd','efc') -Name 'Drive'
$ctx = [pscustomobject]@{
    Console            = $console
    Port               = $Port
    Target             = $Target
    Drives             = @($driveList)
    Security           = [bool]$Security
    IncludeDestructive = [bool]$IncludeDestructive
    Firmware           = $firmware
}

[void](Start-FtmTestRun -Product 'FTC-hardening' -Target $Target -RunProfile $(if ($IncludeDestructive) { 'Full' } else { 'Safe' }) -Environment @{
        'Console port' = $Port
        'FTC target'   = $Target
        'Drives'       = ($driveList -join ', ')
        'Firmware'     = $firmware
        'Security'     = $Security.ToString()
        'Destructive'  = $IncludeDestructive.ToString()
        'Test client'  = "Invoke-FtmHardening.ps1 / FtmTest.psm1 ($($st.Total) vectors verified)"
        'Host'         = "$($PSVersionTable.PSEdition) $($PSVersionTable.PSVersion)"
    })

# ─── Suites ─────────────────────────────────────────────────────────────────────

$suiteMap = [ordered]@{
    'Protocol'    = @{ File = '1-Protocol.Tests.ps1';      Function = 'Invoke-FtmSuiteProtocol';    Title = 'F-P Protocol (object 159)' }
    'Response'    = @{ File = '2-ResponseMatrix.Tests.ps1'; Function = 'Invoke-FtmSuiteResponse';   Title = 'F-R Response matrix' }
    'State'       = @{ File = '3-StateMachine.Tests.ps1';   Function = 'Invoke-FtmSuiteState';      Title = 'F-S State machine' }
    'Access'      = @{ File = '4-Security.Tests.ps1';       Function = 'Invoke-FtmSuiteAccess';     Title = 'F-A Access control' }
    'Console'     = @{ File = '5-Console.Tests.ps1';        Function = 'Invoke-FtmSuiteConsole';    Title = 'F-C Console tunnel (object 160)' }
    'Limits'      = @{ File = '6-Limits.Tests.ps1';         Function = 'Invoke-FtmSuiteLimits';     Title = 'F-L Limits' }
    'NonBlocking' = @{ File = '7-NonBlocking.Tests.ps1';    Function = 'Invoke-FtmSuiteNonBlocking'; Title = 'F-N Non-blocking' }
}

$want = Expand-ListArgument -Value $Suite -Allowed @('Protocol','Response','State','Access','Console','Limits','NonBlocking','All') -Name 'Suite'
if ($want -contains 'All') { $want = @($suiteMap.Keys) }

$suiteDir = Join-Path $here 'Suites'
try {
    foreach ($key in $suiteMap.Keys) {
        if ($want -notcontains $key) { continue }
        $entry = $suiteMap[$key]
        $path = Join-Path $suiteDir $entry.File
        if (-not (Test-Path $path)) {
            Write-Host "  Suite file missing: $path" -ForegroundColor Red
            continue
        }
        Write-Section $entry.Title
        . $path
        & $entry.Function -Ctx $ctx -SuiteTitle $entry.Title
    }
}
finally {
    Close-FtmConsole -Console $console
}

# ─── Report ─────────────────────────────────────────────────────────────────────

$out = Export-FtmTestReport -Directory $ReportDir
$sum = $out.Summary

Write-Section 'Summary'
Write-Host "  Total $($sum.Total)   " -NoNewline
Write-Host "PASS $($sum.Pass)  " -ForegroundColor Green -NoNewline
Write-Host "FAIL $($sum.Fail)  " -ForegroundColor $(if ($sum.Fail -gt 0) { 'Red' } else { 'DarkGray' }) -NoNewline
Write-Host "SKIP $($sum.Skip)  " -ForegroundColor Yellow -NoNewline
Write-Host "N-A $($sum.NA)" -ForegroundColor DarkGray
Write-Host ''
Write-Host "  Report: $($out.Markdown)" -ForegroundColor Cyan
Write-Host "  Data  : $($out.Json)" -ForegroundColor DarkGray
Write-Host ''

if ($sum.Fail -gt 0) { exit 1 }
exit 0
