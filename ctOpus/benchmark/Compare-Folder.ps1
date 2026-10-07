<#
.SYNOPSIS
    ctOpus against opusenc on a folder of WAV files: speed and bit identity.

.DESCRIPTION
    Encodes every WAV file in the folder with opusenc (opus-tools 0.2, libopusenc 0.2.1,
    libopus 1.6.1) and with ctOpus, file after file, round by round. Measures the wall time of
    the whole process (reading, resampling, encoding, Ogg packetization, writing).

    Bit identity: the output of ctOpus (the parallel encoder, and with -Sequential also
    ctOpus --sequential) is compared byte for byte with the output of opusenc with the same
    settings. All programs get --serial 1, so the Ogg stream serial numbers match. Speedups are
    only meaningful for files marked identical = yes.

    -ReferenceArgs lets opusenc run with other settings than ctOpus, for example
    "ctOpus --comp 10 against opusenc --comp 0". The identity check then uses an extra, untimed
    opusenc run with the ctOpus settings.

    Power plan: the script switches to High Performance for reproducible timing (Ultimate
    Performance if that is the only one; where the plan is hidden, a temporary copy) and restores
    the previous plan at the end. If Windows refuses the switch (e.g. Windows Server without
    administrator rights, or a group policy), the report says so and the run goes on with the
    current plan. -KeepPowerPlan leaves the plan alone.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 D:\Music
    powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 D:\Music -EncodeArgs "--comp 10" -ReferenceArgs "--comp 0"
    powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 D:\Music -Sequential -Rounds 3
#>
param(
    [Parameter(Position = 0)][string]$WavDir = '',
    [string]$EncodeArgs = '--comp 10 --bitrate 128',
    [string]$ReferenceArgs = '',
    [int]$Rounds = 1,
    [switch]$Sequential,
    [int]$Threads = 0,
    [string]$Files = 'All',
    [switch]$Recurse,
    [switch]$KeepOpus,
    [switch]$KeepPowerPlan
)

$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
$bin     = if (Test-Path (Join-Path $root 'bin')) { Join-Path $root 'bin' } else { Join-Path (Split-Path $root) 'bin' }
$work    = Join-Path $root 'work'
$results = Join-Path $root 'results'
New-Item -ItemType Directory -Force -Path $work, $results | Out-Null

$ctOpus  = Join-Path $bin 'ctOpus.exe'
$opusenc = Join-Path $bin 'opusenc.exe'
if (-not (Test-Path $ctOpus))  { throw "ctOpus.exe not found at $ctOpus" }
if (-not (Test-Path $opusenc)) { throw "opusenc.exe not found at $opusenc" }

if (-not $WavDir) { $WavDir = Read-Host 'Folder with WAV files' }
$WavDir = $WavDir.Trim('"')
if (-not (Test-Path -LiteralPath $WavDir -PathType Container)) { throw "Folder not found: $WavDir" }

$allWavs = @(Get-ChildItem -LiteralPath $WavDir -Filter *.wav -File -Recurse:$Recurse | Sort-Object FullName)
if ($allWavs.Count -eq 0) { throw "No WAV files found in $WavDir" }
if ($Files -eq 'Quick') {
    $wavs = for ($k = 0; $k -lt $allWavs.Count; $k += 17) { $allWavs[$k] }
} elseif ($Files -match '^\d+$') {
    $wavs = @($allWavs | Select-Object -First ([int]$Files))
} else {
    $wavs = $allWavs
}

if (-not $ReferenceArgs) { $ReferenceArgs = $EncodeArgs }
$sameArgs = $ReferenceArgs -eq $EncodeArgs
$common   = @('--quiet', '--serial', '1')
$ctArgs   = @($EncodeArgs -split ' ' | Where-Object { $_ })
$refArgs  = @($ReferenceArgs -split ' ' | Where-Object { $_ })

$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$report = Join-Path $results ("bench-{0}-{1}.txt" -f $env:COMPUTERNAME, $stamp)
$csv    = Join-Path $results ("bench-{0}-{1}.csv" -f $env:COMPUTERNAME, $stamp)

$refName = if ($sameArgs) { 'opusenc' } else { "opusenc $ReferenceArgs" }
$ctName  = if ($sameArgs) { 'ctOpus' } else { "ctOpus $EncodeArgs" }
$candidates = @(
    [pscustomobject]@{ Name = $refName; Exe = $opusenc; Args = $refArgs; Check = $false }
    [pscustomobject]@{ Name = $ctName; Exe = $ctOpus; Args = $ctArgs + @(if ($Threads -gt 0) { '--threads', "$Threads" }); Check = $true }
)
if ($Sequential) {
    $candidates += [pscustomobject]@{ Name = 'ctOpus --sequential'; Exe = $ctOpus; Args = $ctArgs + @('--sequential'); Check = $true }
}

function Write-Report([string]$text, [string]$color = '') {
    if ($color) { Write-Host $text -ForegroundColor $color } else { Write-Host $text }
    Add-Content -LiteralPath $report -Value $text -Encoding UTF8
}

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
        return -1
    }
    return $sw.Elapsed.TotalSeconds
}

function Median([double[]]$values) {
    $s = @($values | Sort-Object)
    if ($s.Count -eq 0) { return [double]::NaN }
    if ($s.Count % 2 -eq 1) { return $s[($s.Count - 1) / 2] }
    return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2
}

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
$origPlanGuid = $null
$exitCode = 0
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

try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    Write-Report '=================================================================================='
    Write-Report "ctOpus against opusenc  $(Get-Date -Format 'yyyy-MM-dd HH:mm')  computer $env:COMPUTERNAME"
    Write-Report "CPU: $($cpu.Name.Trim()), $($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) logical processors"
    Write-Report "power plan: $activePlan"
    if ($planNote) { Write-Report "power plan note: $planNote" }
    Write-Report "folder: $WavDir  ($($wavs.Count) WAV files)"
    Write-Report "ctOpus: $EncodeArgs$(if ($Threads -gt 0) { " --threads $Threads" })   opusenc: $ReferenceArgs   rounds: $Rounds"
    Write-Report '=================================================================================='
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

        # the reference for the identity check: opusenc with the ctOpus settings
        $refHash = $null
        if (-not $sameArgs) {
            $out = Join-Path $work ("{0:D4}-check.opus" -f $index)
            if ((Invoke-Timed $opusenc ($common + $ctArgs + @($wav.FullName, $out))) -ge 0) {
                $refHash = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash
            } else {
                $errors['opusenc (check)'] = $script:lastError
            }
            Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
        }

        for ($r = 0; $r -lt $Rounds; $r++) {
            $k = 0
            foreach ($c in $candidates) {
                $k++
                if ($errors.ContainsKey($c.Name)) { continue }
                $out = Join-Path $work ("{0:D4}-{1}.opus" -f $index, $k)
                $t = Invoke-Timed $c.Exe ($common + $c.Args + @($wav.FullName, $out))
                if ($t -lt 0) {
                    $errors[$c.Name] = $script:lastError
                    continue
                }
                $times[$c.Name] += $t
                if ($r -eq 0) { $hashes[$c.Name] = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash }
                if (-not $KeepOpus -or $r -lt $Rounds - 1) { Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue }
            }
        }
        if ($sameArgs) { $refHash = $hashes[$refName] }

        $row = [ordered]@{ File = $wav.Name; AudioSeconds = [math]::Round($seconds, 2) }
        foreach ($c in $candidates) { $row[$c.Name] = if ($times[$c.Name].Count -gt 0) { Median $times[$c.Name] } else { [double]::NaN } }
        $refTime = $row[$refName]
        $row['Factor'] = if ($row[$ctName] -gt 0 -and -not [double]::IsNaN($refTime)) { $refTime / $row[$ctName] } else { [double]::NaN }
        $same = @(foreach ($c in $candidates | Where-Object Check) { $refHash -and $hashes.ContainsKey($c.Name) -and $hashes[$c.Name] -eq $refHash })
        $row['Identical'] = if ($errors.Count -gt 0) { 'error' } elseif ($same -contains $false) { 'NO' } else { 'yes' }
        $row['Error'] = ($errors.GetEnumerator() | ForEach-Object { "$($_.Key): $($_.Value)" }) -join '; '
        $rows += [pscustomobject]$row

        $line = "[{0}/{1}] {2}  {3} s audio" -f $index, $wavs.Count, $wav.Name, (Format-Seconds $seconds)
        foreach ($c in $candidates) { $line += "  {0}: {1} s" -f $c.Name, (Format-Seconds $row[$c.Name]) }
        $line += "  factor {0}x  identical: {1}" -f (Format-Seconds $row['Factor']), $row['Identical']
        Write-Report $line $(if ($row['Identical'] -ne 'yes') { 'Red' } else { '' })
        if ($row['Error']) { Write-Report "    $($row['Error'])" 'Red' }
    }

    # totals over the files without errors
    $ok = @($rows | Where-Object { -not $_.Error })
    $identical = @($ok | Where-Object { $_.Identical -eq 'yes' }).Count
    $totalAudio = ($ok | Measure-Object AudioSeconds -Sum).Sum
    Write-Report ''
    Write-Report '=================================================================================='
    Write-Report ("Totals over {0} files, {1} s audio ({2:N2} hours)" -f $ok.Count, (Format-Seconds $totalAudio), ($totalAudio / 3600.0))
    Write-Report ('{0,-32} {1,10} {2,14} {3,12}' -f 'program', 'time s', 'x real time', 'vs opusenc')
    Write-Report '----------------------------------------------------------------------------------'
    $refTotal = ($ok | Measure-Object $refName -Sum).Sum
    foreach ($c in $candidates) {
        $total = ($ok | Measure-Object $c.Name -Sum).Sum
        $realTime = if ($total -gt 0) { $totalAudio / $total } else { [double]::NaN }
        $vsRef = if ($total -gt 0) { $refTotal / $total } else { [double]::NaN }
        Write-Report ('{0,-32} {1,10:N2} {2,13:N0}x {3,11:N2}x' -f $c.Name, $total, $realTime, $vsRef)
    }
    Write-Report '----------------------------------------------------------------------------------'
    $msg = "bit-identical to opusenc ($EncodeArgs): $identical of $($rows.Count) files"
    Write-Report $msg $(if ($identical -eq $rows.Count) { 'Green' } else { 'Red' })
    # exit code: 0 every file encoded and identical, 2 a file differs or was not encoded,
    # 1 other errors (missing programs, bad parameters, abort: thrown above)
    if ($identical -ne $rows.Count) { $exitCode = 2 }
    Write-Report '=================================================================================='

    $rows | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath $csv
    Write-Report ''
    Write-Report "report: $report"
    Write-Report "numbers per file: $csv"
} finally {
    if ($origPlanGuid) {
        $null = Invoke-PowerCfg @('/setactive', $origPlanGuid)
        Write-Host "Power plan restored ($origPlanGuid)" -ForegroundColor Cyan
    }
    if ($createdPlan) { $null = Invoke-PowerCfg @('/delete', $createdPlan) }
}
exit $exitCode
