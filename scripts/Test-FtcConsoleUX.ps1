#!/usr/bin/env pwsh
<#
Open ■
┬────┴  Test-FtcConsoleUX
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: OFM-FileTransferModule/scripts/Test-FtcConsoleUX.ps1

.SYNOPSIS
    Console access-control UX regression: drive a CLIENT device's console with
    `ftc <pa> con`/`login` while flipping the TARGET device's access stage at
    runtime (ftm sec/pw/secwin), and check every refusal / open / no-logout path.

.DESCRIPTION
    Companion to Test-FtcSuite.ps1. Opens TWO serial consoles: the ftc CLIENT and
    the FTC TARGET. For each scenario it sets the target stage via the local test
    hooks (`ftm sec off|prog|always|pw|ets`, `ftm pw <pw>`, `ftm secwin <s>` --
    gated OPENKNX_FTC_SECURITY, runtime-only, reboot restores ETS), runs the client
    command, captures the console output, matches it against the expected pattern,
    and writes a Markdown PASS/FAIL report.

    Covered: OFF / PROG-not-in-prog / ALWAYS-open / PW-not-logged-in / PW-login->open
    / PW-no-auto-logout-while-watching / absent target. Cross-platform via
    .NET System.IO.Ports.SerialPort (same serial handling as Test-FtcSuite.ps1).

.PARAMETER ClientPort
    Serial port of the DRIVING device (the ftc client), e.g. /dev/cu.usbmodem3101.
.PARAMETER TargetPort
    Serial port of the TARGET device whose stage is flipped, e.g. /dev/cu.wchusbserial8430.
.PARAMETER TargetPa
    KNX PA of the target the client acts on. Default 5.0.11.
.PARAMETER TestPw
    Runtime test password set on the target for the PW scenarios. Default test1234.
.PARAMETER AbsentPa
    A PA that is guaranteed absent (for the "no answer" path). Default 5.0.99.
.PARAMETER Window
    Idle window (s) set on the target for the no-auto-logout scenario. Default 8.
.PARAMETER ClientDtr / .PARAMETER TargetDtr
    Assert DTR on that port. RP2040/RP2350 native USB-CDC needs it; ESP/CH340 must NOT
    (DTR low = no auto-reset). Default: both off.
.PARAMETER ReportPath
    Markdown report output. Default ./ftc-consoleux-<timestamp>.md next to the script.
.PARAMETER Baud
    Console baud. Default 115200.
.PARAMETER Help
    Print usage and exit.

.EXAMPLE
    ./Test-FtcConsoleUX.ps1 -ClientPort /dev/cu.usbmodem3101 -TargetPort /dev/cu.wchusbserial8430 -TargetPa 5.0.11
    Full console-UX matrix (client = RP, target = ESP), write the report.
#>
param(
    [string] $ClientPort,
    [string] $TargetPort,
    [string] $TargetPa = "5.0.11",
    [string] $TestPw = "test1234",
    [string] $AbsentPa = "5.0.99",
    [int]    $Window = 8,
    [switch] $ClientDtr,
    [switch] $TargetDtr,
    [string] $ReportPath,
    [int]    $Baud = 115200,
    [switch] $Help
)

function OpenKNX_ShowLogo {
    Write-Host ""
    Write-Host "Open " -NoNewline; Write-Host "$([char]0x25A0)" -ForegroundColor Green
    Write-Host "$([char]0x252C)$([char]0x2500)$([char]0x2500)$([char]0x2500)$([char]0x2534)  Test-FtcConsoleUX"
    Write-Host "$([char]0x25A0) KNX   OpenKNX - Erkan " -NoNewline; Write-Host ([char]0x00C7 + "olak")
    Write-Host ""
}

# Strip ANSI + prompt-redraw noise; split CR so each logical line stands alone.
function CleanConsole([string] $raw) {
    if (-not $raw) { return "" }
    $t = $raw -replace "\x1b\[[0-9;?]*[A-Za-z]", ""
    $t = $t -replace "`r", "`n"
    return $t
}

# Send one line and read until idle (or an -Until regex hits). Returns the cleaned capture.
function Invoke-Console {
    param([System.IO.Ports.SerialPort] $Sp, [string] $Cmd, [int] $TimeoutSec = 8, [string] $Until, [int] $IdleMs = 1200)
    if ($null -ne $Cmd) { $Sp.WriteLine($Cmd) }
    $sb = [System.Text.StringBuilder]::new()
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $lastData = Get-Date
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 120
        $chunk = ""
        try { $chunk = $Sp.ReadExisting() } catch { }
        if ($chunk) {
            [void]$sb.Append($chunk); $lastData = Get-Date
            if ($Until -and (CleanConsole $sb.ToString()) -match $Until) { break }
        }
        elseif (((Get-Date) - $lastData).TotalMilliseconds -gt $IdleMs) { break }
    }
    return (CleanConsole $sb.ToString())
}

# Set the target's runtime stage (and optionally password / idle window).
function Set-TargetStage {
    param([System.IO.Ports.SerialPort] $Tp, [string] $Stage, [string] $Pw, [int] $Win = 0)
    [void](Invoke-Console -Sp $Tp -Cmd "ftm sec $Stage" -TimeoutSec 3)
    if ($Pw) { [void](Invoke-Console -Sp $Tp -Cmd "ftm pw $Pw" -TimeoutSec 3) }
    if ($Win -gt 0) { [void](Invoke-Console -Sp $Tp -Cmd "ftm secwin $Win" -TimeoutSec 3) }
}

if ($Help -or -not $ClientPort -or -not $TargetPort) {
    OpenKNX_ShowLogo
    Write-Host "  Usage: ./Test-FtcConsoleUX.ps1 -ClientPort <port> -TargetPort <port> [-TargetPa 5.0.11]"
    Write-Host "  RP client (native CDC): add -ClientDtr.  ESP/CH340 target: omit -TargetDtr."
    exit 0
}
if (-not $ReportPath) { $ReportPath = Join-Path $PSScriptRoot ("ftc-consoleux-{0}.md" -f (Get-Date -Format "yyyyMMdd-HHmmss")) }

OpenKNX_ShowLogo
Write-Host "  Client $ClientPort  ->  Target $TargetPa on $TargetPort   (pw '$TestPw', window ${Window}s)"
Write-Host ""

$cli = [System.IO.Ports.SerialPort]::new($ClientPort, $Baud)
$cli.DtrEnable = [bool]$ClientDtr; $cli.RtsEnable = $false; $cli.ReadTimeout = 500; $cli.NewLine = "`r`n"
$tgt = [System.IO.Ports.SerialPort]::new($TargetPort, $Baud)
$tgt.DtrEnable = [bool]$TargetDtr; $tgt.RtsEnable = $false; $tgt.ReadTimeout = 500; $tgt.NewLine = "`r`n"
try { $cli.Open() } catch { Write-Host "  Cannot open client $ClientPort : $_" -ForegroundColor Red; exit 2 }
try { $tgt.Open() } catch { Write-Host "  Cannot open target $TargetPort : $_" -ForegroundColor Red; $cli.Close(); exit 2 }

$results = @()
function Record($name, $pass, $expect, $got) {
    $script:results += [pscustomobject]@{ Name = $name; Pass = $pass; Expect = $expect; Got = $got }
    $tag = if ($pass) { "PASS" } else { "FAIL" }
    $col = if ($pass) { "Green" } else { "Red" }
    Write-Host ("  [{0}] {1}" -f $tag, $name) -ForegroundColor $col
}

try {
    # 1) OFF -> console locked
    Set-TargetStage -Tp $tgt -Stage "off"
    $o = Invoke-Console -Sp $cli -Cmd "ftc $TargetPa con" -TimeoutSec 6 -Until "console locked|password|did not"
    Record "OFF -> locked" ($o -match "console locked") 'console locked' $o

    # 2) PROG, not in prog mode -> console locked
    Set-TargetStage -Tp $tgt -Stage "prog"
    $o = Invoke-Console -Sp $cli -Cmd "ftc $TargetPa con" -TimeoutSec 6 -Until "console locked|password|did not"
    Record "PROG(not-prog) -> locked" ($o -match "console locked") 'console locked' $o

    # 3) ALWAYS -> opens, quit closes cleanly
    Set-TargetStage -Tp $tgt -Stage "always"
    [void](Invoke-Console -Sp $cli -Cmd "ftc $TargetPa con" -TimeoutSec 6 -Until "console $TargetPa|locked|did not")
    $o = Invoke-Console -Sp $cli -Cmd "quit" -TimeoutSec 5 -Until "session closed"
    Record "ALWAYS -> open+close" ($o -match "session closed") 'session closed' $o

    # 4) PW, not logged in -> password-protected refusal
    Set-TargetStage -Tp $tgt -Stage "pw" -Pw $TestPw
    $o = Invoke-Console -Sp $cli -Cmd "ftc $TargetPa con" -TimeoutSec 6 -Until "password-protected|console $TargetPa"
    Record "PW(not-logged) -> refusal" ($o -match "password-protected") 'password-protected -- run: … login' $o

    # 5) PW login -> con opens
    $o = Invoke-Console -Sp $cli -Cmd "ftc $TargetPa login $TestPw" -TimeoutSec 6 -Until "login OK|failed|denied"
    $login = ($o -match "login OK")
    $o2 = Invoke-Console -Sp $cli -Cmd "ftc $TargetPa con" -TimeoutSec 6 -Until "console $TargetPa|password|locked"
    [void](Invoke-Console -Sp $cli -Cmd "quit" -TimeoutSec 5 -Until "session closed")
    Record "PW login -> open" ($login -and ($o2 -match "console $TargetPa")) 'login OK + banner' ($o + "`n" + $o2)

    # 6) PW, no auto-logout while watching: short window, login, con, idle > window, command still works
    Set-TargetStage -Tp $tgt -Stage "pw" -Pw $TestPw -Win $Window
    [void](Invoke-Console -Sp $cli -Cmd "ftc $TargetPa login $TestPw" -TimeoutSec 6 -Until "login OK|failed")
    [void](Invoke-Console -Sp $cli -Cmd "ftc $TargetPa con" -TimeoutSec 6 -Until "console $TargetPa|password")
    Write-Host ("      idling {0}s in-session (> {1}s window)..." -f ($Window + 3), $Window)
    Start-Sleep -Seconds ($Window + 3)
    $o = Invoke-Console -Sp $cli -Cmd "uptime" -TimeoutSec 5
    $o2 = Invoke-Console -Sp $cli -Cmd "quit" -TimeoutSec 5 -Until "session closed"
    $noLogout = ($o -notmatch "authorization expired") -and ($o2 -match "session closed")
    Record "PW no-auto-logout (watch>window)" $noLogout 'no "authorization expired"; clean close' ($o + "`n" + $o2)

    # 7) absent target -> no answer
    $o = Invoke-Console -Sp $cli -Cmd "ftc $AbsentPa con" -TimeoutSec 6 -Until "did not answer|no answer|no console"
    Record "absent -> no answer" ($o -match "did not answer|no answer|no console") 'no answer' $o

    # 8) brute-force back-off: 3 wrong logins free, the 4th escalates to a timed wait ("next attempt in N min").
    #    A prior successful login (scenarios 5/6) reset the fail counter, so this starts clean. NOTE: this leaves
    #    the target in a ~1 min back-off; a reboot or a correct login clears it (the console/mode tests are
    #    unaffected -- back-off only gates login attempts).
    Set-TargetStage -Tp $tgt -Stage "pw" -Pw $TestPw
    for ($i = 1; $i -le 3; $i++) { [void](Invoke-Console -Sp $cli -Cmd "ftc $TargetPa login WrongPw$i" -TimeoutSec 6 -Until "wrong password|too many tries|login OK") }
    $o = Invoke-Console -Sp $cli -Cmd "ftc $TargetPa login WrongPw4" -TimeoutSec 6 -Until "too many tries|wrong password|login OK"
    Record "brute-force back-off (4th wrong login)" ($o -match "too many tries") 'too many tries, next attempt in 1 min' $o
}
finally {
    # restore the target to its ETS config
    Set-TargetStage -Tp $tgt -Stage "ets"
    [void](Invoke-Console -Sp $tgt -Cmd "ftm pw" -TimeoutSec 3)
    [void](Invoke-Console -Sp $tgt -Cmd "ftm secwin 0" -TimeoutSec 3)
    $cli.Close(); $tgt.Close()
}

$pass = ($results | Where-Object Pass).Count; $total = $results.Count
$sumCol = if ($pass -eq $total) { "Green" } else { "Yellow" }
Write-Host ""
Write-Host ("  Result: {0}/{1} passed" -f $pass, $total) -ForegroundColor $sumCol

# Markdown report
$md = @()
$md += "# FTC Console-UX test report"
$md += ""
$md += "- Date: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
$md += "- Client: ``$ClientPort``  Target: $TargetPa on ``$TargetPort``  (pw ``$TestPw``, window ${Window}s)"
$md += "- Result: **$pass / $total passed**"
$md += ""
$md += "| Scenario | Status | Expected | Got (excerpt) |"
$md += "|---|---|---|---|"
foreach ($r in $results) {
    $got = ($r.Got -split "`n" | Where-Object { $_ -match "\S" } | Select-Object -Last 1)
    $got = ([string]$got -replace "\|", "\|").Trim()
    $st = if ($r.Pass) { "PASS" } else { "FAIL" }
    $md += ("| {0} | {1} | {2} | {3} |" -f $r.Name, $st, $r.Expect, $got)
}
$md -join "`n" | Out-File -FilePath $ReportPath -Encoding utf8
Write-Host "  Report: $ReportPath"
exit ([int]($pass -ne $total))
