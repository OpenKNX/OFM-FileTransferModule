#!/usr/bin/env pwsh
<#
Open ■
┬────┴  FTM Release Hook (post)
■ KNX   2026 OpenKNX - Erkan Çolak

.SYNOPSIS
    Release hook: puts the PC FileTransferClient into the product release as
    Tools/ftc-cli/<OS>/<arch>/ftc[.exe].

.DESCRIPTION
    Discovered and invoked by OGM-Common's Build-Release-Postprocess.ps1, which finds every
    lib/*/scripts/release/Post.ps1 by convention. The layout lives HERE because it is FTM's
    artifact -- OGM-Common knows nothing about ftc, its pio env names or this folder tree.

    Primary source is the ftc-cli pio build matrix: pio names each env "ftc-cli-<os>-<arch>" and
    the binary "ftc[.exe]", so OS and architecture come straight from the env name and a new
    target needs no change here. FTM's own ftc-cli/release/Tools/ tree is git-ignored and may be
    an old local artifact, so it is only used when there is no build matrix at all.

    Never fails the release: a missing client is normal (separate release cycle), and the release
    still carries the upload script, which then works with an ftc the user installed themselves.

.PARAMETER ReleaseRoot  Absolute path of the product's release/ directory.
.PARAMETER BuildParam   "Dev" or "Release" (unused here, part of the hook contract).

.NOTES
    AUTHOR : Erkan Çolak

.LINK
    https://wiki.openknx.de
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$ReleaseRoot,
    [string]$BuildParam = ""
)

$ftcCliRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "ftc-cli"
$toolsRoot  = Join-Path $ReleaseRoot "Tools/ftc-cli"
$osFolders  = @{ windows = "Windows"; macos = "MacOS"; linux = "Linux" }
$copied     = 0

$pioBuild = Join-Path $ftcCliRoot ".pio/build"
if (Test-Path -Path $pioBuild -PathType Container) {
    foreach ($envDir in Get-ChildItem -Path $pioBuild -Directory -ErrorAction SilentlyContinue) {
        if ($envDir.Name -notmatch '^ftc-cli-([a-z0-9]+)-(.+)$') { continue }
        $osKey = $Matches[1]
        $arch  = $Matches[2]
        if (-not $osFolders.ContainsKey($osKey)) { continue }
        $bin = Get-ChildItem -Path $envDir.FullName -File -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -eq 'ftc' -or $_.Name -eq 'ftc.exe' } | Select-Object -First 1
        if (!$bin) { continue }
        $dstDir = Join-Path (Join-Path $toolsRoot $osFolders[$osKey]) $arch
        New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
        Copy-Item $bin.FullName (Join-Path $dstDir $bin.Name) -Force
        $copied++
    }
}

# Fallback: a prepared tree shipped with FTM (no local matrix build) -- mirrored as-is.
if ($copied -eq 0) {
    $prepared = Join-Path $ftcCliRoot "release/Tools/ftc-cli"
    if (Test-Path -Path $prepared -PathType Container) {
        New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null
        Copy-Item (Join-Path $prepared "*") $toolsRoot -Recurse -Force
        $copied = @(Get-ChildItem -Path $toolsRoot -Recurse -File -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -eq 'ftc' -or $_.Name -eq 'ftc.exe' }).Count
        if ($copied -gt 0) { Write-Host "  (from FTM's prepared tree -- no local ftc-cli build found)" -ForegroundColor DarkGray }
    }
}

if ($copied -gt 0) {
    # Usage notes next to the binaries, so every product ships the same text.
    $ftcReadme = Join-Path $ftcCliRoot "README.md"
    if (Test-Path -Path $ftcReadme -PathType Leaf) { Copy-Item $ftcReadme (Join-Path $toolsRoot "README.md") -Force }
    Write-Host "  copied $copied ftc build(s) to Tools/ftc-cli/<OS>/<arch>/" -ForegroundColor Blue
}
else {
    Write-Host "  ftc not found -- no Tools/ftc-cli; KNX-Upload will use an installed ftc" -ForegroundColor DarkYellow
}
