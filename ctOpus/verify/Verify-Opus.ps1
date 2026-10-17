#Requires -Version 7
<#
.SYNOPSIS
    Bit-identity check of an opusenc-compatible encoder (ctOpus) against opusenc.

.DESCRIPTION
    Encodes a fixed test matrix and compares the SHA-256 of every output .opus file with a table
    written by opusenc (opus-tools 0.2, libopusenc 0.2.1, libopus 1.6.1: bin\opusenc.exe, built from
    opus\).

    -Mode Compare    encodes the matrix with -Exe and reports every case whose hash, size or exit
                     code differs from the table. Exit code 1 on any difference.
    -Mode Reference  writes the table with -Exe (default ..\bin\opusenc.exe), e.g. for a folder of
                     your own WAV files (-CorpusDir, with -HashFile).

    The inputs are the synthetic test files of New-TestWavs.ps1 (written on first use to inputs\):
    computed signals, mono to 10 channels, 8 to 96 kHz, 8-bit to float. The tables in this folder
    were written by bin\opusenc.exe for them. -CorpusDir adds WAV files of your own; their hashes
    are not in the tables, so write your own table first:

      .\Verify-Opus.ps1 -Mode Reference -Matrix Full -CorpusDir D:\Music -HashFile my-Full.csv
      .\Verify-Opus.ps1 -Mode Compare -Matrix Full -CorpusDir D:\Music -HashFile my-Full.csv -Exe ..\build\Release\bin\ctOpus.exe

    A case is keyed by its matrix arguments plus the input file name. -BaseArgs (default
    '--quiet --serial 1': opusenc otherwise picks a random Ogg serial number per run) and
    -ExtraArgs (candidate only, e.g. '--threads 4' or '--sequential') are not part of the key.
    -Repeat n encodes every case n times; all n outputs must match, which catches output that
    depends on thread timing.

    Matrices:
      Quick     the synthetic set x --comp 0/5/10 x --bitrate 64/128, plus option cases at --comp 10
                (bit rates, CBR/CVBR, frame sizes 2.5 to 60 ms, music/speech, loss, downmix, delay)
      Smoke     a subset of Quick (its table): three comp/bitrate pairs and three option cases
      Silk      SILK and hybrid: low bit rates, --speech, the SILK frame sizes, FEC, CBR/CVBR and all
                complexities
      Surround  the multichannel set (3 to 8 channels: mapping family 1, 10 channels: family 255) x
                complexity, bit rate, frame size, CBR/CVBR, downmix, speech and loss options
      Full      the files of -CorpusDir x (--comp 10 --bitrate 128, --comp 5 --bitrate 96, --comp 0 --bitrate 64)

.EXAMPLE
    .\Verify-Opus.ps1 -Mode Compare -Matrix Quick -Exe ..\build\Release\bin\ctOpus.exe
    .\Verify-Opus.ps1 -Mode Compare -Matrix Smoke -Exe ..\bin\ctOpus.exe -ExtraArgs '--threads 4' -Repeat 3
#>
param(
    [Parameter(Mandatory)][ValidateSet('Reference', 'Compare')][string]$Mode,
    [Parameter(Mandatory)][ValidateSet('Smoke', 'Quick', 'Full', 'Surround', 'Silk')][string]$Matrix,
    [string]$Exe = (Join-Path $PSScriptRoot '..\bin\opusenc.exe'),
    [string]$HashFile,
    [string]$CorpusDir = '',
    [string]$SynthDir = (Join-Path $PSScriptRoot 'inputs\synth'),
    [string]$SurroundDir = (Join-Path $PSScriptRoot 'inputs\surround'),
    [string]$BaseArgs = '--quiet --serial 1',
    [string]$ExtraArgs = '',
    [ValidateRange(1, 100)][int]$Repeat = 1,
    [int]$ThrottleLimit = [Environment]::ProcessorCount,
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'ctOpusVerify')
)

$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
if ($Mode -eq 'Reference' -and ($ExtraArgs -or $Repeat -gt 1)) { throw '-ExtraArgs and -Repeat are for compare runs only.' }
if ($Matrix -eq 'Full' -and -not $CorpusDir) { throw 'The Full matrix encodes the WAV files of -CorpusDir (with a table of your own, -HashFile).' }
if (-not $HashFile) {
    if ($CorpusDir) { throw '-CorpusDir needs -HashFile: the tables here only hold the synthetic test files.' }
    $tableMatrix = if ($Matrix -eq 'Smoke') { 'Quick' } else { $Matrix } # Smoke cases are Quick cases
    $HashFile = Join-Path $PSScriptRoot "Reference-Hashes-$tableMatrix.csv"
}
if (-not (Test-Path (Join-Path $SynthDir '*.wav')) -or -not (Test-Path (Join-Path $SurroundDir '*.wav'))) {
    & (Join-Path $PSScriptRoot 'New-TestWavs.ps1') -OutDir (Split-Path $SynthDir) | Out-Null
}

function Get-WavSampleRate([string]$Path) {
    $fs = [IO.File]::OpenRead($Path)
    try {
        $buf = New-Object byte[] 4096
        $n = $fs.Read($buf, 0, $buf.Length)
        for ($i = 12; $i -lt $n - 16; $i++) {
            if ($buf[$i] -eq 0x66 -and $buf[$i + 1] -eq 0x6D -and $buf[$i + 2] -eq 0x74 -and $buf[$i + 3] -eq 0x20) {
                return [BitConverter]::ToUInt32($buf, $i + 12)
            }
        }
        throw "no fmt chunk in $Path"
    }
    finally { $fs.Dispose() }
}

$CompBitrates = foreach ($c in '0', '5', '10') { foreach ($b in '64', '128') { "--comp $c --bitrate $b" } }
$OptionCases = @(
    '', '--bitrate 32', '--bitrate 256', '--cvbr', '--hard-cbr',
    '--framesize 2.5', '--framesize 5', '--framesize 10', '--framesize 40', '--framesize 60',
    '--music', '--speech', '--expect-loss 10', '--downmix-mono', '--no-phase-inv', '--max-delay 0'
) | ForEach-Object { "--comp 10 $_".TrimEnd() }

$corpus = if ($CorpusDir) { @(Get-ChildItem -LiteralPath $CorpusDir -Filter *.wav | Sort-Object Name) } else { @() }
$synth = @(Get-ChildItem -LiteralPath $SynthDir -Filter *.wav | Sort-Object Name)

$rates = @{}
function New-Cases([string[]]$ArgSets, $Inputs) {
    foreach ($i in $Inputs) {
        if (-not $rates.ContainsKey($i.FullName)) { $rates[$i.FullName] = Get-WavSampleRate $i.FullName }
    }
    foreach ($a in $ArgSets) {
        foreach ($i in $Inputs) {
            [pscustomobject]@{ Args = $a; InRate = $rates[$i.FullName]; Input = $i.FullName; Name = $i.Name }
        }
    }
}

# own corpus: every 17th file joins the synthetic set in Quick, Smoke (3 files) and Silk (3 files)
$every17 = @(for ($k = 0; $k -lt $corpus.Count; $k += 17) { $corpus[$k] })
if ($Matrix -eq 'Smoke') {
    if ($Mode -eq 'Reference') { throw 'Smoke uses the Quick table; write the Quick table instead.' }
    $cases = @(New-Cases ('--comp 0 --bitrate 64', '--comp 5 --bitrate 128', '--comp 10 --bitrate 128',
            '--comp 10 --framesize 2.5', '--comp 10 --hard-cbr', '--comp 10 --downmix-mono') (@($every17 | Select-Object -First 3) + $synth))
}
elseif ($Matrix -eq 'Surround') {
    $surround = @(Get-ChildItem -LiteralPath $SurroundDir -Filter *.wav | Sort-Object Name)
    $cases = @(New-Cases ('--comp 10', '--comp 5', '--comp 0', '--comp 10 --bitrate 512', '--comp 10 --bitrate 96',
            '--comp 10 --framesize 5', '--comp 10 --framesize 10', '--comp 10 --framesize 60', '--comp 10 --hard-cbr',
            '--comp 10 --cvbr', '--comp 10 --downmix-stereo', '--comp 10 --speech', '--comp 10 --expect-loss 10') $surround)
}
elseif ($Matrix -eq 'Silk') {
    $silkArgs = @('--bitrate 6', '--bitrate 8', '--bitrate 10', '--bitrate 12', '--bitrate 16', '--bitrate 20', '--bitrate 24',
        '--speech --bitrate 12', '--speech --bitrate 20', '--speech --bitrate 32', '--speech --bitrate 48', '--speech --bitrate 64',
        '--speech --bitrate 16 --framesize 10', '--speech --bitrate 16 --framesize 40', '--speech --bitrate 16 --framesize 60',
        '--speech --bitrate 16 --expect-loss 15', '--bitrate 24 --expect-loss 25', '--speech --bitrate 16 --hard-cbr',
        '--speech --bitrate 16 --cvbr', '--speech --bitrate 12 --downmix-mono', '--speech --bitrate 32 --no-phase-inv') |
        ForEach-Object { "--comp 10 $_" }
    $silkArgs += foreach ($c in '0', '1', '2', '3', '4', '5', '6', '7', '8', '9') { "--comp $c --speech --bitrate 16"; "--comp $c --bitrate 32" }
    $cases = @(New-Cases $silkArgs (@($every17 | Select-Object -First 3) + $synth))
}
elseif ($Matrix -eq 'Quick') {
    $cases = @(New-Cases $CompBitrates (@($every17) + $synth)) + @(New-Cases $OptionCases (@($every17 | Select-Object -First 5) + $synth))
}
else {
    $cases = @(New-Cases ('--comp 10 --bitrate 128', '--comp 5 --bitrate 96', '--comp 0 --bitrate 64') $corpus)
}

New-Item -ItemType Directory -Force $WorkDir | Out-Null
$extra = if ($ExtraArgs) { " + $ExtraArgs" } else { '' }
$times = if ($Repeat -gt 1) { " x$Repeat runs" } else { '' }
Write-Host "$Mode/$Matrix`: $($cases.Count) cases$times with $Exe $BaseArgs$extra (x$ThrottleLimit), table $(Split-Path -Leaf $HashFile)"
$sw = [Diagnostics.Stopwatch]::StartNew()

$runs = foreach ($c in $cases) { for ($r = 0; $r -lt $Repeat; $r++) { [pscustomobject]@{ Case = $c; Run = $r } } }
$results = $runs | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
    $c = $_.Case
    $out = Join-Path $using:WorkDir ([guid]::NewGuid().ToString() + '.opus')
    $argList = @($using:BaseArgs -split ' ' | Where-Object { $_ }) + @($using:ExtraArgs -split ' ' | Where-Object { $_ }) +
        @($c.Args -split ' ' | Where-Object { $_ }) + @($c.Input, $out)
    $p = Start-Process -FilePath $using:Exe -ArgumentList ($argList | ForEach-Object { '"' + $_ + '"' }) `
        -NoNewWindow -Wait -PassThru -RedirectStandardError "$out.err" -RedirectStandardOutput "$out.out"
    $r = [pscustomobject]@{ Key = "$($c.Args)|$($c.Name)"; Args = $c.Args; Input = $c.Name; Run = $_.Run
        ExitCode = $p.ExitCode; Size = -1; Sha256 = ''; InRate = $c.InRate; HeadRate = 0; Channels = 0 }
    if (Test-Path $out) {
        $r.Sha256 = (Get-FileHash -Algorithm SHA256 $out).Hash.ToLowerInvariant()
        $r.Size = (Get-Item $out).Length
        # first Ogg page: 27-byte header + 1 lacing byte, then OpusHead
        $fs = [IO.File]::OpenRead($out)
        try {
            $h = New-Object byte[] 47
            if ($fs.Read($h, 0, 47) -eq 47 -and [Text.Encoding]::ASCII.GetString($h, 28, 8) -eq 'OpusHead') {
                $r.Channels = $h[37]
                $r.HeadRate = [BitConverter]::ToUInt32($h, 40)
            }
        }
        finally { $fs.Dispose() }
        Remove-Item $out
    }
    Remove-Item "$out.err", "$out.out" -ErrorAction SilentlyContinue
    $r
} | Sort-Object Key, Run

Write-Host ("Encoded in {0:mm\:ss}" -f $sw.Elapsed)
$failed = @($results | Where-Object ExitCode -ne 0)

if ($Mode -eq 'Reference') {
    $results | Select-Object Key, ExitCode, Size, Sha256, InRate, HeadRate, Channels |
        Export-Csv -NoTypeInformation -Encoding utf8 $HashFile
    Write-Host "Wrote $($results.Count) hashes to $HashFile ($($failed.Count) non-zero exit codes)"
    $failed | ForEach-Object { Write-Warning "exit $($_.ExitCode): $($_.Key)" }
    $badRate = @($results | Where-Object { $_.ExitCode -eq 0 -and $_.HeadRate -ne $_.InRate })
    $badRate | ForEach-Object { Write-Warning "OpusHead rate $($_.HeadRate) <> input rate $($_.InRate): $($_.Key)" }
    return
}

$reference = @{}
Import-Csv $HashFile | ForEach-Object { $reference[$_.Key] = $_ }
$seen = @{}
$diffs = foreach ($r in $results) {
    $seen[$r.Key] = $true
    $ref = $reference[$r.Key]
    $run = if ($Repeat -gt 1) { " (run $($r.Run + 1))" } else { '' }
    if (-not $ref) { [pscustomobject]@{ Key = $r.Key; Problem = "not in reference$run" }; continue }
    if ("$($r.ExitCode)" -ne $ref.ExitCode) { [pscustomobject]@{ Key = $r.Key; Problem = "exit $($ref.ExitCode) -> $($r.ExitCode)$run" } }
    elseif ($r.Sha256 -ne $ref.Sha256) { [pscustomobject]@{ Key = $r.Key; Problem = "hash differs (size $($ref.Size) -> $($r.Size))$run" } }
    elseif ($r.ExitCode -eq 0 -and $r.HeadRate -ne $r.InRate) {
        [pscustomobject]@{ Key = $r.Key; Problem = "OpusHead rate $($r.HeadRate) <> input rate $($r.InRate)$run" }
    }
}
# Smoke runs a subset of its table: only cases of this run count as missing
$caseKeys = @{}
$cases | ForEach-Object { $caseKeys["$($_.Args)|$($_.Name)"] = $true }
$missing = @($reference.Keys | Where-Object { $caseKeys.ContainsKey($_) -and -not $seen.ContainsKey($_) } |
    ForEach-Object { [pscustomobject]@{ Key = $_; Problem = 'missing in run' } })
$all = @($diffs) + $missing

if ($all.Count -eq 0) {
    Write-Host "OK: all $($results.Count) encodes bit-identical to $HashFile" -ForegroundColor Green
}
else {
    $all | Sort-Object Key | Format-Table -AutoSize | Out-String -Width 300 | Write-Host
    Write-Host "FAIL: $($all.Count) of $($results.Count) encodes differ" -ForegroundColor Red
    exit 1
}
