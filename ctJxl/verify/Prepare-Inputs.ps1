<#
.SYNOPSIS
    Makes the inputs of Verify-Jxl.ps1 in testdata\ and checks them against verify\Input-Hashes.csv.

.DESCRIPTION
    - testdata\photos\: cathedral.ppm and big_building.ppm from the 8-bit RGB test set of imagecompression.info
      (-Photos: the unpacked rgb8bit folder).
    - testdata\images\: the test images, made by New-TestImages.ps1 from the two photos: crops in sizes around the
      encoder's path decisions, 16 bit, float, gray, alpha, PNG and APNG variants, JPEG (written by cjpegli of
      ..\ctJpeg\bin\<platform>), a JXL (written by cjxl of bin\<platform>), metadata files. The bytes do not depend
      on the machine or the platform: Windows and Linux make the same files.
    - testdata\gif\: four GIFs of giflib's test pictures (jxl\giflib-5.2.2\pic).
    - testdata\conformance\: the 25 decoded reference images of the JPEG XL conformance set
      (github.com/libjxl/conformance, BSD-3-Clause), downloaded at a fixed commit, one per test case.
    - testdata\animations\pan-640x360-30.gif: a camera pan over big_building.ppm, 30 frames, made with ffmpeg
      (-Ffmpeg, default ffmpeg on the PATH). Other ffmpeg versions may write other bytes: then the file is removed
      and its one verify case is skipped, as without ffmpeg.
    Windows PowerShell 5.1 or later, on Linux PowerShell 7 (pwsh).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File verify\Prepare-Inputs.ps1 -Photos D:\Downloads\rgb8bit
    pwsh verify/Prepare-Inputs.ps1 -Photos ~/Downloads/rgb8bit
#>
param(
    [Parameter(Mandatory)][string]$Photos,
    [string]$Ffmpeg = 'ffmpeg'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$data = Join-Path $root 'testdata'
$commit = 'b1d0f990b03e57bf6d137c365cd5dc8b470b9191' # libjxl/conformance, master of 2026-09-05
# the programs of this platform (paths with '/': Windows takes them too)
$platform = if ($IsLinux) { 'linux-x64' } else { 'win-x64' }
$exe = if ($IsLinux) { '' } else { '.exe' }

# photos
$photoDir = New-Item -ItemType Directory -Force -Path (Join-Path $data 'photos')
foreach ($p in 'cathedral.ppm', 'big_building.ppm') {
    $src = Join-Path $Photos $p
    if (-not (Test-Path -LiteralPath $src)) { throw "missing $src (the rgb8bit set of https://imagecompression.info/test_images/)" }
    Copy-Item -LiteralPath $src -Destination $photoDir -Force
}

# test images
$cjpegli = Join-Path $root "../ctJpeg/bin/$platform/cjpegli$exe"
if (-not (Test-Path -LiteralPath $cjpegli)) { throw "missing $cjpegli (the ctJpeg folder of this repository)" }
& (Join-Path $PSScriptRoot 'New-TestImages.ps1') -Out (Join-Path $data 'images') -Photos $photoDir.FullName `
    -Oracle (Join-Path $root "bin/$platform/cjxl$exe") -Cjpegli $cjpegli | Out-Null

# GIFs
$gifDir = New-Item -ItemType Directory -Force -Path (Join-Path $data 'gif')
foreach ($g in 'welcome2.gif', 'fire.gif', 'x-trans.gif', 'porsche.gif') {
    Copy-Item -LiteralPath (Join-Path $root "jxl/giflib-5.2.2/pic/$g") -Destination $gifDir -Force
}

# conformance set: one decoded reference image per test case
$conformance = Join-Path $data 'conformance'
if (-not (Test-Path -LiteralPath (Join-Path $conformance 'bike.png'))) {
    $zip = Join-Path $data "conformance-$commit.zip"
    if (-not (Test-Path -LiteralPath $zip)) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest "https://codeload.github.com/libjxl/conformance/zip/$commit" -OutFile $zip -UseBasicParsing
    }
    $unpacked = Join-Path $data 'conformance-repo'
    if (Test-Path -LiteralPath $unpacked) { Remove-Item -Recurse -Force -LiteralPath $unpacked }
    Expand-Archive -LiteralPath $zip -DestinationPath $unpacked
    $cases = Join-Path $unpacked "conformance-$commit/testcases"
    New-Item -ItemType Directory -Force -Path $conformance | Out-Null
    foreach ($case in Get-ChildItem -LiteralPath $cases -Directory | Sort-Object Name) {
        if ($case.Name -like '*_5') { continue } # links to the same files
        foreach ($n in 'ref.pfm', 'ref.png', 'ref.apng', 'ref.gif') {
            $f = Join-Path $case.FullName $n
            if ((Test-Path -LiteralPath $f) -and (Get-Item -LiteralPath $f).Length -gt 1KB) {
                Copy-Item -LiteralPath $f -Destination (Join-Path $conformance ($case.Name + [IO.Path]::GetExtension($n)))
                break
            }
        }
    }
    Copy-Item -LiteralPath (Join-Path $unpacked "conformance-$commit/LICENSE") (Join-Path $conformance 'LICENSE-conformance.txt')
    Remove-Item -Recurse -Force -LiteralPath $unpacked
}

# the animation (optional)
$animDir = New-Item -ItemType Directory -Force -Path (Join-Path $data 'animations')
$pan = Join-Path $animDir.FullName 'pan-640x360-30.gif'
$ffmpegPath = (Get-Command $Ffmpeg -ErrorAction SilentlyContinue).Source
if (-not (Test-Path -LiteralPath $pan) -and $ffmpegPath) {
    $building = Join-Path $photoDir.FullName 'big_building.ppm'
    # -t on the input side: the looped still is finite, so palettegen ends
    & $ffmpegPath -v error -y -loop 1 -framerate 30 -t 1 -i $building -vf 'crop=2560:1440:n*120:1500,scale=640:360:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse' -frames:v 30 -loop 0 $pan
}

# check every input against its hash
$bad = 0
$missing = 0
foreach ($row in Import-Csv (Join-Path $PSScriptRoot 'Input-Hashes.csv')) {
    $path = Join-Path $data ($row.Path.Replace('\', '/'))
    $optional = $row.Path -like 'animations*'
    if (-not (Test-Path -LiteralPath $path)) {
        if ($optional) { Write-Warning "$($row.Path): not made (needs ffmpeg); its verify case is skipped"; $missing++; continue }
        Write-Host "MISSING $($row.Path)"; $bad++; continue
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $row.Sha256) {
        if ($optional) {
            Write-Warning "$($row.Path): other bytes than expected (another ffmpeg version); removed, its verify case is skipped"
            Remove-Item -LiteralPath $path; $missing++; continue
        }
        Write-Host "DIFFERENT $($row.Path)"; $bad++
    }
}
if ($bad -gt 0) { throw "$bad input(s) missing or different" }
Write-Host "inputs in $data checked against Input-Hashes.csv$(if ($missing) { " ($missing optional input skipped)" })"
