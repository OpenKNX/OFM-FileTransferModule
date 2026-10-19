#!/usr/bin/env pwsh
<#
Open ■
┬────┴  Test-OknxServe
■ KNX   2026 OpenKNX - Erkan Çolak

FILEPATH: OFM-FileTransferModule/scripts/Test-OknxServe.ps1

.SYNOPSIS
    Checks `oknx serve` on this machine: start, one instance per port, port held alone, restart, detach,
    and with an interface the release of a killed worker's tunnel.

.DESCRIPTION
    Runs on Windows (Windows PowerShell 5.1 or PowerShell 7) and on Linux, a Raspberry Pi included (PowerShell 7).
    Every serve the script starts uses its own port (-Port) and its own configuration folder in the temp
    directory, so a serve already running on this machine, its key and its rules are not touched.

    Without -Interface nothing is sent to KNX. With -Interface the script reads one group address and opens a
    group monitor on that interface; it never writes to the bus.

    Prints PASS / FAIL / INFO per check and exits with the number of FAILs.

.PARAMETER Oknx
    Path of the oknx binary to test. Default: oknx (or oknx.exe) from PATH.

.PARAMETER Port
    HTTP port of the serve under test. Default 18780.

.PARAMETER Interface
    IPv4 address of a KNXnet/IP interface for the bus checks (group read, monitor, killed worker). Optional.

.PARAMETER KnxPort
    KNXnet/IP port of that interface. Default 3671.

.PARAMETER Ga
    Group address for the group read check. Default 0/7/0.

.EXAMPLE
    ./Test-OknxServe.ps1 -Oknx C:\tmp\oknx.exe
    The checks that need no interface.

.EXAMPLE
    pwsh ./Test-OknxServe.ps1 -Oknx ./oknx -Interface 11.11.0.210
    All checks, on a Raspberry Pi.
#>
param(
    [string] $Oknx = '',
    [int] $Port = 18780,
    [string] $Interface = '',
    [int] $KnxPort = 3671,
    [string] $Ga = '0/7/0'
)

Set-StrictMode -Version 2
# Continue: Windows PowerShell 5.1 turns the stderr of a native program into an error record, which 'Stop' would make fatal
$ErrorActionPreference = 'Continue'

$IsWin = ($PSVersionTable.PSVersion.Major -lt 6) -or $IsWindows
if (-not $Oknx) { $Oknx = if ($IsWin) { 'oknx.exe' } else { 'oknx' } }
$cmd = Get-Command $Oknx -ErrorAction SilentlyContinue
if (-not $cmd) { Write-Host "oknx not found: $Oknx"; exit 1 }
$Oknx = $cmd.Source
$OknxName = [System.IO.Path]::GetFileName($Oknx)
$script:Started = @()

$script:Fails = 0
function Report([string] $Result, [string] $Name, [string] $Detail = '') {
    $color = switch ($Result) { 'PASS' { 'Green' } 'FAIL' { 'Red' } default { 'Yellow' } }
    Write-Host ('{0,-5} {1}' -f $Result, $Name) -ForegroundColor $color -NoNewline
    if ($Detail) { Write-Host ('   ' + $Detail) } else { Write-Host '' }
    if ($Result -eq 'FAIL') { $script:Fails++ }
}

# own configuration folder for every serve this script starts
$Cfg = Join-Path ([System.IO.Path]::GetTempPath()) ('oknx-serve-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $Cfg | Out-Null
$saved = @{ APPDATA = $env:APPDATA; XDG_CONFIG_HOME = $env:XDG_CONFIG_HOME; OKNX_SERVE_PORT = $env:OKNX_SERVE_PORT }
if ($IsWin) { $env:APPDATA = $Cfg } else { $env:XDG_CONFIG_HOME = $Cfg }
$env:OKNX_SERVE_PORT = "$Port"
$OknxDir = Join-Path $Cfg 'oknx'
$KeyFile = Join-Path $OknxDir 'serve.key'
$OpLog = Join-Path (Join-Path $OknxDir 'serve') 'operations.log'
New-Item -ItemType Directory -Path $OknxDir -Force | Out-Null
if ($Interface) {
    $rules = '{ "version": 1, "sites": [ { "name": "test", "interfaces": [ { "ip": "' + $Interface + '", "port": ' + $KnxPort +
             ', "classes": ["passive","net","device-http","bus-read","group-monitor"] } ] } ] }'
    [System.IO.File]::WriteAllText((Join-Path $OknxDir 'rules.json'), $rules)
}

function Invoke-Serve([string] $Method, [string] $Path, [string] $Body = '', [int] $TimeoutMs = 5000) {
    # HttpWebRequest without a proxy works the same on 5.1 and 7
    $r = [System.Net.HttpWebRequest]::Create("http://127.0.0.1:$Port$Path")
    $r.Method = $Method
    $r.Proxy = $null
    $r.Timeout = $TimeoutMs
    $r.ReadWriteTimeout = $TimeoutMs
    if (Test-Path $KeyFile) { $r.Headers.Add('X-Oknx-Key', ([System.IO.File]::ReadAllText($KeyFile)).Trim()) }
    if ($Body) {
        $r.ContentType = 'application/json'
        $b = [System.Text.Encoding]::UTF8.GetBytes($Body)
        $s = $r.GetRequestStream(); $s.Write($b, 0, $b.Length); $s.Close()
    }
    try { $resp = $r.GetResponse() } catch [System.Net.WebException] {
        if ($_.Exception.Response) { $resp = $_.Exception.Response } else { return @{ Status = 0; Body = $_.Exception.Message } }
    }
    $rd = New-Object System.IO.StreamReader($resp.GetResponseStream())
    $text = $rd.ReadToEnd(); $rd.Close()
    return @{ Status = [int]$resp.StatusCode; Body = $text }
}

function Wait-Health([int] $Seconds) {
    $until = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $until) {
        $h = Invoke-Serve 'GET' '/api/health' '' 1000
        if ($h.Status -eq 200 -and $h.Body -match '"oknx serve"') { return $true }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

function Start-Serve([string[]] $Extra = @()) {
    $out = Join-Path $Cfg ('serve-' + [guid]::NewGuid().ToString('N').Substring(0, 6))
    $args2 = @('serve', '--port', "$Port") + $Extra
    $p = Start-Process -FilePath $Oknx -ArgumentList $args2 -PassThru -NoNewWindow `
                       -RedirectStandardOutput "$out.out" -RedirectStandardError "$out.err"
    try { $null = $p.Handle } catch { } # read while it runs, else ExitCode stays empty on Windows
    $script:Started += $p
    return @{ Proc = $p; Out = "$out.out"; Err = "$out.err" }
}

function Stop-Serve {
    & $Oknx serve stop --port $Port *> $null
    $until = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $until -and (Invoke-Serve 'GET' '/api/health' '' 500).Status -eq 200) { Start-Sleep -Milliseconds 200 }
}

function Get-WorkerPid([int] $ParentId, [string] $Ip) {
    # only a worker of the serve this script started: a serve already running here has workers for the same ip
    if ($IsWin) {
        $pat = '"--_worker" "-i" "' + [regex]::Escape($Ip) + '"' # serve quotes every argument of a worker on Windows
        $w = Get-CimInstance Win32_Process -Filter "ParentProcessId = $ParentId" |
             Where-Object { $_.Name -eq $OknxName -and $_.CommandLine -match $pat }
        if ($w) { return @($w)[0].ProcessId }
    } else {
        $o = & pgrep -P $ParentId -f ('_worker -i ' + $Ip.Replace('.', '[.]') + ' ') 2>$null
        if ($o) { return [int](@($o)[0]) }
    }
    return 0
}

function Test-SecondBind {
    # a second listening socket on serve's port; on POSIX it asks for SO_REUSEPORT too, the way a second serve with
    # httplib's default options would (Linux SOL_SOCKET 1 / SO_REUSEPORT 15, macOS 0xFFFF / 0x200)
    $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
    $l.Server.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket, [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
    if (-not $IsWin) {
        $lvl = if ($IsMacOS) { 0xFFFF } else { 1 }
        $opt = if ($IsMacOS) { 0x200 } else { 15 }
        $l.Server.SetRawSocketOption($lvl, $opt, [BitConverter]::GetBytes([int]1))
    }
    try { $l.Start(); $l.Stop(); return $true } catch { return $false }
}

Write-Host "oknx: $Oknx"
Write-Host "test port $Port, config $Cfg"
Write-Host ''

try {
    # 1 start, key file, owner-only
    $s1 = Start-Serve
    if (Wait-Health 10) { Report 'PASS' 'serve starts and answers /api/health' } else { Report 'FAIL' 'serve starts and answers /api/health' (Get-Content $s1.Err -Raw -ErrorAction SilentlyContinue) }
    if (Test-Path $KeyFile) {
        if ($IsWin) {
            $acl = Get-Acl $KeyFile
            $ids = @($acl.Access | ForEach-Object { $_.IdentityReference.Value })
            $me = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
            if ($ids.Count -eq 1 -and $ids[0] -eq $me -and $acl.AreAccessRulesProtected) { Report 'PASS' 'key file readable by its owner alone' $me }
            else { Report 'FAIL' 'key file readable by its owner alone' ("access: " + ($ids -join ', ')) }
        } else {
            $mode = if ($IsMacOS) { (& stat -f '%Lp' $KeyFile).Trim() } else { (& stat -c '%a' $KeyFile).Trim() }
            if ($mode -eq '600') { Report 'PASS' 'key file readable by its owner alone' "mode $mode" } else { Report 'FAIL' 'key file readable by its owner alone' "mode $mode" }
        }
    } else { Report 'FAIL' 'key file created' $KeyFile }

    # 2 a second serve on the same port is refused (and does not stay: 10 s, then it is stopped)
    $s2 = Start-Serve
    if (-not $s2.Proc.WaitForExit(10000)) { try { $s2.Proc.Kill() } catch { }; Report 'FAIL' 'second serve on the same port refused' 'it kept running' }
    else {
        $o2 = (Get-Content $s2.Err -Raw -ErrorAction SilentlyContinue) + (Get-Content $s2.Out -Raw -ErrorAction SilentlyContinue)
        $rc2 = $s2.Proc.ExitCode
        if ($rc2 -ne 0 -and $o2 -match 'already running') { Report 'PASS' 'second serve on the same port refused' "exit $rc2" }
        else { Report 'FAIL' 'second serve on the same port refused' "exit $rc2, $o2" }
    }

    # 3 nobody else can bind the port beside serve
    if (-not (Test-SecondBind)) { Report 'PASS' 'port held by serve alone (a second listening socket refused)' }
    else { Report 'FAIL' 'port held by serve alone (a second listening socket refused)' 'a second socket bound beside serve' }

    # 4 stop after a few requests, start again at once
    1..3 | ForEach-Object { Invoke-Serve 'GET' '/api/health' | Out-Null }
    Stop-Serve
    $t0 = Get-Date
    $s4 = Start-Serve
    if (Wait-Health 10) { Report 'PASS' 'restart on the same port right after stop' ('{0:N1} s' -f ((Get-Date) - $t0).TotalSeconds) }
    else { Report 'FAIL' 'restart on the same port right after stop' (Get-Content $s4.Err -Raw -ErrorAction SilentlyContinue) }
    Stop-Serve

    # 5 --detach: the starting process returns, serve keeps running
    $o5 = & $Oknx serve --port $Port --detach 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -and (Wait-Health 10)) { Report 'PASS' 'serve --detach returns and serve keeps running' ($o5.Trim() -split "`n")[0] }
    else { Report 'FAIL' 'serve --detach returns and serve keeps running' $o5.Trim() }
    Stop-Serve

    if ($Interface) {
        # 6 what the reservation probe costs a command when no serve runs
        # OKNX_SERVE_PORT points at the test port, where no serve runs now: the probe finds none
        $tA = Measure-Command { & $Oknx -q -i $Interface --port $KnxPort ga read $Ga *> $null }; $rcA = $LASTEXITCODE
        $tB = Measure-Command { & $Oknx -q -i $Interface --port $KnxPort --no-serve ga read $Ga *> $null }; $rcB = $LASTEXITCODE
        $d = ('without serve {0:N2} s (exit {1}), with --no-serve {2:N2} s (exit {3})' -f $tA.TotalSeconds, $rcA, $tB.TotalSeconds, $rcB)
        if ($rcA -eq $rcB) { Report 'PASS' 'same result with and without --no-serve' $d } else { Report 'FAIL' 'same result with and without --no-serve' $d }

        # 7 a killed worker: the next worker closes the tunnel it left
        $s7 = Start-Serve
        if (-not (Wait-Health 10)) { Report 'FAIL' 'serve for the worker check' } else {
            $m = Invoke-Serve 'POST' '/api/ops/monitor' ('{"ip":"' + $Interface + '","port":' + $KnxPort + ',"mode":"gm","seconds":60}')
            Start-Sleep -Seconds 3
            $w = Get-WorkerPid $s7.Proc.Id $Interface
            if (-not $w) { Report 'FAIL' 'worker process found' $m.Body } else {
                Stop-Process -Id $w -Force
                Start-Sleep -Seconds 2
                $r = Invoke-Serve 'POST' '/api/ops/ga_read?wait=1' ('{"ip":"' + $Interface + '","port":' + $KnxPort + ',"ga":["' + $Ga + '"]}') 20000
                $log = if (Test-Path $OpLog) { Get-Content $OpLog -Raw } else { '' }
                if ($log -match '"action":"released"') { Report 'PASS' 'the next worker released the killed worker''s tunnel' "worker pid $w" }
                else { Report 'FAIL' 'the next worker released the killed worker''s tunnel' ("ga_read: HTTP " + $r.Status) }
            }
            Stop-Serve
        }
    } else {
        Report 'INFO' 'bus checks skipped' 'give -Interface <ip> for the group read, the probe cost and the killed worker'
    }
}
finally {
    try {
        Stop-Serve
        foreach ($p in $script:Started) { try { if (-not $p.HasExited) { $p.Kill() } } catch { } } # one that ignored serve stop
    } finally {
        foreach ($k in @($saved.Keys)) {
            if ($saved[$k]) { Set-Item -Path "env:$k" -Value $saved[$k] } else { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
        }
    }
}

Write-Host ''
Write-Host 'By hand (the script cannot close its own terminal):'
Write-Host "  - start 'oknx serve --detach' in a terminal, close that terminal window, open a new one: 'oknx serve status' still answers"
Write-Host "  - start 'oknx serve' in the foreground with a monitor running, close its window (not Ctrl+C): the interface's tunnel page shows the slot free at once"
Write-Host ''
Write-Host ("FAILS: {0}   (config left in {1})" -f $script:Fails, $Cfg)
exit $script:Fails
