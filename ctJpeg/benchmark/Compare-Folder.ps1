<#
.SYNOPSIS
    ctJpeg against jpegli (cjpegli), mozjpeg and libjpeg-turbo on a folder of PPM/PGM images:
    speed and bit identity.

.DESCRIPTION
    Encodes every PPM, PGM or PNM image in the folder with cjpegli (jpegli, the reference), ctJpeg,
    and for comparison mozjpeg (cjpeg) and libjpeg-turbo (cjpeg-turbo -optimize -progressive),
    image after image, round by round. Measures the wall time of the whole process (start,
    reading, encoding, writing).

    Bit identity: jpegli writes different bytes depending on the SIMD target it runs on (two
    classes: SSE2/SSSE3/SSE4 and AVX2). ctJpeg reproduces each class as a feature set
    (--simd sse2 | avx2). The script runs cjpegli on the target of the chosen feature set
    (cjpegli --target sse2 | avx2) and compares the ctJpeg output byte for byte with it, for the
    parallel encoder and with -Sequential also ctJpeg --sequential. mozjpeg and libjpeg-turbo are
    other encoders; they are only timed.

    Power plan: the script switches to High Performance for reproducible timing (Ultimate
    Performance if that is the only one; where the plan is hidden, a temporary copy) and restores
    the previous plan at the end, also after an error or Ctrl+C. If Windows refuses the switch
    (e.g. Windows Server without administrator rights, or a group policy), the report says so and
    the run goes on with the current plan. -KeepPowerPlan leaves the plan alone.

    Windows PowerShell 5.1 or later.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 D:\Images
    powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 D:\Images -Quality 90 -Simd sse2 -Sequential -Rounds 3
#>
param(
    [Parameter(Position = 0)][string]$ImageDir = '',
    [int]$Quality = 85,           # -q for jpegli and ctJpeg, -quality for mozjpeg and libjpeg-turbo
    [string]$Simd = 'auto',       # feature set: auto (avx2 if the CPU has it), sse2 or avx2
    [int]$Rounds = 1,             # runs per image and program; the median counts
    [switch]$Sequential,          # also ctJpeg --sequential (one thread)
    [int]$Threads = 0,            # ctJpeg --threads n; 0 = all logical processors
    [switch]$NoOthers,            # skip mozjpeg and libjpeg-turbo
    [string]$Files = 'All',       # All, Quick (every 5th image) or n (the first n images)
    [switch]$Recurse,             # include subfolders
    [switch]$KeepJpeg,            # keep the JPEG files in work\
    [switch]$KeepPowerPlan        # do not switch the power plan
)

$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
# bin\ next to the script (benchmark package) or one level up (the repository, benchmark\)
$bin     = if (Test-Path (Join-Path $root 'bin')) { Join-Path $root 'bin' } else { Join-Path (Split-Path $root) 'bin' }
$work    = Join-Path $root 'work'
$results = Join-Path $root 'results'
New-Item -ItemType Directory -Force -Path $work, $results | Out-Null

$ctJpeg  = Join-Path $bin 'ctJpeg.exe'
$cjpegli = Join-Path $bin 'cjpegli.exe'
$mozjpeg = Join-Path $bin 'cjpeg.exe'
$turbo   = Join-Path $bin 'cjpeg-turbo.exe'
foreach ($exe in $ctJpeg, $cjpegli) { if (-not (Test-Path $exe)) { throw "not found: $exe" } }
if (-not $NoOthers) { foreach ($exe in $mozjpeg, $turbo) { if (-not (Test-Path $exe)) { throw "not found: $exe (or use -NoOthers)" } } }

if (-not $ImageDir) { $ImageDir = Read-Host 'Folder with PPM/PGM images' }
$ImageDir = $ImageDir.Trim('"')
if (-not (Test-Path -LiteralPath $ImageDir -PathType Container)) { throw "Folder not found: $ImageDir" }

$allImages = @(Get-ChildItem -LiteralPath $ImageDir -File -Recurse:$Recurse | Where-Object { $_.Extension -match '^\.(ppm|pgm|pnm)$' } | Sort-Object FullName)
if ($allImages.Count -eq 0) { throw "No PPM/PGM/PNM images found in $ImageDir" }
if ($Files -eq 'Quick') {
    $images = for ($k = 0; $k -lt $allImages.Count; $k += 5) { $allImages[$k] }
} elseif ($Files -match '^\d+$') {
    $images = @($allImages | Select-Object -First ([int]$Files))
} else {
    $images = $allImages
}

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

function Median([double[]]$values) {
    $s = @($values | Sort-Object)
    if ($s.Count -eq 0) { return [double]::NaN }
    if ($s.Count % 2 -eq 1) { return $s[($s.Count - 1) / 2] }
    return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2
}

# megapixels from the PNM header (P5/P6, comments allowed); 0 if unreadable
function Get-Megapixels([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try {
        $buffer = New-Object byte[] 512
        $n = $fs.Read($buffer, 0, $buffer.Length)
        $text = [Text.Encoding]::ASCII.GetString($buffer, 0, $n)
        $text = [regex]::Replace($text, '#[^\n]*', ' ')
        $m = [regex]::Match($text, '^P[256]\s+(\d+)\s+(\d+)')
        if (-not $m.Success) { return 0.0 }
        return [double]$m.Groups[1].Value * [double]$m.Groups[2].Value / 1e6
    } finally { $fs.Dispose() }
}

function Format-Seconds([double]$s) { if ([double]::IsNaN($s)) { '-' } else { $s.ToString('0.000') } }

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

# feature set: avx2 unless ctJpeg reports that this CPU lacks it (an unavailable --simd is an
# error); any other failure of the probe shows up again in the run and must not choose sse2
if ($Simd -eq 'auto') {
    $probe = Join-Path $work 'probe.jpg'
    $Simd = 'avx2'
    if ((Invoke-Timed $ctJpeg @($images[0].FullName, $probe, '-q', "$Quality", '--simd', 'avx2', '--quiet')) -lt 0 -and $script:lastError -match 'not supported by this CPU') { $Simd = 'sse2' }
    Remove-Item -LiteralPath $probe -ErrorAction SilentlyContinue
}
if ($Simd -ne 'sse2' -and $Simd -ne 'avx2') { throw "-Simd must be auto, sse2 or avx2" }

$q = "$Quality"
$ctExtra = @('--simd', $Simd, '--quiet') + @(if ($Threads -gt 0) { '--threads', "$Threads" })
$candidates = @(
    [pscustomobject]@{ Name = "jpegli $Simd"; Exe = $cjpegli; Args = { param($in, $out) @('--target', $Simd, $in, $out, '-q', $q) }; Check = $false; Reference = $true }
    [pscustomobject]@{ Name = 'ctJpeg'; Exe = $ctJpeg; Args = { param($in, $out) @($in, $out, '-q', $q) + $ctExtra }; Check = $true; Reference = $false }
)
if ($Sequential) {
    $candidates += [pscustomobject]@{ Name = 'ctJpeg --sequential'; Exe = $ctJpeg; Args = { param($in, $out) @($in, $out, '-q', $q, '--simd', $Simd, '--quiet', '--sequential') }; Check = $true; Reference = $false }
}
if (-not $NoOthers) {
    $candidates += [pscustomobject]@{ Name = 'mozjpeg'; Exe = $mozjpeg; Args = { param($in, $out) @('-quality', $q, '-outfile', $out, $in) }; Check = $false; Reference = $false }
    $candidates += [pscustomobject]@{ Name = 'libjpeg-turbo'; Exe = $turbo; Args = { param($in, $out) @('-quality', $q, '-optimize', '-progressive', '-outfile', $out, $in) }; Check = $false; Reference = $false }
}
$refName = $candidates[0].Name

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
    Write-Report '=================================================================================='
    Write-Report "ctJpeg against jpegli  $(Get-Date -Format 'yyyy-MM-dd HH:mm')  computer $env:COMPUTERNAME"
    Write-Report "CPU: $($cpu.Name.Trim()), $($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) logical processors"
    Write-Report "power plan: $activePlan"
    if ($planNote) { Write-Report "power plan note: $planNote" }
    Write-Report "folder: $ImageDir  ($($images.Count) images)"
    Write-Report "quality: $Quality   feature set: $Simd$(if ($Threads -gt 0) { "   ctJpeg --threads $Threads" })   rounds: $Rounds"
    Write-Report '=================================================================================='
    Write-Report ''

    $rows = @()
    $index = 0
    foreach ($image in $images) {
        $index++
        $mp = Get-Megapixels $image.FullName
        $times = @{}
        $hashes = @{}
        $errors = @{}
        foreach ($c in $candidates) { $times[$c.Name] = @() }
        for ($r = 0; $r -lt $Rounds; $r++) {
            $k = 0
            foreach ($c in $candidates) {
                $k++
                if ($errors.ContainsKey($c.Name)) { continue }
                $out = Join-Path $work ("{0:D4}-{1}.jpg" -f $index, $k)
                $t = Invoke-Timed $c.Exe (& $c.Args $image.FullName $out)
                if ($t -lt 0) {
                    $errors[$c.Name] = $script:lastError
                    continue
                }
                $times[$c.Name] += $t
                if ($r -eq 0) { $hashes[$c.Name] = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash }
                if (-not $KeepJpeg -or $r -lt $Rounds - 1) { Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue }
            }
        }

        $row = [ordered]@{ File = $image.Name; Megapixels = [math]::Round($mp, 2) }
        foreach ($c in $candidates) { $row[$c.Name] = if ($times[$c.Name].Count -gt 0) { Median $times[$c.Name] } else { [double]::NaN } }
        $refTime = $row[$refName]
        $row['Factor'] = if ($row['ctJpeg'] -gt 0 -and -not [double]::IsNaN($refTime)) { $refTime / $row['ctJpeg'] } else { [double]::NaN }
        $refHash = $hashes[$refName]
        $same = @(foreach ($c in $candidates | Where-Object Check) { $refHash -and $hashes.ContainsKey($c.Name) -and $hashes[$c.Name] -eq $refHash })
        $row['Identical'] = if ($errors.ContainsKey($refName) -or $errors.ContainsKey('ctJpeg')) { 'error' } elseif ($same -contains $false) { 'NO' } else { 'yes' }
        $row['Error'] = ($errors.GetEnumerator() | ForEach-Object { "$($_.Key): $($_.Value)" }) -join '; '
        $rows += [pscustomobject]$row

        $line = "[{0}/{1}] {2}  {3:N1} MP" -f $index, $images.Count, $image.Name, $mp
        foreach ($c in $candidates) { $line += "  {0}: {1} s" -f $c.Name, (Format-Seconds $row[$c.Name]) }
        $line += "  factor {0}x  identical: {1}" -f (Format-Seconds $row['Factor']), $row['Identical']
        Write-Report $line $(if ($row['Identical'] -ne 'yes') { 'Red' } else { '' })
        if ($row['Error']) { Write-Report "    $($row['Error'])" 'Red' }
    }

    # totals over the images that jpegli and ctJpeg encoded
    $ok = @($rows | Where-Object { $_.Identical -ne 'error' })
    $identical = @($ok | Where-Object { $_.Identical -eq 'yes' }).Count
    $totalMp = ($ok | Measure-Object Megapixels -Sum).Sum
    Write-Report ''
    Write-Report '=================================================================================='
    Write-Report ("Totals over {0} images, {1:N1} megapixels (sum of the medians)" -f $ok.Count, $totalMp)
    Write-Report ('{0,-24} {1,10} {2,10} {3,12}' -f 'program', 'time s', 'MP/s', 'vs jpegli')
    Write-Report '----------------------------------------------------------------------------------'
    $refTotal = ($ok | Measure-Object $refName -Sum).Sum
    foreach ($c in $candidates) {
        $total = ($ok | Measure-Object $c.Name -Sum).Sum
        $mps = if ($total -gt 0) { $totalMp / $total } else { [double]::NaN }
        $vsRef = if ($total -gt 0) { $refTotal / $total } else { [double]::NaN }
        Write-Report ('{0,-24} {1,10:N3} {2,10:N1} {3,11:N2}x' -f $c.Name, $total, $mps, $vsRef)
    }
    Write-Report '----------------------------------------------------------------------------------'
    $failed = $identical -ne $rows.Count -or @($rows | Where-Object { $_.Error }).Count -gt 0
    $msg = "bit-identical to jpegli on $Simd (-q $Quality): $identical of $($rows.Count) images"
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
# exit codes (the same in all ThinkMeta.ConcurrentTasks sample packages): 0 every image
# bit-identical, 2 an image differs or a program failed on it, 1 other errors (missing programs,
# wrong parameters, no images, abort; PowerShell's own exit code for a terminating error)
if ($failed) { exit 2 }
exit 0
