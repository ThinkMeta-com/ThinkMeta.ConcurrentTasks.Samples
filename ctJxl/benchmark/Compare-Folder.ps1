<#
.SYNOPSIS
    ctJxl against cjxl (libjxl 0.13.0) on a folder of images: speed and bit identity.

.DESCRIPTION
    Encodes every image in the folder (PPM/PGM/PNM/PAM/PFM, PNG/APNG, GIF, JPEG) with cjxl (the reference, libjxl's
    own thread pool) and ctJxl, once per case (a set of cjxl options, default "-q 90" and "-d 0"), image after image,
    round by round, the two programs alternating. Measures the wall time of the whole process (start, reading,
    encoding, writing). Time per image and program: with 3 or more rounds the best and the worst run are dropped and
    the rest averaged (5 rounds: mean of the middle 3), with fewer rounds the median.

    File cache: the wall time includes reading the image, so every program reads it cold, as a photo usually is:
    before each timed run the script writes a fresh copy of the image with unbuffered I/O (robocopy /J) into work\cold,
    which is then not in the file cache. The copy is not timed. The programs read from the disk the benchmark folder is
    on. -Warm measures with the images in the file cache instead.

    Bit identity: libjxl writes different bytes depending on the SIMD target it runs on. ctJxl is built for AVX2
    (/arch:AVX2) and reproduces libjxl's AVX2 target, so the script runs cjxl on that target (cjxl-oracle --target
    avx2) and compares the ctJxl output byte for byte with it. ctJxl needs a CPU with AVX2 (and FMA, BMI1/2); on
    other CPUs it stops at once with exit code 3, and the script ends with an error (exit code 1).

    Power plan: the script switches to High Performance for reproducible timing (Ultimate Performance if that is the
    only one; where the plan is hidden, a temporary copy) and restores the previous plan at the end, also after an
    error or Ctrl+C. If Windows refuses the switch (e.g. Windows Server without administrator rights, or a group
    policy), the report says so and the run goes on with the current plan. -KeepPowerPlan leaves the plan alone.

    Windows PowerShell 5.1 or later.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 D:\Images
    powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 D:\Images -Cases "-q 90;-d 0 -e 9;-m 1 -q 80" -Rounds 3
#>
param(
    [Parameter(Position = 0)][string]$ImageDir = '',
    [string[]]$Cases = @('-q 90', '-d 0'), # cjxl options per case, the same for both programs; also one string, cases separated by ';'

    [int]$Rounds = 5,              # runs per image, case and program
    [int]$Threads = 0,             # --num_threads n for both programs; 0 = their default (all logical processors)
    [string]$Files = 'All',        # All, Quick (every 5th image) or n (the first n images)
    [switch]$Recurse,              # include subfolders
    [switch]$KeepJxl,              # keep the JXL files in work\
    [switch]$Warm,                 # read the images from the file cache instead of cold copies
    [switch]$KeepPowerPlan         # do not switch the power plan
)

$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
# bin\ next to the script (benchmark package) or bin\win-x64\ one level up
$bin     = if (Test-Path (Join-Path $root 'bin')) { Join-Path $root 'bin' } else { Join-Path (Split-Path $root) 'bin\win-x64' }
$work    = Join-Path $root 'work'
$results = Join-Path $root 'results'
New-Item -ItemType Directory -Force -Path $work, $results | Out-Null

$ctJxl = Join-Path $bin 'ctJxl.exe'
$cjxl  = Join-Path $bin 'cjxl-oracle.exe'
if (-not (Test-Path $cjxl)) { $cjxl = Join-Path $bin 'cjxl.exe' } # its name in the repository's bin\win-x64\
foreach ($exe in $ctJxl, $cjxl) { if (-not (Test-Path $exe)) { throw "not found: $exe" } }
if ($Rounds -lt 1) { throw '-Rounds must be 1 or more' }

if (-not $ImageDir) { $ImageDir = Read-Host 'Folder with images' }
$ImageDir = $ImageDir.Trim('"')
if (-not (Test-Path -LiteralPath $ImageDir -PathType Container)) { throw "Folder not found: $ImageDir" }

$allImages = @(Get-ChildItem -LiteralPath $ImageDir -File -Recurse:$Recurse | Where-Object { $_.Extension -match '^\.(ppm|pgm|pnm|pam|pfm|png|apng|gif|jpg|jpeg)$' } | Sort-Object FullName)
if ($allImages.Count -eq 0) { throw "No images (PPM, PGM, PNM, PAM, PFM, PNG, APNG, GIF, JPEG) found in $ImageDir" }
if ($Files -eq 'Quick') {
    $images = for ($k = 0; $k -lt $allImages.Count; $k += 5) { $allImages[$k] }
} elseif ($Files -match '^\d+$') {
    $images = @($allImages | Select-Object -First ([int]$Files))
} else {
    $images = $allImages
}
# powershell -File passes no arrays: cases may also come as one string separated by ';'
$Cases = @($Cases | ForEach-Object { $_ -split ';' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Cases.Count -eq 0) { throw '-Cases is empty' }

$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$report = Join-Path $results ("bench-{0}-{1}.txt" -f $env:COMPUTERNAME, $stamp)
$csv    = Join-Path $results ("bench-{0}-{1}.csv" -f $env:COMPUTERNAME, $stamp)

function Write-Report([string]$text, [string]$color = '') {
    if ($color) { Write-Host $text -ForegroundColor $color } else { Write-Host $text }
    Add-Content -LiteralPath $report -Value $text -Encoding UTF8
}

# run a program; wall time in seconds, or -1 with the error text in $script:lastError
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
    $null = $stdout.Result
    if ($p.ExitCode -ne 0) {
        $script:lastError = "exit code $($p.ExitCode): $($stderr.Trim())"
        return -1.0
    }
    return $sw.Elapsed.TotalSeconds
}

# deletes a cold copy, also one that came out read-only
function Remove-ColdCopy([string]$path) {
    if (Test-Path -LiteralPath $path) {
        [IO.File]::SetAttributes($path, [IO.FileAttributes]::Normal)
        [IO.File]::Delete($path)
    }
}

# a fresh copy of the input, written with unbuffered I/O (robocopy /J) so it is not in the file cache
function New-ColdCopy([string]$path) {
    $dir = Join-Path $work 'cold'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $name = Split-Path -Leaf $path
    $dst = Join-Path $dir $name
    Remove-ColdCopy $dst
    $src = Split-Path -Parent $path
    if ($src.EndsWith('\')) { $src += '.' } # "D:\" would end in an escaped quote
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $env:SystemRoot 'System32\robocopy.exe'
    $psi.Arguments = '"{0}" "{1}" "{2}" /J /DCOPY:T /A-:R /R:0 /W:0 /NJH /NJS /NFL /NDL /NP' -f $src, $dir, $name
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $text = $p.StandardOutput.ReadToEndAsync()
    $null = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    # robocopy hands the attributes of the source folder to work\cold and those of a read-only image to the copy: both
    # back to normal, so the copy can be deleted after the run
    [IO.File]::SetAttributes($dir, [IO.FileAttributes]::Directory)
    if (Test-Path -LiteralPath $dst) { [IO.File]::SetAttributes($dst, [IO.FileAttributes]::Normal) }
    if ($p.ExitCode -ge 8 -or -not (Test-Path -LiteralPath $dst)) { throw "cold copy of $path failed (robocopy exit code $($p.ExitCode)): $($text.Result.Trim())" }
    return $dst
}

# SHA-256 of a file with .NET (Get-FileHash is missing when the module path of PowerShell 7 leaks in)
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [IO.File]::OpenRead($path)
    try { return [BitConverter]::ToString($sha.ComputeHash($fs)).Replace('-', '') }
    finally { $fs.Dispose(); $sha.Dispose() }
}

# 3 or more runs: drop the best and the worst, mean of the rest; fewer: the median
function Get-RunTime([double[]]$values) {
    $s = @($values | Sort-Object)
    if ($s.Count -eq 0) { return [double]::NaN }
    if ($s.Count -ge 3) { return ($s[1..($s.Count - 2)] | Measure-Object -Average).Average }
    if ($s.Count -eq 1) { return $s[0] }
    return ($s[0] + $s[1]) / 2
}

# megapixels of the first frame from the file header (PNM/PAM/PFM, PNG, GIF); 0 if unknown (e.g. JPEG)
function Get-Megapixels([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try {
        $buffer = New-Object byte[] 1024
        $n = $fs.Read($buffer, 0, $buffer.Length)
        if ($n -ge 24 -and $buffer[0] -eq 0x89 -and $buffer[1] -eq 0x50 -and $buffer[2] -eq 0x4E -and $buffer[3] -eq 0x47) {
            $w = ([int]$buffer[16] -shl 24) -bor ([int]$buffer[17] -shl 16) -bor ([int]$buffer[18] -shl 8) -bor $buffer[19]
            $h = ([int]$buffer[20] -shl 24) -bor ([int]$buffer[21] -shl 16) -bor ([int]$buffer[22] -shl 8) -bor $buffer[23]
            return [double]$w * $h / 1e6
        }
        if ($n -ge 10 -and $buffer[0] -eq 0x47 -and $buffer[1] -eq 0x49 -and $buffer[2] -eq 0x46) {
            return [double]($buffer[6] + 256 * $buffer[7]) * ($buffer[8] + 256 * $buffer[9]) / 1e6
        }
        $text = [regex]::Replace([Text.Encoding]::ASCII.GetString($buffer, 0, $n), '#[^\n]*', ' ')
        if ($text.StartsWith('P7')) {
            $w = [regex]::Match($text, 'WIDTH\s+(\d+)'); $h = [regex]::Match($text, 'HEIGHT\s+(\d+)')
            if ($w.Success -and $h.Success) { return [double]$w.Groups[1].Value * [double]$h.Groups[1].Value / 1e6 }
            return 0.0
        }
        $m = [regex]::Match($text, '^P[2356Ff]\s+(\d+)\s+(\d+)')
        if ($m.Success) { return [double]$m.Groups[1].Value * [double]$m.Groups[2].Value / 1e6 }
        return 0.0
    } finally { $fs.Dispose() }
}

function Format-Seconds([double]$s) { if ([double]::IsNaN($s)) { '-' } else { $s.ToString('0.000') } }

# powercfg without PowerShell's error handling of native programs (Windows PowerShell 5.1 turns any stderr text into a
# terminating error under 'Stop'); returns exit code and text
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

# ctJxl is an AVX2 program: it ends with exit code 3 on a CPU without AVX2 (Shared CpuCheck); any other failure of
# the probe shows up again in the run
$Simd = 'avx2'
if ((Invoke-Timed $ctJxl @('--version')) -lt 0 -and $script:lastError -match '^exit code 3:') {
    throw "ctJxl needs a CPU with AVX2: $($script:lastError)"
}

$threadArgs = @(if ($Threads -gt 0) { '--num_threads', "$Threads" })
$programs = @(
    [pscustomobject]@{ Name = 'cjxl'; Exe = $cjxl; Args = { param($in, $out, $opts) @('--target', $Simd, $in, $out) + $opts + $threadArgs + @('--quiet') } }
    [pscustomobject]@{ Name = 'ctJxl'; Exe = $ctJxl; Args = { param($in, $out, $opts) @($in, $out) + $opts + $threadArgs + @('--quiet') } }
)

# power plan: High Performance during the run, the previous plan afterwards
$highPerformance = '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c'
$ultimate = 'e9a42b02-d5df-448d-aa00-03f14749eb61'
$origPlanGuid = $null
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
                if ((Get-ActivePlan).Guid -eq $target) { $origPlanGuid = $active.Guid }
                else { $planNote = "switch to High Performance refused (exit code $($set.ExitCode)$(if ($set.Text) { ": $($set.Text)" })); run as administrator or choose it by hand" }
            }
        }
    } catch {
        $planNote = "power plan unchanged: $($_.Exception.Message)"
    }
    if ($planNote) { Write-Host $planNote -ForegroundColor Yellow }
}
$activePlan = (Get-ActivePlan).Text
if ($origPlanGuid) { $activePlan += '  (switched for the run)' }

$failed = $false
try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $timeRule = if ($Rounds -ge 3) { "$Rounds runs, best and worst dropped, mean of the rest" } else { "median of $Rounds runs" }
    Write-Report '=================================================================================='
    Write-Report "ctJxl against cjxl (libjxl 0.13.0)  $(Get-Date -Format 'yyyy-MM-dd HH:mm')  computer $env:COMPUTERNAME"
    Write-Report "CPU: $($cpu.Name.Trim()), $($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) logical processors"
    Write-Report "power plan: $activePlan"
    if ($planNote) { Write-Report "power plan note: $planNote" }
    Write-Report "folder: $ImageDir  ($($images.Count) images)"
    Write-Report "cases: $($Cases -join ' | ')   feature set: $Simd$(if ($Threads -gt 0) { "   --num_threads $Threads" })   time: $timeRule"
    Write-Report "file cache: $(if ($Warm) { 'warm (the images are read from the file cache)' } else { "cold (a fresh unbuffered copy in $work\cold before every run)" })"
    Write-Report '=================================================================================='

    $rows = @()
    $caseIndex = 0
    foreach ($case in $Cases) {
        $caseIndex++
        $opts = @($case -split '\s+' | Where-Object { $_ })
        Write-Report ''
        Write-Report "case $caseIndex of $($Cases.Count): $case"
        $index = 0
        foreach ($image in $images) {
            $index++
            $mp = Get-Megapixels $image.FullName
            $times = @{}
            $hashes = @{}
            $errors = @{}
            foreach ($c in $programs) { $times[$c.Name] = @() }
            $differs = $false
            for ($r = 0; $r -lt $Rounds; $r++) {
                foreach ($c in $programs) {
                    if ($errors.ContainsKey($c.Name)) { continue }
                    $out = Join-Path $work ("{0}-{1:D4}-{2}.jxl" -f $caseIndex, $index, $c.Name)
                    $in = if ($Warm) { $image.FullName } else { New-ColdCopy $image.FullName }
                    $t = Invoke-Timed $c.Exe (& $c.Args $in $out $opts)
                    if (-not $Warm) { Remove-ColdCopy $in }
                    if ($t -lt 0) {
                        $errors[$c.Name] = $script:lastError
                        continue
                    }
                    $times[$c.Name] += $t
                    # every run against the first run of the reference
                    $hash = Get-Sha256 $out
                    if ($r -eq 0 -and $c.Name -eq 'cjxl') { $hashes['cjxl'] = $hash }
                    elseif ($c.Name -eq 'ctJxl' -and $hash -ne $hashes['cjxl']) { $differs = $true }
                    if (-not $KeepJxl -or $r -lt $Rounds - 1) { Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue }
                }
            }

            $row = [ordered]@{ Case = $case; File = $image.Name; Megapixels = [math]::Round($mp, 2) }
            foreach ($c in $programs) { $row[$c.Name] = if ($times[$c.Name].Count -gt 0) { Get-RunTime $times[$c.Name] } else { [double]::NaN } }
            $row['Factor'] = if ($row['ctJxl'] -gt 0 -and -not [double]::IsNaN($row['cjxl'])) { $row['cjxl'] / $row['ctJxl'] } else { [double]::NaN }
            $row['Identical'] = if ($errors.Count -gt 0) { 'error' } elseif ($differs) { 'NO' } else { 'yes' }
            $row['Error'] = ($errors.GetEnumerator() | ForEach-Object { "$($_.Key): $($_.Value)" }) -join '; '
            $rows += [pscustomobject]$row

            $line = "[{0}/{1}] {2}" -f $index, $images.Count, $image.Name
            if ($mp -gt 0) { $line += "  {0:N1} MP" -f $mp }
            foreach ($c in $programs) { $line += "  {0}: {1} s" -f $c.Name, (Format-Seconds $row[$c.Name]) }
            $line += "  factor {0}x  identical: {1}" -f (Format-Seconds $row['Factor']), $row['Identical']
            Write-Report $line $(if ($row['Identical'] -ne 'yes') { 'Red' } else { '' })
            if ($row['Error']) { Write-Report "    $($row['Error'])" 'Red' }
        }
    }

    # totals per case over the images both programs encoded bit-identically
    Write-Report ''
    Write-Report '=================================================================================='
    Write-Report ('{0,-28} {1,7} {2,11} {3,11} {4,9}' -f 'case', 'images', 'cjxl s', 'ctJxl s', 'factor')
    Write-Report '----------------------------------------------------------------------------------'
    foreach ($case in $Cases) {
        $ok = @($rows | Where-Object { $_.Case -eq $case -and $_.Identical -eq 'yes' })
        $ref = ($ok | Measure-Object cjxl -Sum).Sum
        $ct = ($ok | Measure-Object ctJxl -Sum).Sum
        $factor = if ($ct -gt 0) { $ref / $ct } else { [double]::NaN }
        Write-Report ('{0,-28} {1,7} {2,11:N3} {3,11:N3} {4,8:N2}x' -f $case, $ok.Count, $ref, $ct, $factor)
    }
    Write-Report '----------------------------------------------------------------------------------'
    $identical = @($rows | Where-Object { $_.Identical -eq 'yes' }).Count
    $failed = $identical -ne $rows.Count
    $msg = "bit-identical to cjxl on $Simd`: $identical of $($rows.Count) encodings"
    Write-Report $msg $(if ($identical -eq $rows.Count) { 'Green' } else { 'Red' })
    Write-Report '=================================================================================='

    $rows | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath $csv
    Write-Report ''
    Write-Report "report: $report"
    Write-Report "numbers per image: $csv"
} finally {
    if ($origPlanGuid) {
        $null = Invoke-PowerCfg @('/setactive', $origPlanGuid)
        Write-Host "Power plan restored ($origPlanGuid)" -ForegroundColor Cyan
    }
    if ($createdPlan) { $null = Invoke-PowerCfg @('/delete', $createdPlan) }
}
# exit codes (the same in all ThinkMeta.ConcurrentTasks sample packages): 0 every encoding bit-identical, 2 an encoding
# differs or a program failed on it, 1 other errors (missing programs, wrong parameters, no images, abort; PowerShell's
# own exit code for a terminating error)
if ($failed) { exit 2 }
exit 0
