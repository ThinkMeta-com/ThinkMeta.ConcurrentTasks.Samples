<#
.SYNOPSIS
    ctDeflate against libdeflate 1.26 gzip.exe: speed and bit identity.

.DESCRIPTION
    Windows PowerShell 5.1 or later. Compresses every input file at every level with gzip.exe (the
    official libdeflate 1.26 build, the reference) and with ctDeflate, one program at a time; per file
    and level the programs run one after the other, round by round, so a slow phase of the computer
    hits both alike. Measured is the wall time of the whole process (start, reading, compressing,
    writing).

    Cold by default: before every measured run the input is copied to work\cold\ with unbuffered
    writes (robocopy /J), so the copy is not in the file cache and the program reads it from the disk,
    as a file it has not seen before. The copy is not timed and is deleted after the run; both
    programs are treated alike. -Warm instead copies each input once to work\ and lets both programs
    read it from the file cache (the method before 2026-10-10).

    Bit identity: ctDeflate's .gz must be byte for byte the same as gzip.exe's (first round).

    The report lists per file and level the median times, the speeds and the factor, and the totals
    per level (sum of the medians). It goes to results\bench-<computer>-<time>.txt, the numbers to a
    .csv next to it.

    Power plan: for the run the script switches to High Performance (Ultimate Performance if that is
    the only one; where the plan is hidden, a temporary copy) and restores the previous plan at the
    end. If Windows refuses the switch (e.g. Windows Server without administrator rights, or a group
    policy), the report says so and the run goes on with the current plan. -KeepPowerPlan leaves the
    plan alone. Whether the previous plan came back is checked and written to the report.

    Exit code (the same for the benchmark scripts of all ThinkMeta.ConcurrentTasks samples): 0 if every
    output was identical, 2 if at least one output differed or a run failed, 1 for other errors: a
    missing program, wrong parameters, ctDeflate refusing the CPU (its exit code 3: the CPU lacks an
    instruction set ctDeflate needs), an abort. These are checked before the power plan is touched.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Compare-Gzip.ps1 D:\Daten\big.bin D:\Logs -Levels 1,6,9 -Rounds 3
#>
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Inputs, # files or folders; else Bench-Files.txt, else asked for
    [string]$Levels = '1,6,9',    # compression levels, comma-separated (a string: -File passes no arrays)
    [int]$Rounds = 3,             # runs per file, level and program; the median counts
    [int]$Threads = 0,            # ctDeflate --threads n; 0 = all logical processors
    [string]$CtArgs = '',         # further ctDeflate arguments, e.g. '--in-memory'
    [switch]$Recurse,             # also the files in subfolders of a folder
    [switch]$Warm,                # read the inputs from the file cache instead of a fresh unbuffered copy per run
    [string]$GzipExe = '',        # default: bin\gzip.exe next to the script or one level up, else TestResults\DeflateReference
    [string]$CtDeflateExe = '',   # default: bin\ctDeflate.exe next to the script or one level up, else API\build\Release\bin
    [switch]$KeepPowerPlan        # do not switch the power plan
)

$ErrorActionPreference = 'Stop'
# Paths that depend on the script folder are resolved here, not in param() (Windows PowerShell 5.1):
# bin\ next to the script (benchmark package), one level up (ctDeflate folder, benchmark\), else
# the development repository.
$root    = $PSScriptRoot
$work    = Join-Path $root 'work'
$results = Join-Path $root 'results'
function Find-Program([string]$name, [string]$devPath) {
    foreach ($dir in (Join-Path $root 'bin'), (Join-Path (Split-Path $root) 'bin')) {
        $path = Join-Path $dir $name
        if (Test-Path -LiteralPath $path) { return $path }
    }
    return Join-Path (Split-Path (Split-Path (Split-Path $root))) $devPath
}
if (-not $GzipExe) { $GzipExe = Find-Program 'gzip.exe' 'TestResults\DeflateReference\gzip.exe' }
if (-not $CtDeflateExe) { $CtDeflateExe = Find-Program 'ctDeflate.exe' 'API\build\Release\bin\ctDeflate.exe' }
foreach ($exe in $GzipExe, $CtDeflateExe) {
    if (-not (Test-Path -LiteralPath $exe)) { throw "program not found: $exe (expected in bin\ next to the script or one level up; or give -GzipExe / -CtDeflateExe)" }
}
New-Item -ItemType Directory -Force -Path $work, $results | Out-Null

if (-not $Inputs -or $Inputs.Count -eq 0) {
    $list = Join-Path $root 'Bench-Files.txt'
    if (Test-Path -LiteralPath $list) {
        # Relative entries are relative to the script folder (a package with its own corpus folder).
        $Inputs = @(Get-Content -LiteralPath $list | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
                $entry = $_.Trim()
                if ([IO.Path]::IsPathRooted($entry)) { $entry } else { Join-Path $root $entry }
            })
    } else {
        $Inputs = @(Read-Host 'File or folder to compress')
    }
}
$files = @()
foreach ($i in $Inputs) {
    $i = $i.Trim('"')
    if (Test-Path -LiteralPath $i -PathType Container) { $files += @(Get-ChildItem -LiteralPath $i -File -Recurse:$Recurse | Sort-Object FullName) }
    elseif (Test-Path -LiteralPath $i -PathType Leaf) { $files += Get-Item -LiteralPath $i }
    else { Write-Host "not found, skipped: $i" -ForegroundColor Yellow }
}
$files = @($files | Where-Object { $_.Length -gt 0 })
if ($files.Count -eq 0) { throw 'no input files' }

$stamp  = Get-Date -Format 'yyyyMMdd-HHmm'
$report = Join-Path $results ("bench-{0}-{1}.txt" -f $env:COMPUTERNAME, $stamp)
$csv    = Join-Path $results ("bench-{0}-{1}.csv" -f $env:COMPUTERNAME, $stamp)
$ctExtra = @($CtArgs -split ' ' | Where-Object { $_ })
$levelList = @($Levels -split '[,; ]' | Where-Object { $_ } | ForEach-Object { [int]$_ })
foreach ($level in $levelList) {
    # gzip.exe has no level 0, ctDeflate no levels 10 to 12
    if ($level -lt 1 -or $level -gt 9) { throw "level ${level}: the levels both programs have are 1 to 9" }
}
if ($Threads -gt 0) { $ctExtra += @('--threads', "$Threads") }

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

# A fresh copy of path in work\cold\, written unbuffered (robocopy /J): it is not in the file cache.
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

function Format-Number([double]$v, [string]$format) { if ([double]::IsNaN($v)) { '-' } else { $v.ToString($format) } }

function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }

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

# ctDeflate checks the CPU at start (exit code 3 with a message if an instruction set is missing);
# a call without arguments only prints its usage (exit code 1) on a CPU it accepts
$null = Invoke-Timed $CtDeflateExe @()
if ($script:lastError -match '^exit code 3:') { throw "ctDeflate cannot run on this CPU: $($script:lastError.Substring(13))" }

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
$exitCode = 1
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
        $text = if ((Get-ActivePlan).Guid -eq $originalPlan) { "power plan restored ($originalPlan)" } else { "power plan NOT restored, still the High Performance plan; previous plan: $originalPlan (powercfg /setactive $originalPlan)" }
        Write-Host $text -ForegroundColor Cyan
        if (Test-Path -LiteralPath $report) { Add-Content -LiteralPath $report -Value $text -Encoding UTF8 }
    }
    if ($createdPlan) { $null = Invoke-PowerCfg @('/delete', $createdPlan) }
}

try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $plan = (Get-ActivePlan).Text
    Write-Report "ctDeflate against libdeflate 1.26 gzip.exe  $(Get-Date -Format 'yyyy-MM-dd HH:mm')  computer $env:COMPUTERNAME"
    Write-Report "CPU: $($cpu.Name.Trim()), $($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) logical processors"
    if ($plan) { Write-Report "power plan: $plan$(if ($originalPlan) { '  (switched for the run)' })" }
    if ($planNote) { Write-Report "power plan note: $planNote" }
    Write-Report "files: $($files.Count), levels: $($levelList -join ','), rounds: $Rounds$(if ($ctExtra.Count -gt 0) { ", ctDeflate $($ctExtra -join ' ')" }), $(if ($Warm) { 'warm (inputs in the file cache)' } else { 'cold (a fresh unbuffered copy of the input before every run)' })"
    Write-Report ''
    Write-Report ('{0,-34} {1,10} {2,3} {3,9} {4,9} {5,9} {6,9} {7,7} {8,5}' -f 'file', 'MB', 'lvl', 'gzip s', 'ct s', 'gzip MB/s', 'ct MB/s', 'factor', 'same')

    $rows = @()
    $index = 0
    foreach ($file in $files) {
        $index++
        $copy = Join-Path $work ("{0:D3}-{1}" -f $index, $file.Name)
        if ($Warm) { Copy-Item -LiteralPath $file.FullName -Destination $copy -Force }
        $mb = $file.Length / 1e6
        foreach ($level in $levelList) {
            # cold: the outputs go next to the copies in work\cold\, named after the input as gzip.exe does
            $refOut = if ($Warm) { "$copy.gz" } else { Join-Path (Join-Path $work 'cold') ($file.Name + '.gz') }
            $ctOut = if ($Warm) { "$copy.ct.gz" } else { Join-Path $work ("{0:D3}.ct.gz" -f $index) }
            $times = @{ gzip = @(); ct = @() }
            $err = ''
            $same = 'NO'
            for ($r = 0; $r -lt $Rounds -and -not $err; $r++) {
                $in = if ($Warm) { $copy } else { New-ColdCopy $file.FullName }
                $t = Invoke-Timed $GzipExe @("-$level", '-k', '-f', $in)
                if (-not $Warm) { [IO.File]::Delete($in) }
                if ($t -lt 0) { $err = "gzip.exe $script:lastError"; break }
                $times.gzip += $t
                $in = if ($Warm) { $copy } else { New-ColdCopy $file.FullName }
                $t = Invoke-Timed $CtDeflateExe (@("-$level") + $ctExtra + @('-o', $ctOut, $in))
                if (-not $Warm) { [IO.File]::Delete($in) }
                if ($t -lt 0) { $err = "ctDeflate $script:lastError"; break }
                $times.ct += $t
                if ($r -eq 0) { $same = if ((Get-Sha256 $refOut) -eq (Get-Sha256 $ctOut)) { 'yes' } else { 'NO' } }
            }
            Remove-Item -LiteralPath $refOut, $ctOut -ErrorAction SilentlyContinue
            $g = Median $times.gzip
            $c = Median $times.ct
            $row = [pscustomobject][ordered]@{
                File = $file.Name; MB = [math]::Round($mb, 2); Level = $level
                GzipSeconds = $g; CtSeconds = $c
                GzipMBs = $(if ($g -gt 0) { $mb / $g } else { [double]::NaN }); CtMBs = $(if ($c -gt 0) { $mb / $c } else { [double]::NaN })
                Factor = $(if ($c -gt 0 -and $g -gt 0) { $g / $c } else { [double]::NaN })
                Identical = $(if ($err) { 'error' } else { $same }); Error = $err
            }
            $rows += $row
            $name = if ($file.Name.Length -gt 34) { $file.Name.Substring(0, 31) + '...' } else { $file.Name }
            Write-Report ('{0,-34} {1,10} {2,3} {3,9} {4,9} {5,9} {6,9} {7,7} {8,5}' -f $name, $mb.ToString('0.0'), $level, (Format-Number $g '0.000'),
                (Format-Number $c '0.000'), (Format-Number $row.GzipMBs '0'), (Format-Number $row.CtMBs '0'), (Format-Number $row.Factor '0.00'), $row.Identical)
            if ($err) { Write-Report "    $err" }
        }
        Remove-Item -LiteralPath $copy -ErrorAction SilentlyContinue
    }

    # totals per level over the files both programs compressed
    Write-Report ''
    Write-Report 'Totals per level (sum of the medians)'
    Write-Report ('{0,3} {1,10} {2,9} {3,9} {4,9} {5,9} {6,7}' -f 'lvl', 'MB', 'gzip s', 'ct s', 'gzip MB/s', 'ct MB/s', 'factor')
    foreach ($level in $levelList) {
        $ok = @($rows | Where-Object { $_.Level -eq $level -and $_.Identical -ne 'error' })
        if ($ok.Count -eq 0) { continue }
        $mb = ($ok | Measure-Object MB -Sum).Sum
        $g = ($ok | Measure-Object GzipSeconds -Sum).Sum
        $c = ($ok | Measure-Object CtSeconds -Sum).Sum
        Write-Report ('{0,3} {1,10} {2,9} {3,9} {4,9} {5,9} {6,7}' -f $level, $mb.ToString('0.0'), $g.ToString('0.000'), $c.ToString('0.000'),
            ($mb / $g).ToString('0'), ($mb / $c).ToString('0'), ($g / $c).ToString('0.00'))
    }
    $different = @($rows | Where-Object { $_.Identical -eq 'NO' })
    $failed = @($rows | Where-Object { $_.Identical -eq 'error' })
    Write-Report ''
    Write-Report ("bit identical to gzip.exe: {0} of {1} runs{2}{3}" -f ($rows.Count - $different.Count - $failed.Count), ($rows.Count - $failed.Count),
        $(if ($different.Count -gt 0) { ", DIFFERENT: " + (($different | ForEach-Object { "$($_.File) -$($_.Level)" }) -join ', ') }),
        $(if ($failed.Count -gt 0) { "; $($failed.Count) runs failed (see above)" }))

    $rows | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath $csv
    Write-Report ''
    Write-Report "report: $report"
    Write-Report "numbers: $csv"
    $exitCode = if ($different.Count -gt 0 -or $failed.Count -gt 0) { 2 } else { 0 }
} finally {
    # input copies and outputs left behind by an error or Ctrl+C
    Get-ChildItem -LiteralPath $work -File -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Restore-PowerPlan
}
exit $exitCode
