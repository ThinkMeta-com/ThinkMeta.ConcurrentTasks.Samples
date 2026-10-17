<#
.SYNOPSIS
    Builds the standalone benchmark package for other PCs: bin\ (ctOpus, ctOpusGui,
    ctOpusLiveBench, opusenc, scheduler core) with the benchmark script.

.DESCRIPTION
    The package needs no installation and holds no WAV files; the user points the script at a
    folder of their own.

.EXAMPLE
    .\New-BenchPackage.ps1
#>
param(
    [string]$BinDir = (Join-Path $PSScriptRoot 'bin'),
    [string]$OutDir = (Join-Path $PSScriptRoot 'dist\ctOpus-Bench'),
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (Test-Path $OutDir) { Remove-Item -Recurse -Force -LiteralPath $OutDir }
$bin = New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'bin')

Copy-Item (Join-Path $BinDir '*.exe'), (Join-Path $BinDir '*.dll') $bin
Copy-Item (Join-Path $PSScriptRoot 'benchmark\Compare-Folder.ps1'), (Join-Path $PSScriptRoot 'benchmark\run.cmd'), (Join-Path $PSScriptRoot 'benchmark\README.txt') $OutDir
Copy-Item (Join-Path $PSScriptRoot 'LICENSE.txt'), (Join-Path $PSScriptRoot 'LICENSE-Core.txt'), (Join-Path $PSScriptRoot 'COPYING') $OutDir

if (-not $NoZip) {
    $zip = "$OutDir.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "package: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 2)) MB)"
}
Write-Host "folder: $OutDir"
