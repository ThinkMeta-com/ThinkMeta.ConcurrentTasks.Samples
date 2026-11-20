<#
.SYNOPSIS
    Bit-compare harness for ctJxl: every output must have the SHA-256 that the reference cjxl (bin\win-x64\cjxl.exe,
    bin/linux-x64/cjxl: unmodified libjxl 0.13.0, --target avx2) gave.

.DESCRIPTION
    -Mode Compare    encodes the test matrix with a candidate (default: your Release build, build\Release\bin\ctJxl.exe,
                     on Linux build/linux-release/bin/ctJxl; -Exe bin\win-x64\ctJxl.exe or bin/linux-x64/ctJxl checks
                     the published one) and reports every case whose hash, size or exit code differs from the table.
                     Exit code 1 on any difference. ctJxl needs an AVX2 CPU here: on Linux it chooses its SIMD code at
                     run time, and the tables are those of AVX2.
    -Mode Reference  writes the table with the reference cjxl.

    -ExtraArgs (candidate only, e.g. '--pool libjxl') is not part of the case key. -Repeat n encodes every case n times;
    all outputs must match (catches output that depends on thread timing). Inputs: testdata\, made by
    verify\Prepare-Inputs.ps1. Needs PowerShell 7 (pwsh).

    Matrices (one case per encoder path and per cjxl option, no full cross product):
      Quick        small images: sizes, distances, efforts 1-11, every lossy and modular option, every input format,
                   animation, metadata, output modes (232 cases).
      Conformance  the 25 reference images of the JPEG XL conformance set at -q 90 and -d 0 (50 cases).
      Smoke        a subset of Quick (its table) for Debug builds and other thread counts.

    Parameters as in every Verify script: -Mode Compare|Reference, -Matrix, -HashFile (default
    Reference-Hashes-<Matrix>-avx2-<platform>.csv: each platform has the tables of its own cjxl), -Exe, -Oracle,
    -ExtraArgs, -Repeat, -ThrottleLimit, -WorkDir, -TimeoutSec.

.EXAMPLE
    pwsh verify\Verify-Jxl.ps1 -Matrix Quick
    pwsh verify\Verify-Jxl.ps1 -Matrix Conformance -Exe bin\win-x64\ctJxl.exe
    pwsh verify/Verify-Jxl.ps1 -Matrix Quick -Exe bin/linux-x64/ctJxl
#>
[CmdletBinding()]
param(
    [ValidateSet('Reference', 'Compare')][string]$Mode = 'Compare',
    [ValidateSet('Smoke', 'Quick', 'Conformance')][string]$Matrix = 'Quick',
    [string]$Exe = $(if ($IsLinux) { Join-Path $PSScriptRoot '../build/linux-release/bin/ctJxl' }
                     else { Join-Path $PSScriptRoot '..\build\Release\bin\ctJxl.exe' }),
    [string]$Oracle = $(if ($IsLinux) { Join-Path $PSScriptRoot '../bin/linux-x64/cjxl' }
                        else { Join-Path $PSScriptRoot '..\bin\win-x64\cjxl.exe' }),
    [string]$HashFile,
    [string]$ExtraArgs = '',
    [ValidateRange(1, 100)][int]$Repeat = 1,
    [int]$ThrottleLimit = [Math]::Max(1, [Environment]::ProcessorCount / 2),
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'JxlVerify'),
    [int]$TimeoutSec = 900,
    [ValidateSet('sse2', 'avx2')][string]$Simd = 'avx2',
    [string]$InputRoot = (Join-Path $PSScriptRoot '../testdata')
)

$ErrorActionPreference = 'Stop'
if ($Mode -eq 'Reference') {
    if ($ExtraArgs) { throw '-ExtraArgs is for compare runs only.' } # -Repeat 2 checks the oracle's determinism
    $Exe = $Oracle
}
$Exe = (Resolve-Path $Exe).Path
$isOracle = (Split-Path -Leaf $Exe) -match '^cjxl(-oracle)?(\.exe)?$' # the public copy names it cjxl(.exe)
if ($Mode -eq 'Reference' -and -not $isOracle) { throw 'Reference tables are written with cjxl-oracle only.' }
if (-not $isOracle -and $Simd -ne 'avx2') { throw 'ctJxl runs its AVX2 code on an AVX2 CPU (Windows: only that): compare with -Simd avx2.' }
if (-not $HashFile) {
    $tableMatrix = if ($Matrix -eq 'Smoke') { 'Quick' } else { $Matrix }
    $platform = if ($IsLinux) { '-linux-x64' } else { '-win-x64' } # the hashes of each platform's own oracle
    $HashFile = Join-Path $PSScriptRoot "Reference-Hashes-$tableMatrix-$Simd$platform.csv"
}
$InputRoot = (Resolve-Path $InputRoot).Path
# the inputs, made by verify\Prepare-Inputs.ps1
$img = Join-Path $InputRoot 'images'
if (-not (Test-Path (Join-Path $img 'crop-800x600.ppm'))) { throw "no test images in $img`: run verify\Prepare-Inputs.ps1 first" }
$gif = Join-Path $InputRoot 'gif'
$pts = Join-Path $InputRoot 'pts'
$photos = Join-Path $InputRoot 'photos'
$conformance = Join-Path $InputRoot 'conformance'
$animations = Join-Path $InputRoot 'animations'
$cases = [Collections.Generic.List[object]]::new()
# -Optional: an input that may be missing (made with a tool that is not always installed); its cases are skipped
function Add-Cases([string[]]$ArgSets, [string[]]$Inputs, [switch]$Optional) {
    foreach ($a in $ArgSets) {
        foreach ($i in $Inputs) {
            if (-not (Test-Path $i)) {
                if ($Optional) { Write-Warning "skipped, input missing: $a | $i"; continue }
                throw "input missing: $i"
            }
            $cases.Add([pscustomobject]@{ Args = $a; Input = (Resolve-Path $i).Path; Name = Split-Path -Leaf $i })
        }
    }
}
function I([string]$name) { Join-Path $img $name }

function Add-Quick {
    # W1/W2/W3: sizes around the path decisions (1 DC group, 2x2 DC groups, 8 and 9 AC groups, <= 8, odd, tiny)
    Add-Cases '-q 90', '-d 0' (I 'crop-1024x768.ppm'), (I 'crop-2560x2304.ppm'), (I 'crop-640x480.ppm'), (I 'crop-1024x512.ppm'),
        (I 'crop-768x768.ppm'), (I 'crop-257x257.ppm'), (I 'crop-7x5.ppm'), (I 'crop-1x1.ppm'), (I 'crop-1x2100.ppm'), (I 'crop-128x128.ppm')
    Add-Cases '-q 50', '-q 68', '-q 95', '-d 0.1', '-d 3', '-d 25' (I 'crop-1024x768.ppm')
    # efforts; e8/e9 stream only at distance <= 0.5
    Add-Cases (1..9 | ForEach-Object { "-q 90 -e $_" }) (I 'crop-640x480.ppm'), (I 'crop-1024x768.ppm')
    Add-Cases '-q 90 -e 10' (I 'crop-640x480.ppm')
    # butteraugli (effort 8 and 9) on images below 8 pixels and on a narrow, tall one
    Add-Cases '-q 90 -e 8', '-q 90 -e 9' (I 'crop-7x5.ppm'), (I 'crop-1x1.ppm'), (I 'crop-1x2100.ppm')
    Add-Cases '-d 0.5 -e 8', '-d 0.5 -e 9' (I 'crop-1024x768.ppm')
    Add-Cases (1..10 | ForEach-Object { "-d 0 -e $_" }) (I 'crop-640x480.ppm')
    Add-Cases '-d 0 -e 11 --allow_expert_options' (I 'crop-128x128.ppm')
    # effort 11 on a PNG: its trial encodings read the rows of the PNG reader task at the same time (InputBands, many
    # waiters per band)
    Add-Cases '-d 0 -e 11 --allow_expert_options' (I 'png-palette-trns-320x240.png')
    # lossy options on a streamed size (12 AC groups); each option once
    Add-Cases ('-p', '--progressive_ac', '--qprogressive_ac', '--progressive_dc 1', '--progressive_dc 2',
        '--photon_noise_iso 400', '--noise 1', '--resampling 2', '--resampling 4',
        '--resampling 2 --already_downsampled --upsampling_mode 0', '--resampling 4 --already_downsampled --upsampling_mode 1',
        '--epf 0', '--epf 3', '--gaborish 0', '--faster_decoding 2', '--faster_decoding 4', '--buffering 0', '--buffering 3',
        '--group_order 1', '--group_order 1 --center_x 100 --center_y 500', '--intensity_target 1000',
        '--disable_perceptual_optimizations', '--codestream_level 10', '--codestream_level 10 --container 1', '--override_bitdepth 10',
        '--container 1', '-x color_space=DisplayP3', '-x color_space=Rec2100PQ', '-x color_space=Rec2100HLG',
        "-x icc_pathname={img}\display-rgb.icc", "-x exif={img}\meta.exif", "-x exif={img}\meta-orient6.exif",
        "-x exif={img}\meta.exif --container 0", "-x xmp={img}\meta.xmp", "-x xmp={img}\meta.xmp --compress_boxes 0",
        "-x xmp={img}\meta.xmp --brotli_effort 3", "-x jumbf={img}\meta.jumbf",
        '--streaming_output --output_mode 1', '--streaming_input --streaming_output --output_mode 1', '--output_mode 2',
        '--buffering 0 --output_mode 2', '--num_threads 0', '-m 0', '-m 1' | ForEach-Object { "-q 90 $_" }) (I 'crop-800x600.ppm')
    Add-Cases '-d 3 --buffering 2', '-d 0 -e 1 --streaming_input --streaming_output' (I 'crop-800x600.ppm')
    Add-Cases '-q 90 --buffering 1' (I 'crop-2560x2304.ppm'), (I 'crop-1024x768.ppm')
    Add-Cases '-q 90 -e 9 --resampling 2' (I 'crop-640x480.ppm')
    Add-Cases '-q 90 --disable_output', '-q 90 --num_reps 2', '-q 90 --num_reps 2 --streaming_output' (I 'crop-7x5.ppm')
    # patches and dots: the whole-frame path, images that have them
    Add-Cases '-q 90 --buffering 0', '-q 90 --buffering 0 --patches 0', '-q 90 --patches 1', '-d 0', '-d 0 --buffering 0',
        '-d 0 --buffering 0 --patches 0' (I 'document-1024x768.png')
    Add-Cases '-q 90 --buffering 0', '-q 90 --buffering 0 --dots 1', '-d 3 --buffering 0', '-d 3 --buffering 0 --dots 0' (I 'dots-800x600.png')
    # modular / lossless options (non-default values)
    Add-Cases ('-I 0', '-I 25', '-I 100', '-C 0', '-C 6', '-C 41', '-g 0', '-g 2', '-g 3', '-P 0', '-P 5', '-P 15',
        '-E 3', '-R 1', '-p', '--faster_decoding 2', '--faster_decoding 4', '--modular_lossy_palette --modular_palette_colors 0' |
        ForEach-Object { "-d 0 $_" }) (I 'crop-640x480.ppm')
    Add-Cases '-m 1 -q 80', '-m 1 -q 80 -R 0', '-m 1 -q 80 -p' (I 'crop-640x480.ppm')
    Add-Cases '-d 0 --modular_palette_colors 0', '-d 0 --modular_palette_colors 0 -X 0 -Y 0', '-d 0 --modular_palette_colors 0 -X 100 -Y 100' (Join-Path $gif 'welcome2.gif')
    # alpha
    Add-Cases '-q 90', '-d 0', '-q 90 -a 1', '-q 90 --strip_alpha 1', '-q 90 --premultiply 1', '-q 90 --keep_invisible 1',
        '-d 0 --keep_invisible 0', '-q 90 --ec_resampling 2', '-d 0 -e 1' (I 'rgba-640x480.png')
    Add-Cases '-q 90', '-q 90 --strip_alpha 0', '-d 0', '-d 0 --strip_alpha 2' (I 'rgba-opaque-640x480.png')
    Add-Cases '-q 90', '-d 0', '-q 90 -a 1' (I 'crop-800x600-alpha.pam')
    # other input formats and bit depths
    Add-Cases '-q 90', '-d 0' (I 'crop-800x600-16bit.ppm'), (I 'crop-800x600.pfm'), (I 'crop-800x600-gray.pgm'), (I 'crop-800x600-gray.pgx'),
        (I 'crop-800x600-q90.jxl'), (I 'anim-320x240.png')
    # alpha over several DC groups (extra channel in every DC group task, ctJxl plan W8)
    Add-Cases '-q 90', '-d 0' (I 'crop-2560x2304-alpha.pam'), (I 'crop-2560x2304-depth.pam')
    Add-Cases '-q 90 -a 1' (I 'crop-2560x2304-alpha.pam')
    # animation: frames encoded ahead in parallel with streamed output and with alpha stripped (ctJxl plan D7)
    Add-Cases '-d 0 --streaming_output', '-q 90 --strip_alpha 1' (I 'anim-320x240.png')
    Add-Cases '-d 0 -e 1' (I 'crop-800x600-16bit.ppm'), (I 'crop-800x600-gray.pgm')
    Add-Cases '-q 90 --override_bitdepth 12' (I 'crop-800x600-16bit.ppm')
    # nested regions (frames ahead x MA subtrees x properties) with many workers: the runner's task budget keeps the
    # runnable tasks within the scheduler's queue (from 17 workers on this case used to spin). The GIF, a camera pan
    # over big_building.ppm, is made with ffmpeg; without it the case is skipped.
    Add-Cases '-d 0 -e 9 --num_threads 64' (Join-Path $animations 'pan-640x360-30.gif') -Optional
    # LZ77 taken over several streams of the global histograms (ctJxl plan D4.4)
    Add-Cases '-d 0 -e 10' (I 'crop-800x600-16bit.ppm')
    # PNM with 10 and 12 bit (maxval 1023 / 4095), also over several DC groups
    Add-Cases '-q 90', '-d 0' (I 'crop-800x600-10bit.ppm'), (I 'crop-800x600-12bit.pgm')
    Add-Cases '-q 90' (I 'crop-2560x2304-10bit.ppm')
    # PNG variants (ctJxl decodes the rows of a PNG while encoding: depth, gray, palette with tRNS, iCCP, a text chunk
    # after the pixel data, many IDAT chunks over several DC groups)
    Add-Cases '-q 90', '-d 0' (I 'png-16bit-sbit-800x600.png'), (I 'png-gray-800x600.png'), (I 'png-palette-trns-320x240.png'),
        (I 'png-iccp-800x600.png'), (I 'png-text-after-800x600.png')
    Add-Cases '-q 90' (I 'png-idats-2560x2304.png')
    # GIF (also animated, with transparency)
    Add-Cases '-d 0', '-q 90' (Join-Path $gif 'welcome2.gif'), (Join-Path $gif 'fire.gif'), (Join-Path $gif 'x-trans.gif'), (Join-Path $gif 'porsche.gif')
    Add-Cases '-d 0 --frame_indexing 1000', '-q 90 -e 9' (Join-Path $gif 'fire.gif')
    # JPEG: lossless recompression (default) and as pixels; 4:2:0, 4:4:4 (CfL), with Exif/XMP segments
    Add-Cases '', '-j 1 --allow_jpeg_reconstruction 0', '-j 1 --jpeg_reconstruction_lfs 1', '-j 1 -e 9', '-j 1 --container 0',
        '-q 90', '-j 0 -q 90', '-j 0 -d 0' (I 'crop-800x600.jpg')
    Add-Cases '', '-j 1 --jpeg_reconstruction_cfl 0', '-j 1 --jpeg_reconstruction_lfs 0' (I 'crop-800x600-444.jpg')
    Add-Cases '', '-j 1 --allow_jpeg_reconstruction 0 -x strip=exif', '-j 1 --allow_jpeg_reconstruction 0 -x strip=xmp', '-j 0 -q 90' (I 'crop-800x600-meta.jpg')
}

function Add-Full {
    $ppm = @(Get-ChildItem $photos -Filter *.ppm | Sort-Object Name | ForEach-Object FullName)
    Add-Cases '-q 70', '-q 90', '-d 0' $ppm
    Add-Cases '-q 68', '-q 80', '-q 90', '-q 95', '-d 0' (Join-Path $pts 'sample-photo-6000x4000.ppm'), (Join-Path $pts 'sample-4.png')
    Add-Cases '', '-j 0 -q 90' (Join-Path $pts 'sample-photo-6000x4000.JPG')
    Add-Cases (1..9 | ForEach-Object { "-q 90 -e $_" }) (I 'crop-2560x2304.ppm')
    Add-Cases (1..9 | ForEach-Object { "-d 0 -e $_" }) (I 'crop-2560x2304.ppm')
}

# Smoke: a subset of Quick (same arguments on the same inputs), for Debug and thread-count runs
if ($Matrix -eq 'Smoke') {
    if ($Mode -eq 'Reference') { throw 'Smoke uses the Quick table; write the Quick table instead.' }
    Add-Cases '-q 90', '-d 0' (I 'crop-1024x768.ppm'), (I 'crop-2560x2304.ppm'), (I 'crop-640x480.ppm'), (I 'crop-7x5.ppm')
    Add-Cases '-q 50' (I 'crop-1024x768.ppm')
    Add-Cases '-q 90 -p' (I 'crop-800x600.ppm')
    Add-Cases '-m 1 -q 80', '-d 0 -e 1' (I 'crop-640x480.ppm')
    Add-Cases '-q 90' (I 'rgba-640x480.png'), (I 'anim-320x240.png')
    Add-Cases '' (I 'crop-800x600.jpg')
}
elseif ($Matrix -eq 'Quick') { Add-Quick }
elseif ($Matrix -eq 'Conformance') {
    if (-not (Test-Path $conformance)) { & (Join-Path $InputRoot 'Get-Conformance.ps1') | Out-Null }
    $files = @(Get-ChildItem $conformance -File | Where-Object { $_.Extension -in '.png', '.apng', '.gif', '.pfm' } | Sort-Object Name | ForEach-Object FullName)
    if ($files.Count -ne 25) { throw "conformance-images: $($files.Count) images, expected 25 (Get-Conformance.ps1)" }
    Add-Cases '-q 90', '-d 0' $files
}
else { Add-Full }

New-Item -ItemType Directory -Force $WorkDir | Out-Null
$how = if ($isOracle) { "--target $Simd" } else { "$Simd $ExtraArgs".Trim() }
Write-Host "$Mode/$Matrix/$Simd`: $($cases.Count) cases x$Repeat with $Exe ($how, x$ThrottleLimit), table $(Split-Path -Leaf $HashFile)"
$sw = [Diagnostics.Stopwatch]::StartNew()

$results = $cases | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
    $c = $_
    $runs = foreach ($n in 1..$using:Repeat) {
        $out = Join-Path $using:WorkDir ([guid]::NewGuid().ToString() + '.jxl')
        # split first, then the folder (which may contain spaces); '{img}\name' is part of the case key, the separator
        # is the platform's
        $caseArgs = @($c.Args -split ' ' | Where-Object { $_ } |
            ForEach-Object { $_.Replace('{img}\', $using:img + [IO.Path]::DirectorySeparatorChar) })
        if ($using:isOracle) { $argList = @('--target', $using:Simd, $c.Input, $out) + $caseArgs }
        else { $argList = @($c.Input, $out) + $caseArgs + @($using:ExtraArgs -split ' ' | Where-Object { $_ }) }
        $p = Start-Process -FilePath $using:Exe -ArgumentList ($argList | ForEach-Object { '"' + $_ + '"' }) `
            -NoNewWindow -PassThru -RedirectStandardError "$out.err" -RedirectStandardOutput "$out.log"
        $null = $p.Handle # keeps the exit code readable after the wait
        $code = if ($p.WaitForExit($using:TimeoutSec * 1000)) { $p.ExitCode } else { $p.Kill($true); $p.WaitForExit(); 'timeout' }
        $r = [pscustomobject]@{ ExitCode = $code; Size = -1; Sha256 = '' }
        # a failed or killed run may leave a partial file (--streaming_output opens it early): hash only on success
        if ($code -eq 0 -and (Test-Path $out)) {
            $r.Sha256 = (Get-FileHash -Algorithm SHA256 $out).Hash.ToLowerInvariant()
            $r.Size = (Get-Item $out).Length
        }
        Remove-Item $out -ErrorAction SilentlyContinue
        Remove-Item "$out.err", "$out.log" -ErrorAction SilentlyContinue
        $r
    }
    $distinct = @($runs | ForEach-Object { "$($_.ExitCode)/$($_.Sha256)" } | Select-Object -Unique)
    $first = $runs | Select-Object -First 1
    [pscustomobject]@{ Key = "$($c.Args)|$($c.Name)"; ExitCode = $first.ExitCode; Size = $first.Size; Sha256 = $first.Sha256
        Unstable = $distinct.Count -gt 1 }
} | Sort-Object Key -CaseSensitive

Write-Host ("Encoded in {0:mm\:ss}" -f $sw.Elapsed)
$failed = @($results | Where-Object ExitCode -ne 0)

if ($Mode -eq 'Reference') {
    $unstable = @($results | Where-Object Unstable)
    if ($unstable.Count -gt 0) { $unstable | ForEach-Object { Write-Warning "oracle unstable: $($_.Key)" }; throw 'oracle output not deterministic' }
    # keys are case-sensitive: '-e 3' (effort) and '-E 3' (previous channels) are different options
    $keys = @($results | Group-Object Key -CaseSensitive | Where-Object Count -gt 1)
    if ($keys) { throw "duplicate case keys: $($keys.Name -join ', ')" }
    $results | Select-Object Key, ExitCode, Size, Sha256 | Export-Csv -Encoding utf8 $HashFile
    Write-Host "Wrote $($results.Count) hashes to $HashFile ($($failed.Count) non-zero exit codes)"
    $failed | ForEach-Object { Write-Warning "exit $($_.ExitCode): $($_.Key)" }
    return
}

$reference = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
Import-Csv $HashFile | ForEach-Object { $reference[$_.Key] = $_ }
$diffs = foreach ($r in $results) {
    $ref = $null
    if (-not $reference.TryGetValue($r.Key, [ref]$ref)) { [pscustomobject]@{ Key = $r.Key; Problem = 'not in reference' }; continue }
    [void]$reference.Remove($r.Key)
    if ($r.Unstable) { [pscustomobject]@{ Key = $r.Key; Problem = "output differs between the $Repeat runs" } }
    elseif ("$($r.ExitCode)" -ne $ref.ExitCode) { [pscustomobject]@{ Key = $r.Key; Problem = "exit $($ref.ExitCode) -> $($r.ExitCode)" } }
    elseif ($r.Sha256 -ne $ref.Sha256) { [pscustomobject]@{ Key = $r.Key; Problem = "hash differs (size $($ref.Size) -> $($r.Size))" } }
}
$caseKeys = [Collections.Generic.Dictionary[string, bool]]::new([StringComparer]::Ordinal)
$cases | ForEach-Object { $caseKeys["$($_.Args)|$($_.Name)"] = $true }
$missing = @(@($reference.Keys) | Where-Object { $caseKeys.ContainsKey($_) } |
    ForEach-Object { [pscustomobject]@{ Key = $_; Problem = 'missing in run' } })
$all = @($diffs) + $missing

if ($all.Count -eq 0) {
    Write-Host "OK: all $($results.Count) cases bit-identical to $HashFile" -ForegroundColor Green
}
else {
    $all | Sort-Object Key -CaseSensitive | Format-Table -AutoSize | Out-String -Width 300 | Write-Host
    Write-Host "FAIL: $($all.Count) of $($results.Count) cases differ" -ForegroundColor Red
    exit 1
}
