#!/usr/bin/env pwsh
# Open ■
# ┬────┴  Test-DocLinks
# ■ KNX   2026 OpenKNX - Erkan Çolak
#
# FILEPATH: scripts/Test-DocLinks.ps1

<#
.SYNOPSIS
    Checks the relative links between documentation files and reports the ones that point nowhere.

.DESCRIPTION
    Markdown links rot silently: a file is renamed, twenty pointers keep compiling, and the reader finds
    a 404 months later. This walks every *.md in the module, resolves each relative link against the
    file that contains it, and fails when one does not exist.

    It also flags links whose FILE NAME differs from the target only by case. That never fails on macOS
    or Windows, where the filesystem does not care, and always fails on GitHub, where it does -- one class of
    broken link that a local check would otherwise never see.

    Exit code 0 = every link resolves, 1 = at least one does not. Suitable for CI.

.PARAMETER Path
    Root to scan. Defaults to the module folder above this script.

.PARAMETER Quiet
    Print only the summary line.

.EXAMPLE
    pwsh scripts/Test-DocLinks.ps1
    Checks the whole module.

.EXAMPLE
    pwsh scripts/Test-DocLinks.ps1 -Path doc -Quiet
    Checks only the doc folder, one line of output.

.NOTES
    Author : Erkan Çolak
    Licence: GNU GPL v3.0
#>
[CmdletBinding()]
param(
    [string] $Path = (Join-Path $PSScriptRoot '..'),
    [switch] $Quiet
)


Write-Host ''
Write-Host '  Open ■' -ForegroundColor Green
Write-Host '  ┬────┴  Test-DocLinks' -ForegroundColor Green
Write-Host '  ■ KNX   2026 OpenKNX - Erkan Çolak' -ForegroundColor DarkGray
Write-Host ''
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path -LiteralPath $Path).Path
# @(...): a pipeline that yields 0 or 1 item has no .Count under Set-StrictMode in PowerShell 7,
# and the summary below reads it - the script then exits 1 and CI reads that as "findings".
$files = @(Get-ChildItem -LiteralPath $root -Recurse -Filter '*.md' -File |
         Where-Object { $_.FullName -notmatch '[\\/](\.git|\.pio|node_modules|release)[\\/]' })

$dead = New-Object System.Collections.ArrayList
$case = New-Object System.Collections.ArrayList
$total = 0

foreach ($file in $files)
{
    $dir  = Split-Path -Parent $file.FullName
    $text = Get-Content -LiteralPath $file.FullName -Raw

    foreach ($m in [regex]::Matches($text, '\]\(([^)#\s]+?\.(?:md|txt))(?:#[^)]*)?\)'))
    {
        $link = $m.Groups[1].Value
        if ($link -match '^(https?:|mailto:)') { continue }
        $total++

        $target = Join-Path $dir $link
        if (Test-Path -LiteralPath $target)
        {
            # Exists here -- but does it exist with THIS spelling? Only a case-sensitive host will say.
            $leaf   = Split-Path -Leaf $link
            $parent = Split-Path -Parent $target
            $real   = Get-ChildItem -LiteralPath $parent -File | Where-Object { $_.Name -ceq $leaf }
            if (-not $real)
            {
                $null = $case.Add([pscustomobject]@{ File = $file.FullName.Substring($root.Length + 1); Link = $link })
            }
            continue
        }
        $null = $dead.Add([pscustomobject]@{ File = $file.FullName.Substring($root.Length + 1); Link = $link })
    }
}

if (-not $Quiet)
{
    foreach ($d in $dead) { Write-Host ("  dead  {0} -> {1}" -f $d.File, $d.Link) -ForegroundColor Red }
    foreach ($c in $case) { Write-Host ("  case  {0} -> {1}  (resolves here, 404 on GitHub)" -f $c.File, $c.Link) -ForegroundColor Yellow }
}

$bad = $dead.Count + $case.Count
Write-Host ("{0} links in {1} files - {2} dead, {3} case-only" -f $total, $files.Count, $dead.Count, $case.Count) `
    -ForegroundColor $(if ($bad -eq 0) { 'Green' } else { 'Red' })

exit $(if ($bad -eq 0) { 0 } else { 1 })
