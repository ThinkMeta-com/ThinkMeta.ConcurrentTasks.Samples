<#
.SYNOPSIS
    The test images of verify\Verify-Jxl.ps1, written to -Out (default testdata\images); verify\Prepare-Inputs.ps1
    calls it.
.DESCRIPTION
    Writes into -Out, keeping files that already exist. Every input is made by code in this script from two photos of
    the imagecompression.info set (-Photos: cathedral.ppm, big_building.ppm), or by a pinned program, so the bytes do
    not depend on the machine or the platform: PNG and APNG are written here (zlib with stored blocks), JPEG by cjpegli
    (jpegli of the ctJpeg folder, --target sse2), the JXL by the reference cjxl (--target avx2), the ICC profile is
    built here; the programs of Windows and Linux write the same bytes. Contents:
      crops of photos (P6 8 bit) in sizes around the encoder's path decisions (1x1 ... 2560x2304, 8 and 9 AC
      groups), the 800x600 crop as 16-bit PPM, float PFM, gray PGM and PGX, PAM with alpha;
      PNG: RGBA with transparent parts, RGBA fully opaque, a synthetic 'document' (few colors, repeated glyphs:
      palette and patches), a synthetic 'dots' image (isolated small dots); APNG with 4 frames;
      JPEG 4:2:0 and 4:4:4, JPEG with Exif and XMP segments; JXL;
      Exif (orientation 1 and 6), XMP, a JUMBF box, an RGB ICC profile (wide-gamut matrix/TRC, gamma 1.8).
#>
param(
    [string]$Out = (Join-Path $PSScriptRoot '../testdata/images'),
    # the programs of the platform; those of Windows and Linux write the same bytes
    [string]$Oracle = $(if ($IsLinux) { Join-Path $PSScriptRoot '../bin/linux-x64/cjxl' }
                        else { Join-Path $PSScriptRoot '..\bin\win-x64\cjxl.exe' }),
    [string]$Cjpegli = $(if ($IsLinux) { Join-Path $PSScriptRoot '../../ctJpeg/bin/linux-x64/cjpegli' }
                         else { Join-Path $PSScriptRoot '..\..\ctJpeg\bin\win-x64\cjpegli.exe' }),
    [string]$Photos = (Join-Path $PSScriptRoot '../testdata/photos') # cathedral.ppm and big_building.ppm
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null

function Read-Ppm([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $m = [regex]::Match([Text.Encoding]::ASCII.GetString($bytes, 0, 64), '^P6\s+(\d+)\s+(\d+)\s+(\d+)\s')
    [pscustomobject]@{ Bytes = $bytes; Width = [int]$m.Groups[1].Value; Height = [int]$m.Groups[2].Value; Offset = $m.Length }
}

function Write-IfMissing([string]$name, [scriptblock]$make) {
    $path = Join-Path $Out $name
    if (-not (Test-Path $path)) { & $make $path }
}

function New-Crop([string]$src, [string]$name, [int]$x0, [int]$y0, [int]$w, [int]$h) {
    Write-IfMissing $name {
        param($dst)
        $s = Read-Ppm $src
        if ($x0 + $w -gt $s.Width -or $y0 + $h -gt $s.Height) { throw "$name does not fit into $src" }
        $header = [Text.Encoding]::ASCII.GetBytes("P6`n$w $h`n255`n")
        $data = New-Object byte[] ($header.Length + $w * $h * 3)
        [Array]::Copy($header, $data, $header.Length)
        for ($y = 0; $y -lt $h; $y++) { [Array]::Copy($s.Bytes, $s.Offset + (($y0 + $y) * $s.Width + $x0) * 3, $data, $header.Length + $y * $w * 3, $w * 3) }
        [IO.File]::WriteAllBytes($dst, $data)
    }
}

# --- PNG writer: zlib with stored (uncompressed) deflate blocks, so the bytes never depend on a compressor
$crcTable = [long[]]::new(256)
for ($k = 0; $k -lt 256; $k++) { [long]$v = $k; for ($j = 0; $j -lt 8; $j++) { $v = if ($v -band 1) { 3988292384L -bxor ($v -shr 1) } else { $v -shr 1 } }; $crcTable[$k] = $v }
function Get-Crc([byte[]]$bytes) { [long]$v = 4294967295L; foreach ($b in $bytes) { $v = $crcTable[($v -bxor $b) -band 255] -bxor ($v -shr 8) }; $v -bxor 4294967295L }
function BE32([long]$v) { [byte[]]@((($v -shr 24) -band 255), (($v -shr 16) -band 255), (($v -shr 8) -band 255), ($v -band 255)) }
function BE16([int]$v) { [byte[]]@((($v -shr 8) -band 255), ($v -band 255)) }
function Get-Chunk([string]$type, [byte[]]$data) {
    $td = [byte[]]([Text.Encoding]::ASCII.GetBytes($type) + $data)
    [byte[]]((BE32 $data.Length) + $td + (BE32 (Get-Crc $td)))
}
function Get-StoredZlib([byte[]]$raw) {
    $ms = New-Object IO.MemoryStream
    $ms.WriteByte(0x78); $ms.WriteByte(0x01)
    $pos = 0
    do {
        $len = [Math]::Min(65535, $raw.Length - $pos)
        $last = if ($pos + $len -ge $raw.Length) { 1 } else { 0 }
        $ms.WriteByte($last); $ms.WriteByte($len -band 255); $ms.WriteByte($len -shr 8)
        $ms.WriteByte((-bnot $len) -band 255); $ms.WriteByte(((-bnot $len) -shr 8) -band 255)
        $ms.Write($raw, $pos, $len); $pos += $len
    } while ($pos -lt $raw.Length)
    [long]$a = 1; [long]$b = 0
    foreach ($x in $raw) { $a = ($a + $x) % 65521; $b = ($b + $a) % 65521 }
    $adler = BE32 (($b -shl 16) -bor $a); $ms.Write($adler, 0, 4)
    $ms.ToArray()
}
# pixels: RGB or RGBA bytes, rows top to bottom; filter 0 per row
function Get-PngRows([byte[]]$pixels, [int]$w, [int]$h, [int]$channels) {
    $raw = New-Object byte[] ($h * (1 + $w * $channels))
    for ($y = 0; $y -lt $h; $y++) { [Array]::Copy($pixels, $y * $w * $channels, $raw, $y * (1 + $w * $channels) + 1, $w * $channels) }
    $raw
}
function Get-Ihdr([int]$w, [int]$h, [int]$channels) { [byte[]]((BE32 $w) + (BE32 $h) + [byte[]](8, $(if ($channels -eq 4) { 6 } else { 2 }), 0, 0, 0)) }
function Write-Png([string]$path, [byte[]]$pixels, [int]$w, [int]$h, [int]$channels) {
    $sig = [byte[]](137, 80, 78, 71, 13, 10, 26, 10)
    $bytes = [byte[]]($sig + (Get-Chunk 'IHDR' (Get-Ihdr $w $h $channels)) + (Get-Chunk 'IDAT' (Get-StoredZlib (Get-PngRows $pixels $w $h $channels))) + (Get-Chunk 'IEND' ([byte[]]@())))
    [IO.File]::WriteAllBytes($path, $bytes)
}

# --- crops
$cathedral = Join-Path $Photos 'cathedral.ppm'
$building = Join-Path $Photos 'big_building.ppm'
New-Crop $cathedral 'crop-800x600.ppm' 0 0 800 600
New-Crop $cathedral 'crop-640x480.ppm' 300 400 640 480       # 6 AC groups: whole-frame path
New-Crop $cathedral 'crop-1024x512.ppm' 200 200 1024 512    # 8 AC groups: the last whole-frame size
New-Crop $cathedral 'crop-768x768.ppm' 200 200 768 768      # 9 AC groups: the first streamed size
New-Crop $cathedral 'crop-1024x768.ppm' 200 200 1024 768    # 12 AC groups, one DC group
New-Crop $building 'crop-2560x2304.ppm' 100 100 2560 2304   # 2x2 DC groups
New-Crop $cathedral 'crop-257x257.ppm' 500 500 257 257
New-Crop $cathedral 'crop-7x5.ppm' 1000 1000 7 5
New-Crop $cathedral 'crop-1x1.ppm' 1000 1000 1 1
New-Crop $cathedral 'crop-1x2100.ppm' 900 0 1 2100
New-Crop $cathedral 'crop-128x128.ppm' 700 900 128 128
$c = Read-Ppm (Join-Path $Out 'crop-800x600.ppm')
$w = $c.Width; $h = $c.Height; $n = $w * $h

# --- other formats of the 800x600 crop
Write-IfMissing 'crop-800x600-16bit.ppm' {
    param($path)
    $header = [Text.Encoding]::ASCII.GetBytes("P6`n$w $h`n65535`n")
    $data = New-Object byte[] ($header.Length + 6 * $n)
    [Array]::Copy($header, $data, $header.Length)
    for ($i = 0; $i -lt 3 * $n; $i++) { $data[$header.Length + 2 * $i] = $c.Bytes[$c.Offset + $i]; $data[$header.Length + 2 * $i + 1] = [byte](($i * 37) -band 255) }
    [IO.File]::WriteAllBytes($path, $data)
}
# 10- and 12-bit PNM (maxval 1023 / 4095): the decoders scale by the maxval, not by 8 or 16 bit (plan D1.6a)
function Write-DeepPnm([string]$path, [string]$magic, [int]$maxval, [int]$shift, [object]$src, [int]$channels) {
    $count = $src.Width * $src.Height
    $header = [Text.Encoding]::ASCII.GetBytes("$magic`n$($src.Width) $($src.Height)`n$maxval`n")
    $data = New-Object byte[] ($header.Length + 2 * $channels * $count)
    [Array]::Copy($header, $data, $header.Length)
    for ($i = 0; $i -lt $count; $i++) {
        for ($ch = 0; $ch -lt $channels; $ch++) {
            $v8 = if ($channels -eq 1) { [int][Math]::Round(0.299 * $src.Bytes[$src.Offset + 3 * $i] + 0.587 * $src.Bytes[$src.Offset + 3 * $i + 1] + 0.114 * $src.Bytes[$src.Offset + 3 * $i + 2]) } else { [int]$src.Bytes[$src.Offset + 3 * $i + $ch] }
            $v = ($v8 -shl $shift) -bor (($i * 7 + $ch) -band ((1 -shl $shift) - 1))
            $o = $header.Length + 2 * ($channels * $i + $ch)
            $data[$o] = [byte]($v -shr 8); $data[$o + 1] = [byte]($v -band 255)
        }
    }
    [IO.File]::WriteAllBytes($path, $data)
}
Write-IfMissing 'crop-800x600-10bit.ppm' { param($path); Write-DeepPnm $path 'P6' 1023 2 $c 3 }
Write-IfMissing 'crop-800x600-12bit.pgm' { param($path); Write-DeepPnm $path 'P5' 4095 4 $c 1 }
Write-IfMissing 'crop-2560x2304-10bit.ppm' { param($path); Write-DeepPnm $path 'P6' 1023 2 (Read-Ppm (Join-Path $Out 'crop-2560x2304.ppm')) 3 }
Write-IfMissing 'crop-800x600.pfm' {
    param($path)
    $fs = [IO.File]::Create($path); $bw = New-Object IO.BinaryWriter($fs)
    $bw.Write([Text.Encoding]::ASCII.GetBytes("PF`n$w $h`n-1.0`n"))
    for ($y = $h - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $w * 3; $x++) {
            $v = $c.Bytes[$c.Offset + $y * $w * 3 + $x] / 255.0
            if ($x -lt 60) { $v = $v * 1.5 }
            $bw.Write([single]$v)
        }
    }
    $bw.Dispose()
}
if (-not (Test-Path (Join-Path $Out 'crop-800x600-gray.pgm')) -or -not (Test-Path (Join-Path $Out 'crop-800x600-gray.pgx'))) {
    $gray = New-Object byte[] $n
    for ($i = 0; $i -lt $n; $i++) { $gray[$i] = [byte][Math]::Round(0.299 * $c.Bytes[$c.Offset + 3 * $i] + 0.587 * $c.Bytes[$c.Offset + 3 * $i + 1] + 0.114 * $c.Bytes[$c.Offset + 3 * $i + 2]) }
    Write-IfMissing 'crop-800x600-gray.pgm' { param($path); [IO.File]::WriteAllBytes($path, [byte[]]([Text.Encoding]::ASCII.GetBytes("P5`n$w $h`n255`n") + $gray)) }
    Write-IfMissing 'crop-800x600-gray.pgx' { param($path); [IO.File]::WriteAllBytes($path, [byte[]]([Text.Encoding]::ASCII.GetBytes("PG ML + 8 $w $h`n") + $gray)) }
}
Write-IfMissing 'crop-800x600-alpha.pam' {
    param($path)
    $header = [Text.Encoding]::ASCII.GetBytes("P7`nWIDTH $w`nHEIGHT $h`nDEPTH 4`nMAXVAL 255`nTUPLTYPE RGB_ALPHA`nENDHDR`n")
    $data = New-Object byte[] ($header.Length + 4 * $n)
    [Array]::Copy($header, $data, $header.Length)
    for ($i = 0; $i -lt $n; $i++) {
        $o = $header.Length + 4 * $i
        $data[$o] = $c.Bytes[$c.Offset + 3 * $i]; $data[$o + 1] = $c.Bytes[$c.Offset + 3 * $i + 1]; $data[$o + 2] = $c.Bytes[$c.Offset + 3 * $i + 2]
        $data[$o + 3] = [byte]((($i % $w) + [int]($i / $w)) * 255 / ($w + $h))
    }
    [IO.File]::WriteAllBytes($path, $data)
}
# RGBA over several DC groups (alpha as an extra channel in every DC group; ctJxl plan W8): the 2560x2304
# crop with the same diagonal alpha ramp, row by row
Write-IfMissing 'crop-2560x2304-alpha.pam' {
    param($path)
    $s = Read-Ppm (Join-Path $Out 'crop-2560x2304.ppm')
    $sw = $s.Width; $sh = $s.Height
    $header = [Text.Encoding]::ASCII.GetBytes("P7`nWIDTH $sw`nHEIGHT $sh`nDEPTH 4`nMAXVAL 255`nTUPLTYPE RGB_ALPHA`nENDHDR`n")
    $data = New-Object byte[] ($header.Length + 4 * $sw * $sh)
    [Array]::Copy($header, $data, $header.Length)
    for ($y = 0; $y -lt $sh; $y++) {
        $i = $s.Offset + $y * $sw * 3
        $o = $header.Length + $y * $sw * 4
        for ($x = 0; $x -lt $sw; $x++) {
            $data[$o] = $s.Bytes[$i]; $data[$o + 1] = $s.Bytes[$i + 1]; $data[$o + 2] = $s.Bytes[$i + 2]
            $data[$o + 3] = [byte](($x + $y) * 255 / ($sw + $sh))
            $i += 3; $o += 4
        }
    }
    [IO.File]::WriteAllBytes($path, $data)
}
# RGB plus a non-interleaved extra channel (Depth) over several DC groups: the same crop with a horizontal
# depth ramp (ctJxl plan W8)
Write-IfMissing 'crop-2560x2304-depth.pam' {
    param($path)
    $s = Read-Ppm (Join-Path $Out 'crop-2560x2304.ppm')
    $sw = $s.Width; $sh = $s.Height
    $header = [Text.Encoding]::ASCII.GetBytes("P7`nWIDTH $sw`nHEIGHT $sh`nDEPTH 4`nMAXVAL 255`nTUPLTYPE RGB`nTUPLTYPE Depth`nENDHDR`n")
    $data = New-Object byte[] ($header.Length + 4 * $sw * $sh)
    [Array]::Copy($header, $data, $header.Length)
    for ($y = 0; $y -lt $sh; $y++) {
        $i = $s.Offset + $y * $sw * 3
        $o = $header.Length + $y * $sw * 4
        for ($x = 0; $x -lt $sw; $x++) {
            $data[$o] = $s.Bytes[$i]; $data[$o + 1] = $s.Bytes[$i + 1]; $data[$o + 2] = $s.Bytes[$i + 2]
            $data[$o + 3] = [byte]($x * 255 / $sw)
            $i += 3; $o += 4
        }
    }
    [IO.File]::WriteAllBytes($path, $data)
}

# RGBA pixels from a section of the crop with an alpha function
function Get-Rgba([int]$bw, [int]$bh, [int]$dx, [int]$dy, [scriptblock]$alpha) {
    $px = New-Object byte[] ($bw * $bh * 4)
    for ($y = 0; $y -lt $bh; $y++) {
        for ($x = 0; $x -lt $bw; $x++) {
            $i = $c.Offset + (($y + $dy) * $w + ($x + $dx)) * 3
            $o = ($y * $bw + $x) * 4
            $px[$o] = $c.Bytes[$i]; $px[$o + 1] = $c.Bytes[$i + 1]; $px[$o + 2] = $c.Bytes[$i + 2]; $px[$o + 3] = [byte](& $alpha $x $y)
        }
    }
    $px
}
Write-IfMissing 'rgba-640x480.png' { param($path); Write-Png $path (Get-Rgba 640 480 80 60 { param($x, $y) if ($x -ge 400 -and $y -ge 300) { 0 } else { [int](255 * $x / 639) } }) 640 480 4 }
Write-IfMissing 'rgba-opaque-640x480.png' { param($path); Write-Png $path (Get-Rgba 640 480 80 60 { param($x, $y) 255 }) 640 480 4 }

# synthetic 'document': white page, a few colors, a small set of glyph-like 8x12 shapes repeated in lines
Write-IfMissing 'document-1024x768.png' {
    param($path)
    $dw = 1024; $dh = 768
    $px = New-Object byte[] ($dw * $dh * 3)
    for ($i = 0; $i -lt $px.Length; $i++) { $px[$i] = 255 }
    $glyphs = foreach ($g in 0..11) { , @(foreach ($r in 0..11) { (($g * 2654435761L + $r * 40503L) -shr 3) -band 0xFF }) }
    $ink = @(@(0, 0, 0), @(30, 30, 120), @(150, 20, 20))
    for ($line = 0; $line -lt 40; $line++) {
        $y0 = 20 + $line * 18
        $col = $ink[$line % 3]
        for ($ch = 0; $ch -lt 100; $ch++) {
            $x0 = 24 + $ch * 9
            $g = $glyphs[(($line * 7 + $ch * 3) % 12)]
            if ((($line * 31 + $ch * 17) % 9) -eq 0) { continue } # spaces
            for ($r = 0; $r -lt 12; $r++) { for ($b = 0; $b -lt 8; $b++) { if ($g[$r] -band (1 -shl $b)) { $o = (($y0 + $r) * $dw + $x0 + $b) * 3; $px[$o] = $col[0]; $px[$o + 1] = $col[1]; $px[$o + 2] = $col[2] } } }
        }
    }
    Write-Png $path $px $dw $dh 3
}

# synthetic 'dots': dark sky with isolated small bright dots (for --dots)
Write-IfMissing 'dots-800x600.png' {
    param($path)
    $px = New-Object byte[] ($n * 3)
    for ($i = 0; $i -lt $n; $i++) { $px[3 * $i] = 10; $px[3 * $i + 1] = 12; $px[3 * $i + 2] = 30 }
    for ($k = 0; $k -lt 400; $k++) {
        $cx = ($k * 7919) % 790 + 5; $cy = ($k * 104729) % 590 + 5; $amp = 120 + ($k * 37) % 135
        for ($dy = -3; $dy -le 3; $dy++) { for ($dx = -3; $dx -le 3; $dx++) {
            $v = $amp * [Math]::Exp(- ($dx * $dx + $dy * $dy) / 2.0)
            $o = (($cy + $dy) * $w + $cx + $dx) * 3
            for ($ch = 0; $ch -lt 3; $ch++) { $px[$o + $ch] = [byte][Math]::Min(255, $px[$o + $ch] + $v) }
        } }
    }
    Write-Png $path $px $w $h 3
}

# APNG: 4 frames of 320x240 (shifted sections), 10 fps
Write-IfMissing 'anim-320x240.png' {
    param($path)
    $ms = New-Object IO.MemoryStream
    $append = { param([byte[]]$b) $ms.Write($b, 0, $b.Length) }
    & $append ([byte[]](137, 80, 78, 71, 13, 10, 26, 10))
    & $append (Get-Chunk 'IHDR' (Get-Ihdr 320 240 4))
    & $append (Get-Chunk 'acTL' ([byte[]]((BE32 4) + (BE32 0))))
    $seq = 0
    for ($f = 0; $f -lt 4; $f++) {
        & $append (Get-Chunk 'fcTL' ([byte[]]((BE32 $seq) + (BE32 320) + (BE32 240) + (BE32 0) + (BE32 0) + (BE16 1) + (BE16 10) + [byte[]](0, 0))))
        $seq++
        $z = Get-StoredZlib (Get-PngRows (Get-Rgba 320 240 (40 * $f) (30 * $f) { param($x, $y) 255 }) 320 240 4)
        if ($f -eq 0) { & $append (Get-Chunk 'IDAT' $z) } else { & $append (Get-Chunk 'fdAT' ([byte[]]((BE32 $seq) + $z))); $seq++ }
    }
    & $append (Get-Chunk 'IEND' ([byte[]]@()))
    [IO.File]::WriteAllBytes($path, $ms.ToArray())
}

# --- metadata
$exif1 = [byte[]](0x49, 0x49, 0x2A, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0)
$exif6 = [byte[]]$exif1.Clone(); $exif6[18] = 6
$xmp = '<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>ctJxl test</dc:title></rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>'
Write-IfMissing 'meta.exif' { param($path); [IO.File]::WriteAllBytes($path, $exif1) }
Write-IfMissing 'meta-orient6.exif' { param($path); [IO.File]::WriteAllBytes($path, $exif6) }
Write-IfMissing 'meta.xmp' { param($path); [IO.File]::WriteAllText($path, $xmp) }
# JUMBF: a superbox 'jumb' content = description box 'jumd' (type UUID, toggles, label) and a 'json' content box
Write-IfMissing 'meta.jumbf' {
    param($path)
    $label = [byte[]]([Text.Encoding]::ASCII.GetBytes('ctjxl.test') + [byte[]](0))
    $jumd = [byte[]]((BE32 (8 + 16 + 1 + $label.Length)) + [Text.Encoding]::ASCII.GetBytes('jumd') + [byte[]](0x6A, 0x73, 0x6F, 0x6E, 0x00, 0x11, 0x00, 0x10, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71) + [byte[]](3) + $label)
    $json = [Text.Encoding]::ASCII.GetBytes('{"test":1}')
    $jsonBox = [byte[]]((BE32 (8 + $json.Length)) + [Text.Encoding]::ASCII.GetBytes('json') + $json)
    [IO.File]::WriteAllBytes($path, [byte[]]($jumd + $jsonBox))
}

# ICC v2 RGB display profile, built here: wide-gamut primaries, D50-adapted, gamma 1.8 (not an enum encoding)
Write-IfMissing 'display-rgb.icc' {
    param($path)
    function S15([double]$v) { BE32 ([long][Math]::Round($v * 65536) -band 0xFFFFFFFFL) }
    function Xyz([double]$x, [double]$y, [double]$z) { [byte[]]([Text.Encoding]::ASCII.GetBytes('XYZ ') + [byte[]](0, 0, 0, 0) + (S15 $x) + (S15 $y) + (S15 $z)) }
    $curv = [byte[]]([Text.Encoding]::ASCII.GetBytes('curv') + [byte[]](0, 0, 0, 0) + (BE32 1) + (BE16 ([int](1.8 * 256))) + [byte[]](0, 0))
    $descText = [byte[]]([Text.Encoding]::ASCII.GetBytes('ctJxl test RGB') + [byte[]](0))
    $desc = [byte[]]([Text.Encoding]::ASCII.GetBytes('desc') + [byte[]](0, 0, 0, 0) + (BE32 $descText.Length) + $descText + (New-Object byte[] 4) + (New-Object byte[] 4) + (New-Object byte[] 67))
    $cprt = [byte[]]([Text.Encoding]::ASCII.GetBytes('text') + [byte[]](0, 0, 0, 0) + [Text.Encoding]::ASCII.GetBytes('No copyright') + [byte[]](0))
    $tags = [ordered]@{
        'desc' = $desc; 'cprt' = $cprt; 'wtpt' = (Xyz 0.9642 1.0 0.8249)
        'rXYZ' = (Xyz 0.7161 0.2587 0.0); 'gXYZ' = (Xyz 0.1009 0.7247 0.0518); 'bXYZ' = (Xyz 0.1471 0.0166 0.7732)
        'rTRC' = $curv; 'gTRC' = $curv; 'bTRC' = $curv
    }
    $count = $tags.Count
    $offset = 128 + 4 + 12 * $count
    $table = [Collections.Generic.List[byte]]::new(); $data = [Collections.Generic.List[byte]]::new()
    $table.AddRange([byte[]](BE32 $count))
    foreach ($k in $tags.Keys) {
        $t = $tags[$k]
        while (($offset + $data.Count) % 4) { $data.Add(0) }
        $table.AddRange([byte[]]([Text.Encoding]::ASCII.GetBytes($k) + (BE32 ($offset + $data.Count)) + (BE32 $t.Length)))
        $data.AddRange([byte[]]$t)
    }
    while ($data.Count % 4) { $data.Add(0) }
    $size = 128 + $table.Count + $data.Count
    $header = New-Object byte[] 128
    [Array]::Copy((BE32 $size), 0, $header, 0, 4)
    [Array]::Copy([byte[]](2, 0x10, 0, 0), 0, $header, 8, 4)                               # version 2.1
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes('mntrRGB XYZ '), 0, $header, 12, 12)    # class, color space, PCS
    [Array]::Copy([byte[]]((BE16 2026) + (BE16 1) + (BE16 1) + (BE16 0) + (BE16 0) + (BE16 0)), 0, $header, 24, 12)
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes('acsp'), 0, $header, 36, 4)
    [Array]::Copy([byte[]]((S15 0.9642) + (S15 1.0) + (S15 0.8249)), 0, $header, 68, 12) # illuminant D50
    [IO.File]::WriteAllBytes($path, [byte[]]($header + $table.ToArray() + $data.ToArray()))
}

# --- JPEG by cjpegli (pinned, sse2): 4:2:0 and 4:4:4; one with Exif and XMP APP1 segments
Write-IfMissing 'crop-800x600.jpg' {
    param($path)
    $ErrorActionPreference = 'Continue' # Windows PowerShell 5.1: stderr text of a native program is no error here
    & $Cjpegli --target sse2 (Join-Path $Out 'crop-800x600.ppm') $path -q 90 --chroma_subsampling 420 2>$null
    if ($LASTEXITCODE) { throw 'cjpegli failed' }
}
Write-IfMissing 'crop-800x600-444.jpg' {
    param($path)
    $ErrorActionPreference = 'Continue' # Windows PowerShell 5.1: stderr text of a native program is no error here
    & $Cjpegli --target sse2 (Join-Path $Out 'crop-800x600.ppm') $path -q 90 --chroma_subsampling 444 2>$null
    if ($LASTEXITCODE) { throw 'cjpegli failed' }
}
Write-IfMissing 'crop-800x600-meta.jpg' {
    param($path)
    $jpg = [IO.File]::ReadAllBytes((Join-Path $Out 'crop-800x600.jpg'))
    $exifSeg = [byte[]]([Text.Encoding]::ASCII.GetBytes("Exif") + [byte[]](0, 0) + $exif6)
    $xmpSeg = [byte[]]([Text.Encoding]::ASCII.GetBytes('http://ns.adobe.com/xap/1.0/') + [byte[]](0) + [Text.Encoding]::UTF8.GetBytes($xmp))
    $app = [byte[]]([byte[]](0xFF, 0xE1) + (BE16 ($exifSeg.Length + 2)) + $exifSeg + [byte[]](0xFF, 0xE1) + (BE16 ($xmpSeg.Length + 2)) + $xmpSeg)
    [IO.File]::WriteAllBytes($path, [byte[]]($jpg[0..1] + $app + $jpg[2..($jpg.Length - 1)]))
}

# --- PNG variants for the chunked PNG input of ctJxl (plan D1.6b): bit depth, gray, palette with tRNS, iCCP,
#     a text chunk after the pixel data (falls back to the buffered decoder), many IDAT chunks over 2x2 DC groups
function Write-PngRaw([string]$path, [int]$w, [int]$h, [int]$depth, [int]$colorType, [byte[]]$raw,
                      [object[]]$before = @(), [object[]]$after = @(), [int]$idatSize = 0) {
    $ms = New-Object IO.MemoryStream
    $append = { param([byte[]]$b) $ms.Write($b, 0, $b.Length) }
    & $append ([byte[]](137, 80, 78, 71, 13, 10, 26, 10))
    & $append (Get-Chunk 'IHDR' ([byte[]]((BE32 $w) + (BE32 $h) + [byte[]]($depth, $colorType, 0, 0, 0))))
    foreach ($c in $before) { & $append $c }
    $z = Get-StoredZlib $raw
    if ($idatSize -le 0) { $idatSize = $z.Length }
    for ($p = 0; $p -lt $z.Length; $p += $idatSize) {
        $n = [Math]::Min($idatSize, $z.Length - $p)
        $part = New-Object byte[] $n; [Array]::Copy($z, $p, $part, 0, $n)
        & $append (Get-Chunk 'IDAT' $part)
    }
    foreach ($c in $after) { & $append $c }
    & $append (Get-Chunk 'IEND' ([byte[]]@()))
    [IO.File]::WriteAllBytes($path, $ms.ToArray())
}
# rows with filter byte 0 from $bytesPerRow bytes per row, produced by $pixel (row, column index of the byte)
function Get-RawRows([int]$h, [int]$bytesPerRow, [scriptblock]$fill) {
    $raw = New-Object byte[] ($h * (1 + $bytesPerRow))
    for ($y = 0; $y -lt $h; $y++) { & $fill $raw ($y * (1 + $bytesPerRow) + 1) $y }
    $raw
}
Write-IfMissing 'png-16bit-sbit-800x600.png' {
    param($path)
    $raw = Get-RawRows $h (6 * $w) { param($raw, $o, $y) for ($i = 0; $i -lt 3 * $w; $i++) { $v8 = $c.Bytes[$c.Offset + 3 * $w * $y + $i]; $raw[$o + 2 * $i] = $v8; $raw[$o + 2 * $i + 1] = [byte](($v8 -band 0xC0) -bor (($i * 13) -band 0x3F)) } }
    Write-PngRaw $path $w $h 16 2 $raw -before @((Get-Chunk 'sBIT' ([byte[]](10, 10, 10))))
}
Write-IfMissing 'png-gray-800x600.png' {
    param($path)
    $raw = Get-RawRows $h $w { param($raw, $o, $y) for ($x = 0; $x -lt $w; $x++) { $i = 3 * ($w * $y + $x); $raw[$o + $x] = [byte][Math]::Round(0.299 * $c.Bytes[$c.Offset + $i] + 0.587 * $c.Bytes[$c.Offset + $i + 1] + 0.114 * $c.Bytes[$c.Offset + $i + 2]) } }
    Write-PngRaw $path $w $h 8 0 $raw
}
Write-IfMissing 'png-palette-trns-320x240.png' {
    param($path)
    $plte = New-Object byte[] (3 * 256); for ($k = 0; $k -lt 256; $k++) { $plte[3 * $k] = [byte]$k; $plte[3 * $k + 1] = [byte]((255 - $k) -band 255); $plte[3 * $k + 2] = [byte](($k * 7) -band 255) }
    $trns = New-Object byte[] 64; for ($k = 0; $k -lt 64; $k++) { $trns[$k] = [byte]($k * 4) }
    $raw = Get-RawRows 240 320 { param($raw, $o, $y) for ($x = 0; $x -lt 320; $x++) { $raw[$o + $x] = [byte](($x + 3 * $y) -band 255) } }
    Write-PngRaw $path 320 240 8 3 $raw -before @((Get-Chunk 'PLTE' $plte), (Get-Chunk 'tRNS' $trns))
}
Write-IfMissing 'png-iccp-800x600.png' {
    param($path)
    $icc = [IO.File]::ReadAllBytes((Join-Path $Out 'display-rgb.icc'))
    $iccp = [byte[]]([Text.Encoding]::ASCII.GetBytes('display') + [byte[]](0, 0) + (Get-StoredZlib $icc))
    $raw = Get-RawRows $h (3 * $w) { param($raw, $o, $y) [Array]::Copy($c.Bytes, $c.Offset + 3 * $w * $y, $raw, $o, 3 * $w) }
    Write-PngRaw $path $w $h 8 2 $raw -before @((Get-Chunk 'iCCP' $iccp))
}
Write-IfMissing 'png-text-after-800x600.png' {
    param($path)
    $raw = Get-RawRows $h (3 * $w) { param($raw, $o, $y) [Array]::Copy($c.Bytes, $c.Offset + 3 * $w * $y, $raw, $o, 3 * $w) }
    Write-PngRaw $path $w $h 8 2 $raw -after @((Get-Chunk 'tEXt' ([byte[]]([Text.Encoding]::ASCII.GetBytes('Comment') + [byte[]](0) + [Text.Encoding]::ASCII.GetBytes('after the pixel data')))))
}
Write-IfMissing 'png-idats-2560x2304.png' {
    param($path)
    $b = Read-Ppm (Join-Path $Out 'crop-2560x2304.ppm')
    $raw = Get-RawRows $b.Height (3 * $b.Width) { param($raw, $o, $y) [Array]::Copy($b.Bytes, $b.Offset + 3 * $b.Width * $y, $raw, $o, 3 * $b.Width) }
    Write-PngRaw $path $b.Width $b.Height 8 2 $raw -idatSize 65536
}

# --- JXL input (cjxl re-encodes it): the 800x600 crop at -q 90 by the oracle, avx2
Write-IfMissing 'crop-800x600-q90.jxl' {
    param($path)
    $ErrorActionPreference = 'Continue' # Windows PowerShell 5.1: stderr text of a native program is no error here
    & $Oracle --target avx2 (Join-Path $Out 'crop-800x600.ppm') $path -q 90 --quiet
    if ($LASTEXITCODE) { throw 'oracle failed on the JXL input' }
}
Get-ChildItem $Out | Select-Object Name, Length
