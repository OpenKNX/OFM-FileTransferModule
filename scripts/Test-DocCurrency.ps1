#!/usr/bin/env pwsh
# Open ■
# ┬────┴  Test-DocCurrency
# ■ KNX   2026 OpenKNX - Erkan Çolak
#
# FILEPATH: scripts/Test-DocCurrency.ps1

<#
.SYNOPSIS
    Compares what the documentation claims against what the code actually contains.

.DESCRIPTION
    Documentation goes stale silently: a constant is renamed, a mode is dropped, an endpoint is added,
    and the text keeps reading plausibly for months. This checks the claims that CAN be checked
    mechanically:

      switches    every FTC_/FTM_/OPENKNX_/KNX_ name written in backticks must exist in the sources
      values      a documented "NAME | 42" must match the constant's value in the code
      endpoints   every route the web client registers must appear in the documentation
      files       every source file named in backticks must exist somewhere in the workspace

    A name that lives in a neighbouring module (OGM-Common, OFM-Network, lib/knx, lib/TPUart) is not a
    finding -- pass its path with -Extra so it counts as known.

    Exit code 0 = nothing stale found, 1 = at least one finding. Suitable for CI.

.PARAMETER Path
    Module root. Defaults to the folder above this script.

.PARAMETER Extra
    Additional source roots to accept identifiers from, e.g. ../OGM-Common ../OFM-Network.

.EXAMPLE
    pwsh scripts/Test-DocCurrency.ps1 -Extra ..\OGM-Common, ..\OFM-Network, ..\knx, ..\tpuart

.EXAMPLE
    pwsh scripts/Test-DocCurrency.ps1
    Without -Extra every identifier that lives in a neighbouring module is reported, so this form
    exits 1 on a healthy tree. Use it to see what the module itself owns, not as the CI invocation.

.NOTES
    Author : Erkan Çolak
    Licence: GNU GPL v3.0
#>
[CmdletBinding()]
param(
    [string]   $Path  = (Join-Path $PSScriptRoot '..'),
    [string[]] $Extra = @(),

    # Named in the docs on purpose but owned by nobody here: the Arduino framework, and the per-product
    # scripts that live in an OAM repo rather than in this module.
    [string[]] $Ignore = @('Arduino.h', 'Build-knxprods.ps1', 'Prepare-Firmware.ps1')
)

# The logo is a dot-sourced script, so it is sourced at SCRIPT scope: a function defined
# inside a function body does not escape that scope.
. (Join-Path $PSScriptRoot 'lib/KnxLogo.ps1')


Write-KnxLogo -Title 'Test-DocCurrency'
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path -LiteralPath $Path).Path
$skip = '[\\/](\.git|\.pio|node_modules|release|Reports)[\\/]'

function Get-SourceText([string[]] $Roots)
{
    $sb = New-Object System.Text.StringBuilder
    foreach ($r in $Roots)
    {
        Get-ChildItem -LiteralPath $r -Recurse -File |
            Where-Object { $_.Extension -in '.cpp', '.h', '.ini', '.json' } |
            Where-Object { $_.FullName -notmatch $skip } |
            ForEach-Object { $null = $sb.AppendLine((Get-Content -LiteralPath $_.FullName -Raw)) }
    }
    $sb.ToString()
}

# Resolve the extra roots once, and say which ones were taken -- a silently ignored path would turn every
# identifier from a neighbouring module into a false finding.
$roots = @($root)
# Accept both spellings: several arguments, or one comma-separated string. Called from a POSIX shell the
# comma form arrives as a single element, and silently scanning nothing is the worst possible outcome here.
$Extra = $Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' }
foreach ($e in $Extra)
{
    if (Test-Path -LiteralPath $e) { $roots += (Resolve-Path -LiteralPath $e).Path }
    else { Write-Host ("  note   extra root not found, ignored: {0}" -f $e) -ForegroundColor DarkYellow }
}
Write-Host ("scanning {0} source root(s): {1}" -f $roots.Count, (($roots | Split-Path -Leaf) -join ', ')) -ForegroundColor DarkGray

$src   = Get-SourceText $roots
# @(...): a pipeline that yields 0 or 1 item has no .Count under Set-StrictMode in PowerShell 7,
# and the summary below reads it - the script then exits 1 and CI reads that as "findings".
$docs  = @(Get-ChildItem -LiteralPath (Join-Path $root 'doc') -Recurse -Filter '*.md' -File)
$names = [System.Collections.Generic.HashSet[string]]::new()
# No \b in front: a build flag is written -DNAME, and the D would swallow the boundary.
foreach ($m in [regex]::Matches($src, '(?:FTC|FTM|OPENKNX|KNX)_[A-Z0-9_]{2,}\b')) { $null = $names.Add($m.Value) }

$files = [System.Collections.Generic.HashSet[string]]::new()
foreach ($r in $roots)
{
    Get-ChildItem -LiteralPath $r -Recurse -File |
        Where-Object { $_.FullName -notmatch $skip } |
        ForEach-Object { $null = $files.Add($_.Name) }
}

$findings = New-Object System.Collections.ArrayList
function Add-Finding($Kind, $Doc, $What, $Detail)
{
    $null = $findings.Add([pscustomobject]@{ Kind = $Kind; Doc = $Doc; What = $What; Detail = $Detail })
}

foreach ($d in $docs)
{
    $rel  = $d.FullName.Substring($root.Length + 1)
    $text = Get-Content -LiteralPath $d.FullName -Raw

    # switches and constants the document names
    foreach ($m in [regex]::Matches($text, '`((?:FTC|FTM|OPENKNX|KNX)_[A-Z0-9_]{2,})`'))
    {
        if (-not $names.Contains($m.Groups[1].Value))
        {
            Add-Finding 'switch' $rel $m.Groups[1].Value 'named in the docs, not found in any source'
        }
    }

    # "| `NAME` | 42 |" -- the value must be the one the code defines
    foreach ($m in [regex]::Matches($text, '`((?:FTC|FTM)_[A-Z0-9_]{2,})`\s*\|\s*(\d{1,6})\b'))
    {
        $n = $m.Groups[1].Value; $v = $m.Groups[2].Value
        # Two spellings carry a value in this codebase: a C/C++ initialiser "NAME = 42" and a
        # "#define NAME 42", which has no '='. Matching only the first left the whole value check
        # inert for the build switches it is aimed at, because those are all defines.
        $def = [regex]::Match($src, "(?m)^\s*#\s*define\s+$n\s+(\d+)\b")
        if (-not $def.Success) { $def = [regex]::Match($src, "$n\s*=\s*(\d+)") }
        if ($def.Success -and $def.Groups[1].Value -ne $v)
        {
            Add-Finding 'value' $rel $n ("doc says {0}, code says {1}" -f $v, $def.Groups[1].Value)
        }
    }

    # source files the document points at
    foreach ($m in [regex]::Matches($text, '`([A-Za-z0-9_.-]+\.(?:cpp|h|js|css|ps1|psm1|py))`'))
    {
        if ($Ignore -contains $m.Groups[1].Value) { continue }
        if (-not $files.Contains($m.Groups[1].Value))
        {
            Add-Finding 'file' $rel $m.Groups[1].Value 'named in the docs, no such file'
        }
    }
}

# every registered web route must be documented somewhere
$webSrc = Join-Path $root 'src/FileTransferWebClient.cpp'
if (Test-Path -LiteralPath $webSrc)
{
    $allDocs = ($docs | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
    foreach ($m in [regex]::Matches((Get-Content -LiteralPath $webSrc -Raw), 'addRoute\(\s*WEB_[A-Z]+\s*,\s*"([^"]+)"'))
    {
        $route = $m.Groups[1].Value
        if ($allDocs -notmatch [regex]::Escape($route))
        {
            Add-Finding 'endpoint' 'doc/' $route 'route exists, documented nowhere'
        }
    }
}

foreach ($f in $findings)
{
    Write-Host ("  {0,-9} {1,-34} {2}  --  {3}" -f $f.Kind, $f.Doc, $f.What, $f.Detail) -ForegroundColor Yellow
}
Write-Host ("{0} documents checked - {1} finding(s)" -f $docs.Count, $findings.Count) `
    -ForegroundColor $(if ($findings.Count -eq 0) { 'Green' } else { 'Red' })

exit $(if ($findings.Count -eq 0) { 0 } else { 1 })
