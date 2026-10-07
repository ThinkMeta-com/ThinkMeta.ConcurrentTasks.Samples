<#
.SYNOPSIS
    Builds the standalone benchmark package for other PCs: bin\ (ctDeflate, gzip, scheduler core)
    with the benchmark script, optionally with a test corpus.

.DESCRIPTION
    The package needs no installation. Without -Corpus it holds no input files and the user points
    the script at files or folders of their own. With -Corpus the given folders and files are copied
    to corpus\ in the package and listed in Bench-Files.txt, so that run.cmd without arguments
    benchmarks them.

.EXAMPLE
    .\New-BenchPackage.ps1
    powershell -File .\New-BenchPackage.ps1 -Corpus D:\corpus\silesia,D:\corpus\enwik9 -CorpusNote D:\corpus\CORPUS.txt
#>
param(
    [string]$BinDir = '',
    [string]$OutDir = '',
    [string[]]$Corpus = @(),
    [string]$CorpusNote = '',   # a text file about the corpus (sources, terms), copied as CORPUS.txt
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
# Paths that depend on the script folder are resolved here, not in param() (Windows PowerShell 5.1).
$Corpus = @($Corpus | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) # -File passes "a,b" as one string
if (-not $BinDir) { $BinDir = Join-Path $PSScriptRoot 'bin' }
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot 'dist\ctDeflate-Bench' }
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (Test-Path $OutDir) { Remove-Item -Recurse -Force -LiteralPath $OutDir }
$bin = New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'bin')

Copy-Item (Join-Path $BinDir '*.exe'), (Join-Path $BinDir '*.dll') $bin
Copy-Item (Join-Path $PSScriptRoot 'benchmark\Compare-Gzip.ps1'), (Join-Path $PSScriptRoot 'benchmark\run.cmd'), (Join-Path $PSScriptRoot 'benchmark\README.txt') $OutDir
Copy-Item (Join-Path $PSScriptRoot 'LICENSE.txt'), (Join-Path $PSScriptRoot 'LICENSE-Core.txt'), (Join-Path $PSScriptRoot 'COPYING') $OutDir

if ($Corpus.Count -gt 0) {
    $corpusDir = New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'corpus')
    $entries = @('# Inputs of run.cmd without arguments (relative to this folder)')
    foreach ($c in $Corpus) {
        $item = Get-Item -LiteralPath $c
        Copy-Item -Recurse -LiteralPath $item.FullName (Join-Path $corpusDir $item.Name)
        $entries += "corpus\$($item.Name)"
    }
    Set-Content -LiteralPath (Join-Path $OutDir 'Bench-Files.txt') -Value $entries -Encoding ASCII
    if ($CorpusNote) { Copy-Item -LiteralPath $CorpusNote (Join-Path $OutDir 'CORPUS.txt') }
}

if (-not $NoZip) {
    $zip = "$OutDir.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "package: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 2)) MB)"
}
Write-Host "folder: $OutDir"
