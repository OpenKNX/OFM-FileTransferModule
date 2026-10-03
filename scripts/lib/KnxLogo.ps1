#!/usr/bin/env pwsh
# Open ■
# ┬────┴  KnxLogo
# ■ KNX   2026 OpenKNX - Erkan Çolak
#
# FILEPATH: scripts/lib/KnxLogo.ps1

<#
.SYNOPSIS
    The OpenKNX logo block, in one place for every script of this module.

.DESCRIPTION
    The colours OGM-Common uses in OpenKNX_ShowLogo
    (scripts/setup/reusable/data/OpenKNX-UI-Generic.ps1): "Open " in the default colour, the squares
    and the bar in green, " KNX" plain. The copyright follows on the same line in grey, so every script
    carries it without each one having to remember.

    Before this file five scripts carried their own copy of OpenKNX_ShowLogo and three drew a variant
    of their own (the whole first line green, the last line grey, a copyright line appended), so one
    test run showed two different logos.

    The glyphs are built from code points. A literal depends on the file still being UTF-8 when it is
    read; a code point cannot be broken by an editor that saves without a BOM.

    Dot-source it at SCRIPT scope, never inside a function: a function defined in a function body does
    not escape that scope, and the call at script level then fails.

.PARAMETER Title
    The one line next to the bar, e.g. 'FTC / FTM hardening'.

.EXAMPLE
    . (Join-Path $PSScriptRoot 'lib/KnxLogo.ps1')
    Write-KnxLogo -Title 'FTC / FTM hardening'
#>

function Write-KnxLogo {
    param([string]$Title = '')
    $sq  = [char]::ConvertFromUtf32(0x25A0)   # black square
    $bar = [char]::ConvertFromUtf32(0x252C) + ([char]::ConvertFromUtf32(0x2500) * 4) + [char]::ConvertFromUtf32(0x2534)
    Write-Host ''
    Write-Host 'Open ' -NoNewline
    Write-Host $sq -ForegroundColor Green
    if ($Title) { Write-Host ("{0}  {1}" -f $bar, $Title) -ForegroundColor Green }
    else        { Write-Host $bar -ForegroundColor Green }
    Write-Host $sq -NoNewline -ForegroundColor Green
    Write-Host ' KNX' -NoNewline
    Write-Host '   2026 OpenKNX - Erkan Çolak' -ForegroundColor DarkGray
    Write-Host ''
}
