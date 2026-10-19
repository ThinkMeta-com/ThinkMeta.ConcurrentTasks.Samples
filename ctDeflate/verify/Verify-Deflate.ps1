<#
.SYNOPSIS
    Checks that a ctDeflate.exe writes exactly the .gz files of libdeflate 1.26 gzip.exe, byte for byte.

.DESCRIPTION
    Requires PowerShell 7 (the cases run in parallel with ForEach-Object -Parallel).

    -Matrix Generated (default): synthetic inputs that this script generates itself (random, periodic, text-like and
      mostly-zero data, 22 MB in all), levels 1 to 9, against the SHA-256 table Reference-Hashes-Generated.csv.
      Needs no download.
    -Matrix Quick: the Silesia corpus (12 files, 212 MB), levels 1, 2, 5, 6, 8, 9, against Reference-Hashes-Quick.csv.
    -Matrix Full: Silesia and enwik9 (1.2 GB), levels 1 to 9, against Reference-Hashes-Full.csv.
      Silesia and enwik9 go to verify\corpus (silesia\<12 files> and enwik9), see the README.
    -Files: your own files or folders, compared directly with bin\gzip.exe at every level of -Levels.

    Each case runs ctDeflate.exe -<level> [-ExtraArgs] -o <temporary file> <input>; the outputs go to a temporary
    folder. A run that takes longer than -TimeoutSec counts as a hang and fails. -Mode Reference writes the tables
    from gzip.exe instead (for maintainers).

    Exit code: 0 if every case is identical, 1 otherwise.

    Parameters, the same in the Verify script of every sample:
      -Mode Compare|Reference  Compare (default) checks -Exe; Reference writes the table with -Oracle.
      -Matrix                  the test matrix; -HashFile the table (default: the one of the matrix here).
      -Exe                     the program to check (default: build\Release\bin\ctDeflate.exe).
      -Oracle                  the reference program (default: bin\gzip.exe).
      -ExtraArgs               options for -Exe only (e.g. '--threads 4', '--sequential').
      -Repeat n                every case n times; all outputs must match (catches thread-timing dependent output).
      -ThrottleLimit, -WorkDir cases run at once and the folder of the temporary outputs.
      -TimeoutSec              a run beyond it counts as a hang: killed, exit code 'timeout'.
    -Jobs, the name of earlier versions for -ThrottleLimit, still works.

.EXAMPLE
    pwsh verify\Verify-Deflate.ps1 -Exe build\Release\bin\ctDeflate.exe
    pwsh verify\Verify-Deflate.ps1 -Exe build\Release\bin\ctDeflate.exe -Matrix Quick -ExtraArgs '--threads 2 --chunk 4096' -Repeat 2
    pwsh verify\Verify-Deflate.ps1 -Exe build\Release\bin\ctDeflate.exe -Files D:\Data -Levels 1,6,9
#>
param(
    [ValidateSet('Reference', 'Compare')][string]$Mode = 'Compare',
    [ValidateSet('Generated', 'Quick', 'Full')][string]$Matrix = 'Generated',
    [string]$Exe = '',
    [string]$Oracle = '',
    [string]$HashFile = '',
    [string]$ExtraArgs = '',
    [ValidateRange(1, 100)][int]$Repeat = 1,
    [Alias('Jobs')][int]$ThrottleLimit = 8, # ctDeflate itself runs on all logical processors
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'ctDeflateVerify'),
    [int]$TimeoutSec = 300,
    [string[]]$Files = @(),
    [string]$Levels = '1,6,9',
    [string]$Corpus = ''
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Verify-Deflate.ps1 needs PowerShell 7 (pwsh).' }
$root = Split-Path $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'build\Release\bin\ctDeflate.exe' }
if (-not $Oracle) { $Oracle = Join-Path $root 'bin\gzip.exe' }
if (-not $Corpus) { $Corpus = Join-Path $PSScriptRoot 'corpus' }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$Oracle = (Resolve-Path -LiteralPath $Oracle).Path
if (-not $HashFile) { $HashFile = Join-Path $PSScriptRoot "Reference-Hashes-$Matrix.csv" }
if ($Mode -eq 'Reference' -and ($ExtraArgs -or $Repeat -gt 1)) { throw '-ExtraArgs and -Repeat are for compare runs only.' }

$tmp = Join-Path $WorkDir ([guid]::NewGuid().ToString())
New-Item -ItemType Directory -Force $tmp | Out-Null

# Deterministic synthetic inputs (xorshift64*), the same on every machine.
function New-GeneratedInputs([string]$dir) {
    Add-Type -TypeDefinition @'
public static class CtDeflateVerifyData {
    static ulong state;
    static ulong Next() { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 2685821657736338717UL; }
    public static byte[] Make(string kind, int size, ulong seed) {
        state = seed; var b = new byte[size];
        string[] words = { "the ", "of ", "and ", "data ", "block ", "match ", "deflate ", "stream ", "level ", "chunk ",
                           "resolve ", "parse ", "\r\n", "1234 ", "<tag>", "</tag>", "value=\"", "\" ", "ThinkMeta ", "zero " };
        int i = 0;
        switch (kind) {
            case "random": for (; i < size; i++) b[i] = (byte)Next(); break;
            case "periodic": for (; i < size; i++) b[i] = (byte)((i % 251) ^ (i / 4093 % 7)); break;
            case "text":
                while (i < size) { var w = words[(int)(Next() % (ulong)words.Length)]; for (int k = 0; k < w.Length && i < size; k++) b[i++] = (byte)w[k]; }
                break;
            case "sparse":
                for (; i < size; i++) b[i] = (Next() % 97 == 0) ? (byte)Next() : (byte)0;
                break;
            case "mixed":
                for (; i < size; i++) { int part = i / 65536 % 3; b[i] = part == 0 ? (byte)Next() : part == 1 ? (byte)(i % 13) : (byte)(97 + (int)(Next() % 6)); }
                break;
        }
        return b;
    }
}
'@
    $set = @(@('random.bin', 'random', 4MB), @('periodic.bin', 'periodic', 4MB), @('text.txt', 'text', 8MB),
        @('sparse.bin', 'sparse', 2MB), @('mixed.bin', 'mixed', 4MB), @('small.txt', 'text', 3000), @('tiny.bin', 'random', 40))
    $seed = [uint64]::Parse('11400714819323198485') # 0x9E3779B97F4A7C15
    foreach ($s in $set) {
        [IO.File]::WriteAllBytes((Join-Path $dir $s[0]), [CtDeflateVerifyData]::Make($s[1], [int]$s[2], $seed))
        $seed = $seed + [uint64]1
    }
}

try {
    # the cases: input path, name in the table, level
    if ($Files.Count -gt 0) {
        $levelList = @($Levels -split '[,; ]' | Where-Object { $_ } | ForEach-Object { [int]$_ })
        $inputs = foreach ($f in $Files) {
            if (Test-Path -LiteralPath $f -PathType Container) { Get-ChildItem -LiteralPath $f -File -Recurse | Where-Object Length -GT 0 }
            elseif (Test-Path -LiteralPath $f -PathType Leaf) { Get-Item -LiteralPath $f }
            else { throw "not found: $f" }
        }
        $cases = foreach ($i in $inputs) { foreach ($l in $levelList) { [pscustomobject]@{ Path = $i.FullName; Name = $i.FullName; Level = $l } } }
        $live = $true
    } else {
        $levelList = if ($Matrix -eq 'Quick') { 1, 2, 5, 6, 8, 9 } else { 1..9 }
        if ($Matrix -eq 'Generated') {
            $genDir = Join-Path $tmp 'generated'
            New-Item -ItemType Directory $genDir | Out-Null
            New-GeneratedInputs $genDir
            $names = Get-ChildItem $genDir -File | Sort-Object Name | ForEach-Object { "generated/$($_.Name)" }
            $base = $tmp
        } else {
            $names = @('dickens', 'mozilla', 'mr', 'nci', 'ooffice', 'osdb', 'reymont', 'samba', 'sao', 'webster', 'xml', 'x-ray') | ForEach-Object { "silesia/$_" }
            if ($Matrix -eq 'Full') { $names += 'enwik9' }
            $base = $Corpus
        }
        $cases = foreach ($n in $names) {
            $path = Join-Path $base ($n -replace '/', '\')
            if (-not (Test-Path -LiteralPath $path)) { throw "corpus file missing: $path (see the README: verify\corpus)" }
            foreach ($l in $levelList) { [pscustomobject]@{ Path = $path; Name = $n; Level = $l } }
        }
        $live = $false
    }

    $runs = foreach ($c in $cases) { for ($n = 0; $n -lt $Repeat; $n++) { [pscustomobject]@{ Path = $c.Path; Name = $c.Name; Level = $c.Level; Run = $n } } }
    $results = $runs | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
        $case = $_
        function Invoke-Case([string]$exe, [string[]]$arguments, [int]$timeoutSec) {
            $psi = [Diagnostics.ProcessStartInfo]::new($exe)
            foreach ($a in $arguments) { $psi.ArgumentList.Add($a) }
            $psi.UseShellExecute = $false
            $psi.RedirectStandardError = $true
            $proc = [Diagnostics.Process]::Start($psi)
            $null = $proc.StandardError.ReadToEndAsync()
            if (-not $proc.WaitForExit($timeoutSec * 1000)) { $proc.Kill(); $proc.WaitForExit(); return 'timeout' }
            return $proc.ExitCode
        }
        function Get-Sha([string]$path) { if (Test-Path -LiteralPath $path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash } else { '' } }

        $out = Join-Path $using:tmp ([guid]::NewGuid().ToString() + '.gz')
        $hash = ''; $size = 0; $code = 0; $ref = ''
        if ($using:Mode -eq 'Reference' -or ($using:live -and $case.Run -eq 0)) {
            # gzip.exe -c writes to stdout; cmd redirection keeps the bytes unchanged
            $refOut = "$out.ref"
            cmd /c "`"$using:Oracle`" -c -$($case.Level) `"$($case.Path)`" > `"$refOut`""
            $ref = Get-Sha $refOut
            if ($using:Mode -eq 'Reference') { $size = (Get-Item -LiteralPath $refOut).Length; $hash = $ref }
            Remove-Item -LiteralPath $refOut -ErrorAction SilentlyContinue
        }
        if ($using:Mode -eq 'Compare') {
            $arguments = @("-$($case.Level)")
            if ($using:ExtraArgs) { $arguments += @($using:ExtraArgs -split ' ' | Where-Object { $_ }) }
            $arguments += @('-o', $out, $case.Path)
            $code = Invoke-Case $using:Exe $arguments $using:TimeoutSec
            $hash = if ($code -eq 0) { Get-Sha $out } else { '' }
            $size = if (Test-Path -LiteralPath $out) { (Get-Item -LiteralPath $out).Length } else { 0 }
            Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
        }
        [pscustomobject]@{ File = $case.Name; Level = $case.Level; Run = $case.Run; ExitCode = $code; Size = $size; Sha256 = $hash; Oracle = $ref }
    }

    if ($Mode -eq 'Reference') {
        $results | Sort-Object File, Level | Select-Object File, Level, Size, Sha256 | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath $HashFile
        Write-Host "reference: $($results.Count) cases -> $HashFile"
        exit 0
    }

    if ($live) {
        $expected = @{}
        foreach ($r in $results | Where-Object Run -EQ 0) { $expected["$($r.File)|$($r.Level)"] = $r.Oracle }
    } else {
        $expected = @{}
        foreach ($r in Import-Csv -LiteralPath $HashFile) { $expected["$($r.File)|$($r.Level)"] = $r.Sha256 }
    }
    $bad = @($results | Where-Object { $_.ExitCode -ne 0 -or -not $_.Sha256 -or $_.Sha256 -ne $expected["$($_.File)|$($_.Level)"] })
    foreach ($b in $bad | Sort-Object File, Level, Run) {
        $why = if ($b.ExitCode -eq 'timeout') { 'hang' } elseif ($b.ExitCode -ne 0) { "exit code $($b.ExitCode)" } else { 'different bytes' }
        $run = if ($Repeat -gt 1) { " (run $($b.Run + 1))" } else { '' }
        Write-Host ("DIFFERENT {0} -{1}: {2}{3}" -f $b.File, $b.Level, $why, $run) -ForegroundColor Red
    }
    $label = if ($live) { 'files' } else { $Matrix }
    Write-Host ("{0}/{1} identical ({2}{3})" -f ($results.Count - $bad.Count), $results.Count, $label, $(if ($ExtraArgs) { ", $ExtraArgs" }))
    exit $(if ($bad.Count -eq 0) { 0 } else { 1 })
} finally {
    Remove-Item -Recurse -Force -LiteralPath $tmp -ErrorAction SilentlyContinue
}
