# Copyright (c) 2026 ThinkMeta Software GmbH
# SPDX-License-Identifier: MIT (see LICENSE.txt)
<#
.SYNOPSIS
    Prepares the inputs of Verify-Jpeg.ps1 in testdata\ and checks them against Input-Hashes.csv.

.DESCRIPTION
    photos\  the 14 photos of the 8-bit RGB set of https://imagecompression.info/test_images/
             (download "rgb8bit", unpack, pass the folder with -Photos). Free redistribution with
             the notice of the set, which is not repeated here; they are copied, not changed.
    images\  3 synthetic images, made by GenTestImages (built with ctJpeg.slnx).
    edge\    8 small images of odd sizes, GenTestImages --edge.
    gray\    4 grayscale images, GenTestImages --gray, from 3 photos and the synthetic text image.

    Every input must have the size and SHA-256 listed in Input-Hashes.csv, since the reference
    tables were written from exactly these files. Windows PowerShell 5.1 or PowerShell 7.

.EXAMPLE
    .\Prepare-Inputs.ps1 -Photos D:\Downloads\rgb8bit
#>
param(
    [Parameter(Mandatory)][string]$Photos,
    [string]$OutDir = '',
    [string]$GenTestImages = ''
)

$ErrorActionPreference = 'Stop'
# defaults here, not in param(): $PSScriptRoot may be empty there under Windows PowerShell 5.1
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot '..\testdata' }
if (-not $GenTestImages) { $GenTestImages = Join-Path $PSScriptRoot '..\build\Release\bin\GenTestImages.exe' }
if (-not (Test-Path -LiteralPath $GenTestImages)) { throw "GenTestImages.exe not found: $GenTestImages (build ctJpeg.slnx, Release x64, first)" }
$OutDir = [IO.Path]::GetFullPath($OutDir)
$table = @(Import-Csv -LiteralPath (Join-Path $PSScriptRoot 'Input-Hashes.csv'))

foreach ($sub in 'photos', 'images', 'edge', 'gray') { New-Item -ItemType Directory -Force -Path (Join-Path $OutDir $sub) | Out-Null }

# the photos
foreach ($row in $table | Where-Object { $_.File -like 'photos/*' }) {
    $name = Split-Path -Leaf $row.File
    $from = Join-Path $Photos $name
    if (-not (Test-Path -LiteralPath $from)) { throw "photo $name not found in $Photos" }
    Copy-Item -LiteralPath $from -Destination (Join-Path $OutDir 'photos') -Force
}

# the synthetic images
function Invoke-Gen([string[]]$arguments) {
    & $GenTestImages @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "GenTestImages $($arguments -join ' ') failed (exit code $LASTEXITCODE)" }
}
Invoke-Gen @((Join-Path $OutDir 'images'))
Invoke-Gen @('--edge', (Join-Path $OutDir 'edge'))
foreach ($pair in @(@('photos\cathedral.ppm', 'cathedral-gray.pgm'), @('photos\leaves_iso_200.ppm', 'leaves_iso_200-gray.pgm'),
        @('photos\spider_web.ppm', 'spider_web-gray.pgm'), @('images\text-4960x7016.ppm', 'text-gray.pgm'))) {
    Invoke-Gen @('--gray', (Join-Path $OutDir $pair[0]), (Join-Path $OutDir "gray\$($pair[1])"))
}

# every input as listed
$sha = [System.Security.Cryptography.SHA256]::Create()
$bad = 0
foreach ($row in $table) {
    $path = Join-Path $OutDir ($row.File -replace '/', '\')
    $problem = ''
    if (-not (Test-Path -LiteralPath $path)) { $problem = 'missing' }
    else {
        $fs = [IO.File]::OpenRead($path)
        try { $hash = [BitConverter]::ToString($sha.ComputeHash($fs)).Replace('-', '').ToLowerInvariant() } finally { $fs.Dispose() }
        if ((Get-Item -LiteralPath $path).Length -ne [int64]$row.Size) { $problem = 'size differs' }
        elseif ($hash -ne $row.Sha256) { $problem = 'SHA-256 differs' }
    }
    if ($problem) { $bad++; Write-Host "$($row.File): $problem" -ForegroundColor Red }
}
if ($bad -gt 0) { Write-Host "$bad of $($table.Count) inputs differ from Input-Hashes.csv" -ForegroundColor Red; exit 1 }
Write-Host "all $($table.Count) inputs in $OutDir match Input-Hashes.csv" -ForegroundColor Green
exit 0
