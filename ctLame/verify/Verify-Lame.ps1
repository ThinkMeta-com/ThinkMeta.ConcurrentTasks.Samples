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

    -Mode Reference encodes the signal cases of Quick and Full with -Oracle and writes the table;
    this is how Reference-Hashes-Signals.csv was made with bin\lame.exe.

    Parameters, the same in the Verify script of every sample:
      -Mode Compare|Reference  Compare (default) checks -Exe; Reference writes the table with -Oracle.
      -Matrix                  the test matrix; -HashFile the table (default: the one of the matrix here).
      -Exe                     the program to check (default: ..\build\Release\bin\ctLame.exe).
      -Oracle                  the reference program (default: ..\build\Release\bin\lame.exe).
      -ExtraArgs               options for -Exe only (e.g. '--threads 4', '--sequential').
      -Repeat n                every case n times; all outputs must match (catches thread-timing dependent output).
      -ThrottleLimit, -WorkDir cases run at once and the folder of the temporary outputs.
      -TimeoutSec              a run beyond it counts as a hang: killed, exit code 'timeout'.
    The names of earlier versions still work: -CtLame (-Exe), -Lame (-Oracle), -CtLameArgs
    (-ExtraArgs), -Table (-HashFile), -WriteTable (-Mode Reference).

    Needs PowerShell 7 (ForEach-Object -Parallel).

.EXAMPLE
    .\Verify-Lame.ps1 -Signals
    .\Verify-Lame.ps1 -WavDir D:\music\wav -Matrix Full
    .\Verify-Lame.ps1 -Signals -WavDir D:\music\wav -Matrix Smoke -ExtraArgs '--threads 1' -Repeat 3
#>
#Requires -Version 7
param(
    [ValidateSet('Reference', 'Compare')][string]$Mode = 'Compare',
    [ValidateSet('Smoke', 'Quick', 'Full')][string]$Matrix = 'Quick',
    [Alias('CtLame')][string]$Exe = (Join-Path $PSScriptRoot '..\build\Release\bin\ctLame.exe'),
    [Alias('Lame')][string]$Oracle = (Join-Path $PSScriptRoot '..\build\Release\bin\lame.exe'),
    [Alias('Table')][string]$HashFile = (Join-Path $PSScriptRoot 'Reference-Hashes-Signals.csv'),
    [Alias('CtLameArgs')][string]$ExtraArgs = '', # options for ctLame only, e.g. '--threads 1' or '--sequential'
    [ValidateRange(1, 100)][int]$Repeat = 1,
    [int]$ThrottleLimit = [Environment]::ProcessorCount,
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'ctLame-verify'),
    [int]$TimeoutSec = 300,
    [switch]$Signals,
    [string[]]$WavDir = @(),
    [switch]$WriteTable # the name of earlier versions for -Mode Reference
)

$ErrorActionPreference = 'Stop'
if ($WriteTable) { $Mode = 'Reference' }
$WriteTable = $Mode -eq 'Reference'
if ($WriteTable -and ($ExtraArgs -or $Repeat -gt 1)) { throw '-ExtraArgs and -Repeat are for compare runs only.' }
if ($WriteTable) { $Signals = $true }
if (-not $Signals -and $WavDir.Count -eq 0) { throw 'nothing to check: pass -Signals and/or -WavDir <folder>' }

function Resolve-Exe([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "$Path not found - build ctLame.slnx (Release, x64) first or pass -Exe/-Oracle" }
    (Resolve-Path -LiteralPath $Path).Path
}
$needLame = $WriteTable -or $WavDir.Count -gt 0
if ($needLame) { $Oracle = Resolve-Exe $Oracle }
if (-not $WriteTable) { $Exe = Resolve-Exe $Exe }

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
    Import-Csv -LiteralPath $HashFile | ForEach-Object { $reference[$_.Key] = $_ }
}

$what = if ($WriteTable) { "writing $HashFile with $Oracle" } else { "$Exe $ExtraArgs".TrimEnd() }
$times = if ($Repeat -gt 1) { " x$Repeat runs" } else { '' }
Write-Host "$(if ($WriteTable) { 'Table' } else { $Matrix }): $($cases.Count) cases$times, $what (x$ThrottleLimit)"
$sw = [Diagnostics.Stopwatch]::StartNew()

$results = $cases | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
    $c = $_

    # Encodes the case with one program; returns exit code and SHA-256 of the output.
    function Invoke-Encode([string]$Exe, [string]$Extra) {
        $out = Join-Path $using:WorkDir ([guid]::NewGuid().ToString() + '.mp3')
        $argList = @('--silent') + @($Extra -split ' ' | Where-Object { $_ }) + @($c.Args -split ' ') + @($c.Input, $out)
        $p = Start-Process -FilePath $Exe -ArgumentList ($argList | ForEach-Object { '"' + $_ + '"' }) `
            -NoNewWindow -PassThru -RedirectStandardError "$out.err"
        $null = $p.Handle # keeps the exit code readable after the wait
        $code = if ($p.WaitForExit($using:TimeoutSec * 1000)) { $p.ExitCode } else { $p.Kill($true); $p.WaitForExit(); 'timeout' }
        $hash = ''
        if (Test-Path -LiteralPath $out) {
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $out).Hash.ToLowerInvariant()
            Remove-Item -LiteralPath $out
        }
        Remove-Item -LiteralPath "$out.err" -ErrorAction SilentlyContinue
        [pscustomobject]@{ ExitCode = "$code"; Sha256 = $hash }
    }

    $key = "$($c.Args)|$($c.Name)"
    if ($using:WriteTable) {
        $r = Invoke-Encode $using:Oracle ''
        return [pscustomobject]@{ Key = $key; ExitCode = $r.ExitCode; Sha256 = $r.Sha256 }
    }
    $expected = if ($c.Against -eq 'lame') { Invoke-Encode $using:Oracle '' } else { ($using:reference)[$key] }
    $runs = @(foreach ($n in 1..$using:Repeat) { Invoke-Encode $using:Exe $using:ExtraArgs })
    $actual = $runs[0]
    $unstable = @($runs | ForEach-Object { "$($_.ExitCode)/$($_.Sha256)" } | Select-Object -Unique).Count -gt 1
    $problem = if (-not $expected) { 'not in the table' }
               elseif ($unstable) { "output differs between the $($using:Repeat) runs" }
               elseif ($expected.ExitCode -ne $actual.ExitCode) { "exit code $($expected.ExitCode) -> $($actual.ExitCode)" }
               elseif ($expected.Sha256 -ne $actual.Sha256) { 'output differs' }
    $source = if ($c.Against -eq 'lame') { 'lame.exe' } else { 'table' }
    [pscustomobject]@{ Case = "$($c.Args) | $($c.Name)"; Against = $source; Problem = $problem }
}

Write-Host ("Encoded in {0:mm\:ss}" -f $sw.Elapsed)
if ($WriteTable) {
    $results | Sort-Object Key | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath $HashFile
    $nonZero = @($results | Where-Object ExitCode -ne '0').Count
    Write-Host "wrote $($results.Count) hashes to $HashFile ($nonZero non-zero exit codes)"
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
