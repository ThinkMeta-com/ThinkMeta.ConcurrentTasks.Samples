<#
.SYNOPSIS
    ctLame against lame.exe on a folder of WAV files: speed and bit identity.

.DESCRIPTION
    Runs from the unpacked package (New-BenchPackage.ps1). Windows PowerShell 5.1 or later.
    Encodes every WAV file of the folder with lame.exe (the reduced LAME 4.0, the reference) and
    ctLame, one program at a time; per file the programs run one after the other, round by round,
    so a slow phase of the computer hits all of them alike. Measured is the wall time of the whole
    process (start, reading, encoding, writing).

    Cold: before every measured run the WAV file is copied unbuffered (robocopy /J) into work\cold,
    so no program reads it from the file cache, as when a file is encoded for the first time; the
    copy is not timed and is deleted afterwards. -Warm reads the files where they are.

    Bit identity: ctLame's MP3 must be byte for byte the same as lame.exe's (first round).

    The report lists per file the audio length, the median time of each program and the factor,
    and the totals (sum of the medians) as a multiple of real time and of lame.exe. It goes to
    results\bench-<computer>-<time>.txt, the numbers per file to a .csv next to it.

    Exit code: 0 if every file was encoded by both programs with identical bytes, 2 if a file
    differs or could not be encoded, 1 on other errors (folder or program missing).

    Power plan: for the run the script switches to High Performance (Ultimate Performance if that
    is the only one; where the plan is hidden, a temporary copy) and restores the previous plan at
    the end. If Windows refuses the switch (e.g. Windows Server without administrator rights, or a
    group policy), the report says so and the run goes on with the current plan. -KeepPowerPlan
    leaves the plan alone.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 D:\Musik
    powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 D:\Musik -Rounds 3 -Sequential -EncodeArgs '-V 2'
#>
param(
    [Parameter(Position = 0)][string]$WavDir, # folder with the WAV files; asked for if missing
    [string]$EncodeArgs = '-V 4',             # encoder settings for both programs
    [int]$Rounds = 1,                         # runs per file and program; the median counts
    [switch]$Sequential,                      # also ctLame --sequential (one thread)
    [int]$Threads = 0,                        # ctLame --threads n; 0 = all logical processors
    [switch]$Recurse,                         # also the WAV files in subfolders
    [switch]$KeepMp3,                         # keep the MP3 files in work\
    [switch]$KeepPowerPlan,                   # do not switch the power plan
    [switch]$Warm                             # read the WAV files where they are (from the file cache)
)

$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
# bin\ next to the script (benchmark package) or one level up (ctLame repository, benchmark\)
$bin     = if (Test-Path (Join-Path $root 'bin')) { Join-Path $root 'bin' } else { Join-Path (Split-Path $root) 'bin' }
$work    = Join-Path $root 'work'
$results = Join-Path $root 'results'

$ctLame = Join-Path $bin 'ctLame.exe'
$lame   = Join-Path $bin 'lame.exe'

# both programs must be there (the ctLame repository does not include ctLame.exe yet)
foreach ($exe in $lame, $ctLame) {
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        Write-Host "missing: $exe" -ForegroundColor Red
        if ($exe -eq $ctLame) { Write-Host 'The benchmark compares ctLame.exe with lame.exe; ctLame.exe is not included in this folder (yet), see README.md.' -ForegroundColor Red }
        exit 1
    }
}
New-Item -ItemType Directory -Force -Path $work, $results | Out-Null

if (-not $WavDir) { $WavDir = Read-Host 'Folder with WAV files' }
$WavDir = $WavDir.Trim('"')
if (-not (Test-Path -LiteralPath $WavDir -PathType Container)) { throw "folder not found: $WavDir" }
$wavs = @(Get-ChildItem -LiteralPath $WavDir -Filter *.wav -File -Recurse:$Recurse | Sort-Object FullName)
if ($wavs.Count -eq 0) { throw "no WAV files in $WavDir" }

$stamp   = Get-Date -Format 'yyyyMMdd-HHmm'
$report  = Join-Path $results ("bench-{0}-{1}.txt" -f $env:COMPUTERNAME, $stamp)
$csv     = Join-Path $results ("bench-{0}-{1}.csv" -f $env:COMPUTERNAME, $stamp)
$argList = @($EncodeArgs -split ' ' | Where-Object { $_ })

$candidates = @(
    [pscustomobject]@{ Name = 'lame.exe'; Exe = $lame; Extra = @() }
    [pscustomobject]@{ Name = 'ctLame'; Exe = $ctLame; Extra = @(if ($Threads -gt 0) { '--threads', "$Threads" }) }
)
if ($Sequential) { $candidates += [pscustomobject]@{ Name = 'ctLame --sequential'; Exe = $ctLame; Extra = @('--sequential') } }

function Write-Report([string]$text) {
    Write-Host $text
    Add-Content -LiteralPath $report -Value $text -Encoding UTF8
}

# run a program; returns the wall time in seconds, or -1 with the error text in $script:lastError
function Invoke-Timed([string]$exe, [string[]]$arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $stderr = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    $sw.Stop()
    $script:lastOutput = $stdout.Result + $stderr
    if ($p.ExitCode -ne 0) {
        $script:lastError = "exit code $($p.ExitCode): $($stderr.Trim())"
        return -1
    }
    return $sw.Elapsed.TotalSeconds
}

# a fresh copy of the file that is not in the file cache: robocopy /J copies unbuffered
function New-ColdCopy([string]$path) {
    $dir = Join-Path $work 'cold'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $name = Split-Path -Leaf $path
    $dst = Join-Path $dir $name
    if (Test-Path -LiteralPath $dst) { [IO.File]::Delete($dst) }
    $src = Split-Path -Parent $path
    if ($src.EndsWith('\')) { $src += '.' } # "D:\" would end in an escaped quote
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $env:SystemRoot 'System32\robocopy.exe'
    $psi.Arguments = '"{0}" "{1}" "{2}" /J /R:0 /W:0 /NJH /NJS /NFL /NDL /NP' -f $src, $dir, $name
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $text = $p.StandardOutput.ReadToEndAsync()
    $null = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    if ($p.ExitCode -ge 8 -or -not (Test-Path -LiteralPath $dst)) { throw "cold copy of $path failed (robocopy exit code $($p.ExitCode)): $($text.Result.Trim())" }
    return $dst
}

function Median([double[]]$values) {
    $s = @($values | Sort-Object)
    if ($s.Count -eq 0) { return [double]::NaN }
    if ($s.Count % 2 -eq 1) { return $s[($s.Count - 1) / 2] }
    return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2
}

# audio length of a WAV file: data chunk size / byte rate (the rest of the file if the size is unset)
function Get-WavSeconds([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try {
        $br = New-Object IO.BinaryReader($fs)
        if ($fs.Length -lt 12) { return 0 }
        $fs.Position = 12
        $byteRate = 0
        while ($fs.Position + 8 -le $fs.Length) {
            $id = [Text.Encoding]::ASCII.GetString($br.ReadBytes(4))
            $size = [int64]$br.ReadUInt32()
            if ($id -eq 'fmt ') {
                $start = $fs.Position
                $fs.Position = $start + 8
                $byteRate = $br.ReadUInt32()
                $fs.Position = $start + $size + ($size % 2)
            } elseif ($id -eq 'data') {
                $rest = $fs.Length - $fs.Position
                if ($size -eq 0 -or $size -eq 0xFFFFFFFF -or $size -gt $rest) { $size = $rest }
                if ($byteRate -eq 0) { return 0 }
                return $size / $byteRate
            } else {
                $fs.Position += $size + ($size % 2)
            }
        }
        return 0
    } finally { $fs.Dispose() }
}

function Format-Seconds([double]$s) { if ([double]::IsNaN($s)) { '-' } else { $s.ToString('0.00') } }

# powercfg without PowerShell's error handling of native programs (Windows PowerShell 5.1 turns
# any stderr text into a terminating error under 'Stop'); returns exit code and text
function Invoke-PowerCfg([string[]]$arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $env:SystemRoot 'System32\powercfg.exe'
    $psi.Arguments = $arguments -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $p = [Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $stderr = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    [pscustomobject]@{ ExitCode = $p.ExitCode; Text = ($stdout.Result + $stderr).Trim() }
}

$guidPattern = '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}'
function Get-ActivePlan {
    $r = Invoke-PowerCfg @('/getactivescheme')
    $guid = if ($r.Text -match $guidPattern) { $matches[0].ToLowerInvariant() } else { $null }
    [pscustomobject]@{ Guid = $guid; Text = $r.Text }
}

# power plan: High Performance during the run, the previous plan afterwards
$highPerformance = '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c'
$ultimate = 'e9a42b02-d5df-448d-aa00-03f14749eb61'
$originalPlan = $null
$createdPlan = $null
$planNote = ''
if (-not $KeepPowerPlan) {
    try {
        $active = Get-ActivePlan
        if ($active.Guid -and $active.Guid -ne $highPerformance -and $active.Guid -ne $ultimate) {
            $listed = @([regex]::Matches((Invoke-PowerCfg @('/list')).Text, $guidPattern) | ForEach-Object { $_.Value.ToLowerInvariant() })
            $target = if ($listed -contains $highPerformance) { $highPerformance } elseif ($listed -contains $ultimate) { $ultimate } else { $null }
            if (-not $target) {
                # hidden plan (e.g. notebooks with modern standby): a temporary copy
                $copy = Invoke-PowerCfg @('/duplicatescheme', $highPerformance)
                if ($copy.ExitCode -eq 0 -and $copy.Text -match $guidPattern) { $target = $createdPlan = $matches[0].ToLowerInvariant() }
                else { $planNote = "no High Performance plan ($($copy.Text))" }
            }
            if ($target) {
                $set = Invoke-PowerCfg @('/setactive', $target)
                if ((Get-ActivePlan).Guid -eq $target) { $originalPlan = $active.Guid }
                else { $planNote = "switch to High Performance refused (exit code $($set.ExitCode)$(if ($set.Text) { ": $($set.Text)" })); run as administrator or choose it by hand" }
            }
        }
    } catch {
        $planNote = "power plan unchanged: $($_.Exception.Message)"
    }
    if ($planNote) { Write-Host $planNote -ForegroundColor Yellow }
}

function Restore-PowerPlan {
    if ($originalPlan) {
        $null = Invoke-PowerCfg @('/setactive', $originalPlan)
        Write-Host "power plan restored ($originalPlan)" -ForegroundColor Cyan
    }
    if ($createdPlan) { $null = Invoke-PowerCfg @('/delete', $createdPlan) }
}

$exitCode = 0
try {
    # system
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    # a warm-up encode of the first file (not measured) that also prints ctLame's feature line
    $warmup = Join-Path $work 'warmup.mp3'
    $null = Invoke-Timed $ctLame (@('--features', '--silent') + $argList + @($wavs[0].FullName, $warmup))
    Remove-Item -LiteralPath $warmup -ErrorAction SilentlyContinue
    $featureLine = ($script:lastOutput -split "`n" | Where-Object { $_ -match 'ctLame features' } | Select-Object -First 1)
    $plan = (Get-ActivePlan).Text
    Write-Report "ctLame against lame.exe  $(Get-Date -Format 'yyyy-MM-dd HH:mm')  computer $env:COMPUTERNAME"
    Write-Report "CPU: $($cpu.Name.Trim()), $($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) logical processors"
    if ($featureLine) { Write-Report $featureLine.Trim() }
    if ($plan) { Write-Report "power plan: $plan$(if ($originalPlan) { '  (switched for the run)' })" }
    if ($planNote) { Write-Report "power plan note: $planNote" }
    Write-Report "folder: $WavDir  ($($wavs.Count) WAV files)"
    Write-Report "settings: $EncodeArgs, rounds: $Rounds$(if ($Threads -gt 0) { ", ctLame --threads $Threads" }), $(if ($Warm) { 'warm (files from the cache)' } else { 'cold (fresh unbuffered copy before every run)' })"
    Write-Report ''

    $rows = @()
    $index = 0
    foreach ($wav in $wavs) {
        $index++
        $seconds = Get-WavSeconds $wav.FullName
        $times = @{}
        $hashes = @{}
        $errors = @{}
        foreach ($c in $candidates) { $times[$c.Name] = @() }
        for ($r = 0; $r -lt $Rounds; $r++) {
            $k = 0
            foreach ($c in $candidates) {
                $k++
                if ($errors.ContainsKey($c.Name)) { continue }
                $out = Join-Path $work ("{0:D4}-{1}.mp3" -f $index, $k)
                $in = if ($Warm) { $wav.FullName } else { New-ColdCopy $wav.FullName }
                $t = Invoke-Timed $c.Exe (@('--silent') + $argList + $c.Extra + @($in, $out))
                if (-not $Warm) { [IO.File]::Delete($in) }
                if ($t -lt 0) {
                    $errors[$c.Name] = $script:lastError
                    continue
                }
                $times[$c.Name] += $t
                if ($r -eq 0) { $hashes[$c.Name] = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash }
                if (-not $KeepMp3 -and $r -eq $Rounds - 1) { Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue }
            }
        }

        $row = [ordered]@{ File = $wav.Name; AudioSeconds = [math]::Round($seconds, 2) }
        foreach ($c in $candidates) { $row[$c.Name] = if ($times[$c.Name].Count -gt 0) { Median $times[$c.Name] } else { [double]::NaN } }
        $lameTime = $row['lame.exe']
        $row['Factor'] = if ($row['ctLame'] -gt 0 -and -not [double]::IsNaN($lameTime)) { $lameTime / $row['ctLame'] } else { [double]::NaN }
        $same = @()
        foreach ($c in $candidates | Where-Object { $_.Name -ne 'lame.exe' }) {
            if ($hashes.ContainsKey('lame.exe') -and $hashes.ContainsKey($c.Name)) { $same += ($hashes[$c.Name] -eq $hashes['lame.exe']) }
        }
        $row['Identical'] = if ($errors.Count -gt 0) { 'error' } elseif ($same.Count -gt 0 -and -not ($same -contains $false)) { 'yes' } else { 'NO' }
        $row['Error'] = ($errors.GetEnumerator() | ForEach-Object { "$($_.Key): $($_.Value)" }) -join '; '
        $rows += [pscustomobject]$row

        $line = "[{0}/{1}] {2}  {3} s audio" -f $index, $wavs.Count, $wav.Name, (Format-Seconds $seconds)
        foreach ($c in $candidates) { $line += "  {0} {1} s" -f $c.Name, (Format-Seconds $row[$c.Name]) }
        $line += "  factor {0}  identical {1}" -f (Format-Seconds $row['Factor']), $row['Identical']
        Write-Report $line
        if ($row['Error']) { Write-Report "    $($row['Error'])" }
    }

    # totals over the files every program encoded
    $ok = @($rows | Where-Object { $_.Identical -ne 'error' })
    $audio = ($ok | Measure-Object AudioSeconds -Sum).Sum
    Write-Report ''
    Write-Report ("Totals over {0} files, {1} s audio (sum of the medians)" -f $ok.Count, (Format-Seconds $audio))
    Write-Report ('{0,-22} {1,10} {2,13} {3,12}' -f 'program', 'time s', 'x real time', 'vs lame.exe')
    $lameTotal = ($ok | Measure-Object 'lame.exe' -Sum).Sum
    foreach ($c in $candidates) {
        $total = ($ok | Measure-Object $c.Name -Sum).Sum
        $realTime = if ($total -gt 0) { $audio / $total } else { [double]::NaN }
        $vsLame = if ($total -gt 0) { $lameTotal / $total } else { [double]::NaN }
        Write-Report ('{0,-22} {1,10} {2,13} {3,12}' -f $c.Name, $total.ToString('0.00'), $realTime.ToString('0'), $vsLame.ToString('0.00'))
    }
    $different = @($rows | Where-Object { $_.Identical -eq 'NO' })
    $failed = @($rows | Where-Object { $_.Identical -eq 'error' })
    Write-Report ''
    Write-Report ("bit identical to lame.exe: {0} of {1} files{2}{3}" -f ($ok.Count - $different.Count), $ok.Count,
        $(if ($different.Count -gt 0) { ", DIFFERENT: " + (($different | ForEach-Object File) -join ', ') }),
        $(if ($failed.Count -gt 0) { "; $($failed.Count) files not encoded (see above)" }))

    $rows | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath $csv
    Write-Report ''
    Write-Report "report: $report"
    Write-Report "numbers per file: $csv"
    if ($different.Count -gt 0 -or $failed.Count -gt 0) { $exitCode = 2 }
} finally {
    Restore-PowerPlan
}
exit $exitCode
