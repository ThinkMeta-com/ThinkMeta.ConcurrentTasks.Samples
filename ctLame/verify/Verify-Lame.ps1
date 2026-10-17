<#
.SYNOPSIS
    Checks that ctLame.exe writes exactly the MP3 files of the reference lame.exe.

.DESCRIPTION
    Encodes a matrix of options and compares exit code and SHA-256 of every output. Any
    difference fails the run (exit code 1). Two sources of truth:

    -Signals   the synthetic test signals of New-TestSignals.ps1 (computed, the same on every
               machine) against Reference-Hashes-Signals.csv, the hashes the published
               bin\lame.exe writes for them.
    -WavDir    WAV files of your own: every case is encoded by lame.exe and by ctLame.exe and the
               two outputs are compared.

    Both can be given in one run. By default both programs come from this repository's Release
    build (build\Release\bin): lame.exe built from lame\, the reduced LAME 4.0 that ctLame is a
    port of, and ctLame.exe built from src\. The reduced lame.exe writes the same files as
    unmodified LAME 4.0 with '--resample <input rate>' where LAME would resample.

    Matrices:
      Smoke  three -V/-q pairs and three option cases (3 of your files, all signals)
      Quick  -V 0/2/4/6.5/9 x -q 0/3/7 (20 of your files, all signals), plus 24 option cases
             at -V 2 (5 of your files, all signals)
      Full   -V 0/2/4/6 at the default -q (all files)
    Your files are spread evenly over the sorted list.

    -WriteTable encodes the signal cases of Quick and Full with -Lame and writes the table; this
    is how Reference-Hashes-Signals.csv was made with bin\lame.exe.

    Needs PowerShell 7 (ForEach-Object -Parallel).

.EXAMPLE
    .\Verify-Lame.ps1 -Signals
    .\Verify-Lame.ps1 -WavDir D:\music\wav -Matrix Full
    .\Verify-Lame.ps1 -Signals -WavDir D:\music\wav -Matrix Smoke -CtLameArgs '--threads 1'
#>
#Requires -Version 7
param(
    [switch]$Signals,
    [string[]]$WavDir = @(),
    [ValidateSet('Smoke', 'Quick', 'Full')][string]$Matrix = 'Quick',
    [string]$CtLame = (Join-Path $PSScriptRoot '..\build\Release\bin\ctLame.exe'),
    [string]$Lame = (Join-Path $PSScriptRoot '..\build\Release\bin\lame.exe'),
    [string]$CtLameArgs = '',                    # extra options for ctLame only, e.g. '--threads 1' or '--sequential'
    [string]$Table = (Join-Path $PSScriptRoot 'Reference-Hashes-Signals.csv'),
    [switch]$WriteTable,
    [int]$ThrottleLimit = [Environment]::ProcessorCount,
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'ctLame-verify')
)

$ErrorActionPreference = 'Stop'
if ($WriteTable) { $Signals = $true }
if (-not $Signals -and $WavDir.Count -eq 0) { throw 'nothing to check: pass -Signals and/or -WavDir <folder>' }

function Resolve-Exe([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "$Path not found - build ctLame.slnx (Release, x64) first or pass -CtLame/-Lame" }
    (Resolve-Path -LiteralPath $Path).Path
}
$needLame = $WriteTable -or $WavDir.Count -gt 0
if ($needLame) { $Lame = Resolve-Exe $Lame }
if (-not $WriteTable) { $CtLame = Resolve-Exe $CtLame }

$VbrQualities = '0', '2', '4', '6.5', '9'
$Qualities = '0', '3', '7'
$OptionCases = @(
    '-m m', '-m s', '-m j', '-m f', '-a',
    '-b 128 -B 256', '-b 96 -B 320 -F',
    '--lowpass 16', '--lowpass 18 --lowpass-width 1', '--highpass 1',
    '--nores', '--strictly-enforce-ISO', '-p', '-t', '-c -o', '-e 5', '-Y',
    '--scale 0.5', '--gain -6', '--swap-channel',
    '--preset medium', '--preset standard', '--preset extreme', '--r3mix'
) | ForEach-Object { "-V 2 $_" }
$SmokeCases = '-V 0 -q 0', '-V 4 -q 3', '-V 9 -q 7', '-V 2 -m m', '-V 2 --scale 0.5', '-V 2 --swap-channel'
$GridCases = foreach ($v in $VbrQualities) { foreach ($q in $Qualities) { "-V $v -q $q" } }
$FullCases = '-V 0', '-V 2', '-V 4', '-V 6'

# count items spread evenly over the list
function Select-Spread([object[]]$Items, [int]$Count) {
    if ($Items.Count -le $Count) { return $Items }
    for ($k = 0; $k -lt $Count; $k++) { $Items[[math]::Floor($k * $Items.Count / $Count)] }
}

function New-Cases([string[]]$ArgSets, [object[]]$Inputs, [string]$Against) {
    foreach ($a in $ArgSets) {
        foreach ($i in $Inputs) { [pscustomobject]@{ Args = $a; Input = $i.FullName; Name = $i.Name; Against = $Against } }
    }
}

New-Item -ItemType Directory -Force $WorkDir | Out-Null
$cases = @()

if ($Signals) {
    $signalDir = Join-Path $WorkDir 'signals'
    & (Join-Path $PSScriptRoot 'New-TestSignals.ps1') -OutDir $signalDir | Out-Null
    $signalFiles = @(Get-ChildItem -LiteralPath $signalDir -Filter *.wav -File | Sort-Object Name)
    $sets = if ($WriteTable) { @($GridCases) + $OptionCases + $FullCases }
            else { switch ($Matrix) { 'Smoke' { $SmokeCases } 'Quick' { @($GridCases) + $OptionCases } 'Full' { $FullCases } } }
    $cases += @(New-Cases $sets $signalFiles 'table')
}
if ($WavDir.Count -gt 0 -and -not $WriteTable) {
    $files = @(foreach ($dir in $WavDir) { Get-ChildItem -LiteralPath $dir -Filter *.wav -File }) | Sort-Object FullName
    if ($files.Count -eq 0) { throw "no WAV files in $($WavDir -join ', ')" }
    switch ($Matrix) {
        'Smoke' { $cases += @(New-Cases $SmokeCases (Select-Spread $files 3) 'lame') }
        'Quick' {
            $quick = @(Select-Spread $files 20)
            $cases += @(New-Cases $GridCases $quick 'lame') + @(New-Cases $OptionCases (Select-Spread $quick 5) 'lame')
        }
        'Full' { $cases += @(New-Cases $FullCases $files 'lame') }
    }
}

$reference = @{}
if ($Signals -and -not $WriteTable) {
    Import-Csv -LiteralPath $Table | ForEach-Object { $reference[$_.Key] = $_ }
}

$what = if ($WriteTable) { "writing $Table with $Lame" } else { "$CtLame $CtLameArgs" }
Write-Host "$(if ($WriteTable) { 'Table' } else { $Matrix }): $($cases.Count) cases, $what (x$ThrottleLimit)"
$sw = [Diagnostics.Stopwatch]::StartNew()

$results = $cases | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
    $c = $_

    # Encodes the case with one program; returns exit code and SHA-256 of the output.
    function Invoke-Encode([string]$Exe, [string]$Extra) {
        $out = Join-Path $using:WorkDir ([guid]::NewGuid().ToString() + '.mp3')
        $argList = @('--silent') + @($Extra -split ' ' | Where-Object { $_ }) + @($c.Args -split ' ') + @($c.Input, $out)
        $p = Start-Process -FilePath $Exe -ArgumentList ($argList | ForEach-Object { '"' + $_ + '"' }) `
            -NoNewWindow -Wait -PassThru -RedirectStandardError "$out.err"
        $hash = ''
        if (Test-Path -LiteralPath $out) {
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $out).Hash.ToLowerInvariant()
            Remove-Item -LiteralPath $out
        }
        Remove-Item -LiteralPath "$out.err" -ErrorAction SilentlyContinue
        [pscustomobject]@{ ExitCode = "$($p.ExitCode)"; Sha256 = $hash }
    }

    $key = "$($c.Args)|$($c.Name)"
    if ($using:WriteTable) {
        $r = Invoke-Encode $using:Lame ''
        return [pscustomobject]@{ Key = $key; ExitCode = $r.ExitCode; Sha256 = $r.Sha256 }
    }
    $expected = if ($c.Against -eq 'lame') { Invoke-Encode $using:Lame '' } else { ($using:reference)[$key] }
    $actual = Invoke-Encode $using:CtLame $using:CtLameArgs
    $problem = if (-not $expected) { 'not in the table' }
               elseif ($expected.ExitCode -ne $actual.ExitCode) { "exit code $($expected.ExitCode) -> $($actual.ExitCode)" }
               elseif ($expected.Sha256 -ne $actual.Sha256) { 'output differs' }
    $source = if ($c.Against -eq 'lame') { 'lame.exe' } else { 'table' }
    [pscustomobject]@{ Case = "$($c.Args) | $($c.Name)"; Against = $source; Problem = $problem }
}

Write-Host ("Encoded in {0:mm\:ss}" -f $sw.Elapsed)
if ($WriteTable) {
    $results | Sort-Object Key | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath $Table
    $nonZero = @($results | Where-Object ExitCode -ne '0').Count
    Write-Host "wrote $($results.Count) hashes to $Table ($nonZero non-zero exit codes)"
    return
}
$failed = @($results | Where-Object Problem)
$sources = ($results | Group-Object Against | ForEach-Object { "$($_.Count) against $($_.Name)" }) -join ', '
if ($failed.Count -eq 0) {
    Write-Host "OK: all $($results.Count) cases bit-identical ($sources)" -ForegroundColor Green
}
else {
    $failed | Sort-Object Case | Format-Table -AutoSize | Out-String -Width 300 | Write-Host
    Write-Host "FAIL: $($failed.Count) of $($results.Count) cases differ ($sources)" -ForegroundColor Red
    exit 1
}
