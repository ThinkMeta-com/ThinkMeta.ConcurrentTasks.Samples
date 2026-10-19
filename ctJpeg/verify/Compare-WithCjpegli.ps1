# Copyright (c) 2026 ThinkMeta Software GmbH
# SPDX-License-Identifier: MIT (see LICENSE.txt)
<#
.SYNOPSIS
    Checks ctJpeg against cjpegli on your own images: every output must be byte for byte the
    output of cjpegli on the same SIMD target.

.DESCRIPTION
    Encodes every PPM/PGM image of the folder with bin\cjpegli.exe --target <simd> and with the
    ctJpeg build (-Exe), for every quality and subsampling given, and compares the files. Nothing
    is timed. Windows PowerShell 5.1 or PowerShell 7. Exit code 0 if every output is identical,
    2 if one differs or a program failed, 1 for other errors.

.EXAMPLE
    .\Compare-WithCjpegli.ps1 D:\Images
    .\Compare-WithCjpegli.ps1 D:\Images -Simd sse2 -Qualities '50,85,95' -Subsampling 'default,420' -Exe ..\bin\ctJpeg.exe
#>
param(
    [Parameter(Mandatory, Position = 0)][string]$ImageDir,
    [ValidateSet('sse2', 'avx2')][string]$Simd = 'avx2',
    [string]$Qualities = '50,75,85,95',       # a string: powershell -File passes no arrays
    [string]$Subsampling = 'default,444,420',
    [string]$ExtraArgs = '',                  # for ctJpeg only, e.g. '--threads 4' or '--sequential'
    [string]$Exe = '',
    [string]$Cjpegli = ''
)

$ErrorActionPreference = 'Stop'
# defaults here, not in param(): $PSScriptRoot may be empty there under Windows PowerShell 5.1
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot '..\build\Release\bin\ctJpeg.exe' }
if (-not $Cjpegli) { $Cjpegli = Join-Path $PSScriptRoot '..\bin\cjpegli.exe' }
foreach ($p in $Exe, $Cjpegli) { if (-not (Test-Path -LiteralPath $p)) { Write-Host "not found: $p" -ForegroundColor Red; exit 1 } }
$images = @(Get-ChildItem -LiteralPath $ImageDir -File | Where-Object { $_.Extension -match '^\.(ppm|pgm|pnm)$' } | Sort-Object Name)
if ($images.Count -eq 0) { Write-Host "no PPM/PGM images in $ImageDir" -ForegroundColor Red; exit 1 }
$work = Join-Path ([IO.Path]::GetTempPath()) ('ctJpeg-compare-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Invoke-Program([string]$exe, [string[]]$arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $null = $p.StandardOutput.ReadToEndAsync()
    $null = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return $p.ExitCode
}
$sha = [System.Security.Cryptography.SHA256]::Create()
function Get-Sha256([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try { return [BitConverter]::ToString($sha.ComputeHash($fs)) } finally { $fs.Dispose() }
}

$cases = 0
$failed = 0
try {
    foreach ($img in $images) {
        foreach ($q in ($Qualities -split '[,\s]+' | Where-Object { $_ })) {
            foreach ($sub in ($Subsampling -split '[,\s]+' | Where-Object { $_ })) {
                $opts = @('-q', $q) + @(if ($sub -ne 'default') { '--chroma_subsampling', $sub })
                $ref = Join-Path $work 'ref.jpg'
                $out = Join-Path $work 'ct.jpg'
                foreach ($f in $ref, $out) { if (Test-Path -LiteralPath $f) { [IO.File]::Delete($f) } }
                $cases++
                $codeRef = Invoke-Program $Cjpegli (@('--target', $Simd, $img.FullName, $ref) + $opts)
                $codeCt = Invoke-Program $Exe (@($img.FullName, $out) + $opts + @('--simd', $Simd, '--quiet') + @($ExtraArgs -split ' ' | Where-Object { $_ }))
                $problem = ''
                if ($codeRef -ne $codeCt) { $problem = "exit code cjpegli $codeRef, ctJpeg $codeCt" }
                elseif ($codeRef -eq 0 -and (Get-Sha256 $ref) -ne (Get-Sha256 $out)) { $problem = 'output differs' }
                if ($problem) { $failed++; Write-Host ("{0} {1}: {2}" -f $img.Name, ($opts -join ' '), $problem) -ForegroundColor Red }
            }
        }
    }
}
finally {
    Remove-Item -Recurse -Force -LiteralPath $work -ErrorAction SilentlyContinue
}
if ($failed -gt 0) { Write-Host "$failed of $cases cases differ from cjpegli on $Simd" -ForegroundColor Red; exit 2 }
Write-Host "all $cases cases identical to cjpegli on $Simd ($($images.Count) images)" -ForegroundColor Green
exit 0
