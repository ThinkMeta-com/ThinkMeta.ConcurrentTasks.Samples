# Copyright (c) 2026 ThinkMeta Software GmbH
# SPDX-License-Identifier: MIT (see LICENSE.txt)
<#
.SYNOPSIS
    Bit-compare harness for ctJpeg: every output must be byte for byte the output of jpegli
    (cjpegli) on the same SIMD target. Needs PowerShell 7 (cases run in parallel).

.DESCRIPTION
    Encodes a fixed test matrix with a cjpegli-compatible encoder and records the SHA256 of every
    output JPEG, separately per feature set (-Simd).

    -Mode Reference  writes the hash table (CSV) with the oracle: the unmodified jpegli library and
                     cjpegli (google/jpegli @ 031a007) with the Highway target fixed by
                     cjpegli --target <simd> (bin\cjpegli.exe, or built from jpeg\jpegli-oracle).
                     The tables in this folder were written that way.
    -Mode Compare    encodes the same matrix with a candidate binary and reports every case whose
                     hash, size or exit code differs from the table. Exit code 1 on any difference.

    Feature sets: sse2 (= jpegli SSE2, SSSE3 and SSE4, which give identical output) and avx2.
    The oracle gets '--target <simd>' in front of its arguments, any other binary gets
    '--simd <simd>' after them. -ExtraArgs (candidate only, e.g. '--threads 4' or '--sequential')
    is not part of the case key.

    -Repeat n encodes every case n times; all n outputs must match the table, which catches
    nondeterministic (thread-timing dependent) output.

    Inputs (testdata\, made by Prepare-Inputs.ps1): photos\*.ppm (imagecompression.info set,
    14 photos), images\*.ppm (3 synthetic, GenTestImages), edge\*.ppm (8 odd sizes 1x1 ..
    1001x777, GenTestImages --edge), gray\*.pgm (4 grayscale, GenTestImages --gray).

    Matrices:
      Quick  every 2nd photo + synthetic + edge + 2 gray x -q 50/75/90/95 x subsampling
             default/444/420, plus -d 0.5/2/5 on 3 photos + edge.
      Full   all inputs x -q 30/50/65/75/85/90/95/100 x subsampling default/444/420,
             plus -d 0.25/0.5/1/2/4/8.
      Smoke  a subset of Quick (its table): 3 photos + edge + 1 gray x four cases. For paths
             besides the main one (Debug, --sequential, --threads).

.EXAMPLE
    .\Verify-Jpeg.ps1 -Mode Compare -Matrix Quick -Simd avx2
    .\Verify-Jpeg.ps1 -Mode Compare -Matrix Full -Simd sse2 -Exe ..\bin\ctJpeg.exe
    .\Verify-Jpeg.ps1 -Mode Reference -Matrix Quick -Simd sse2 -Exe ..\bin\cjpegli.exe -HashFile my-table.csv
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Reference', 'Compare')][string]$Mode,
    [Parameter(Mandatory)][ValidateSet('Smoke', 'Quick', 'Full')][string]$Matrix,
    [Parameter(Mandatory)][ValidateSet('sse2', 'avx2')][string]$Simd,
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\Release\bin\ctJpeg.exe'), # your build; ..\bin\ctJpeg.exe for the published one
    [string]$HashFile,
    [string]$InputRoot = (Join-Path $PSScriptRoot '..\testdata'),
    [string]$ExtraArgs = '',
    [int]$Repeat = 1,
    [int]$ThrottleLimit = [Environment]::ProcessorCount,
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'JpegVerify')
)

$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$isOracle = (Split-Path -Leaf $Exe) -like 'cjpegli*'
if ($Mode -eq 'Reference' -and -not $isOracle) { throw 'Reference tables are written with cjpegli only.' }
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Verify-Jpeg.ps1 needs PowerShell 7 (pwsh).' }
if (-not $HashFile) {
    $tableMatrix = if ($Matrix -eq 'Smoke') { 'Quick' } else { $Matrix } # Smoke cases are Quick cases
    $HashFile = Join-Path $PSScriptRoot "Reference-Hashes-$tableMatrix-$Simd.csv"
}

function Get-Inputs([string]$Sub, [string]$Filter) {
    $dir = Join-Path $InputRoot $Sub
    $files = @(Get-ChildItem -LiteralPath $dir -Filter $Filter | Sort-Object Name)
    if ($files.Count -eq 0) { throw "No inputs in $dir" }
    $files
}
$photos = Get-Inputs 'photos' '*.ppm'
$synth = Get-Inputs 'images' '*.ppm'
$edge = Get-Inputs 'edge' '*.ppm'
$gray = Get-Inputs 'gray' '*.pgm'

# Three photos of the Quick set (every 2nd photo) for the distance cases and Smoke.
$keyPhotos = 'big_tree.ppm', 'cathedral.ppm', 'nightshot_iso_1600.ppm'
$Subsampling = '', ' --chroma_subsampling 444', ' --chroma_subsampling 420'
function New-Cases([string[]]$ArgSets, $Inputs) {
    foreach ($a in $ArgSets) {
        foreach ($i in $Inputs) { [pscustomobject]@{ Args = $a; Input = $i.FullName; Name = $i.Name } }
    }
}
function Get-QualityArgs([string[]]$Qualities) {
    foreach ($q in $Qualities) { foreach ($s in $Subsampling) { "-q $q$s" } }
}

if ($Matrix -eq 'Smoke') {
    if ($Mode -eq 'Reference') { throw 'Smoke uses the Quick table; write the Quick table instead.' }
    $smokePhotos = $photos | Where-Object { $_.Name -in $keyPhotos }
    $smokeGray = $gray | Where-Object Name -eq 'cathedral-gray.pgm'
    $cases = @(New-Cases ('-q 75', '-q 95 --chroma_subsampling 420', '-q 50 --chroma_subsampling 444') (@($smokePhotos) + $edge + $smokeGray)) +
        @(New-Cases ('-d 2') (@($smokePhotos) + $edge))
}
elseif ($Matrix -eq 'Quick') {
    $quickPhotos = for ($k = 0; $k -lt $photos.Count; $k += 2) { $photos[$k] }
    $quickGray = $gray | Where-Object { $_.Name -in 'cathedral-gray.pgm', 'text-gray.pgm' }
    $distancePhotos = $photos | Where-Object { $_.Name -in $keyPhotos }
    $cases = @(New-Cases (Get-QualityArgs '50', '75', '90', '95') (@($quickPhotos) + $synth + $edge + $quickGray)) +
        @(New-Cases ('-d 0.5', '-d 2', '-d 5') (@($distancePhotos) + $edge))
}
else {
    $all = @($photos) + $synth + $edge + $gray
    $cases = @(New-Cases (Get-QualityArgs '30', '50', '65', '75', '85', '90', '95', '100') $all) +
        @(New-Cases ('-d 0.25', '-d 0.5', '-d 1', '-d 2', '-d 4', '-d 8') $all)
}

New-Item -ItemType Directory -Force $WorkDir | Out-Null
$how = if ($isOracle) { "--target $Simd" } else { "--simd $Simd $ExtraArgs".Trim() }
Write-Host "$Mode/$Matrix/$Simd`: $($cases.Count) cases x$Repeat with $Exe ($how, x$ThrottleLimit), table $(Split-Path -Leaf $HashFile)"
$sw = [Diagnostics.Stopwatch]::StartNew()

$results = $cases | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
    $c = $_
    $runs = foreach ($n in 1..$using:Repeat) {
        $out = Join-Path $using:WorkDir ([guid]::NewGuid().ToString() + '.jpg')
        $caseArgs = @($c.Args -split ' ' | Where-Object { $_ })
        if ($using:isOracle) {
            $argList = @('--target', $using:Simd, $c.Input, $out) + $caseArgs
        }
        else {
            $argList = @($c.Input, $out) + $caseArgs + @('--simd', $using:Simd) + @($using:ExtraArgs -split ' ' | Where-Object { $_ })
        }
        $p = Start-Process -FilePath $using:Exe -ArgumentList ($argList | ForEach-Object { '"' + $_ + '"' }) `
            -NoNewWindow -Wait -PassThru -RedirectStandardError "$out.err" -RedirectStandardOutput "$out.log"
        $r = [pscustomobject]@{ ExitCode = $p.ExitCode; Size = -1; Sha256 = '' }
        if (Test-Path $out) {
            $r.Sha256 = (Get-FileHash -Algorithm SHA256 $out).Hash.ToLowerInvariant()
            $r.Size = (Get-Item $out).Length
            Remove-Item $out
        }
        Remove-Item "$out.err", "$out.log" -ErrorAction SilentlyContinue
        $r
    }
    $distinct = @($runs | ForEach-Object { "$($_.ExitCode)/$($_.Sha256)" } | Select-Object -Unique)
    $first = $runs | Select-Object -First 1
    [pscustomobject]@{ Key = "$($c.Args)|$($c.Name)"; ExitCode = $first.ExitCode; Size = $first.Size; Sha256 = $first.Sha256
        Unstable = $distinct.Count -gt 1 }
} | Sort-Object Key

Write-Host ("Encoded in {0:mm\:ss}" -f $sw.Elapsed)
$failed = @($results | Where-Object ExitCode -ne 0)

if ($Mode -eq 'Reference') {
    $unstable = @($results | Where-Object Unstable)
    if ($unstable.Count -gt 0) { $unstable | ForEach-Object { Write-Warning "oracle unstable: $($_.Key)" }; throw 'oracle output not deterministic' }
    $results | Select-Object Key, ExitCode, Size, Sha256 | Export-Csv -NoTypeInformation -Encoding utf8 $HashFile
    Write-Host "Wrote $($results.Count) hashes to $HashFile ($($failed.Count) non-zero exit codes)"
    $failed | ForEach-Object { Write-Warning "exit $($_.ExitCode): $($_.Key)" }
    return
}

$reference = @{}
Import-Csv $HashFile | ForEach-Object { $reference[$_.Key] = $_ }
$diffs = foreach ($r in $results) {
    $ref = $reference[$r.Key]
    if (-not $ref) { [pscustomobject]@{ Key = $r.Key; Problem = 'not in reference' }; continue }
    $reference.Remove($r.Key)
    if ($r.Unstable) { [pscustomobject]@{ Key = $r.Key; Problem = "output differs between the $Repeat runs" } }
    elseif ("$($r.ExitCode)" -ne $ref.ExitCode) { [pscustomobject]@{ Key = $r.Key; Problem = "exit $($ref.ExitCode) -> $($r.ExitCode)" } }
    elseif ($r.Sha256 -ne $ref.Sha256) { [pscustomobject]@{ Key = $r.Key; Problem = "hash differs (size $($ref.Size) -> $($r.Size))" } }
}
# Smoke runs a subset of its table - only cases of this run count as missing.
$caseKeys = @{}
$cases | ForEach-Object { $caseKeys["$($_.Args)|$($_.Name)"] = $true }
$missing = @($reference.Keys | Where-Object { $caseKeys.ContainsKey($_) } |
    ForEach-Object { [pscustomobject]@{ Key = $_; Problem = 'missing in run' } })
$all = @($diffs) + $missing

if ($all.Count -eq 0) {
    Write-Host "OK: all $($results.Count) cases bit-identical to $HashFile" -ForegroundColor Green
}
else {
    $all | Sort-Object Key | Format-Table -AutoSize | Out-String -Width 300 | Write-Host
    Write-Host "FAIL: $($all.Count) of $($results.Count) cases differ" -ForegroundColor Red
    exit 1
}
