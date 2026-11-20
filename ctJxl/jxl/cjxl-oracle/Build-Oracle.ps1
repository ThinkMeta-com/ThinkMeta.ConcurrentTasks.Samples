<#
.SYNOPSIS
    Builds the reference cjxl (bin\win-x64\cjxl.exe): unmodified libjxl 0.13.0 and its cjxl, MSVC, static C runtime,
    Release, no AVX-512, with the input decoders of the official libjxl Windows release (giflib, libjpeg-turbo).

.DESCRIPTION
    cjxl-oracle.exe is cjxl with one more option, --target <emu128|scalar|sse2|ssse3|sse4|avx2>, that fixes libjxl's
    SIMD target (the bytes depend on it); without it, it picks the best target the CPU has, as cjxl does. It links the
    unchanged libjxl of ..\libjxl-0.13.0 and its unchanged tools\cjxl_main.cc (oracle_main.cc only adds --target).

    Needs Visual Studio 2026 with the C++ tools and CMake, and NASM (https://www.nasm.us) on the PATH or as -Nasm.
    Output: build\cjxl\Release\cjxl-oracle.exe. Windows PowerShell 5.1 or later.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File jxl\cjxl-oracle\Build-Oracle.ps1
#>
param(
    [string]$Nasm = 'nasm.exe',
    [string]$CMake = ''
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$jxl = Split-Path $here
if (-not $CMake) {
    $CMake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $CMake)) { $CMake = 'cmake.exe' }
}
$nasmPath = (Get-Command $Nasm -ErrorAction SilentlyContinue).Source
if (-not $nasmPath) { throw "NASM not found ($Nasm): install it from https://www.nasm.us or pass -Nasm" }
$build = Join-Path $here 'build'
$install = Join-Path $build 'deps'
$vendor = Join-Path $jxl 'libjxl-0.13.0'

function Invoke-CMake([string[]]$arguments) {
    & $CMake @arguments
    if ($LASTEXITCODE) { throw "cmake $($arguments -join ' ') failed" }
}

try {
    # giflib and libjpeg-turbo: static, static C runtime (libjpeg-turbo with its NASM SIMD code)
    Invoke-CMake @('-S', (Join-Path $here 'giflib'), '-B', (Join-Path $build 'giflib'), '-G', 'Visual Studio 18 2026', '-A', 'x64',
        "-DCMAKE_INSTALL_PREFIX=$install")
    Invoke-CMake @('--build', (Join-Path $build 'giflib'), '--config', 'Release', '--target', 'install', '--', '-m', '-v:m', '-nologo')
    Invoke-CMake @('-S', (Join-Path $jxl 'libjpeg-turbo-3.2.0'), '-B', (Join-Path $build 'libjpeg-turbo'), '-G', 'Visual Studio 18 2026',
        '-A', 'x64', "-DCMAKE_INSTALL_PREFIX=$install", '-DENABLE_SHARED=OFF', '-DENABLE_STATIC=ON', '-DWITH_CRT_DLL=OFF',
        '-DWITH_TURBOJPEG=OFF', '-DWITH_TOOLS=OFF', "-DCMAKE_ASM_NASM_COMPILER=$nasmPath")
    Invoke-CMake @('--build', (Join-Path $build 'libjpeg-turbo'), '--config', 'Release', '--target', 'install', '--', '-m', '-v:m', '-nologo')
    # libjxl and cjxl-oracle
    Invoke-CMake @('-S', $here, '-B', (Join-Path $build 'cjxl'), '-G', 'Visual Studio 18 2026', '-A', 'x64',
        "-DCMAKE_PREFIX_PATH=$($install -replace '\\', '/')")
    Invoke-CMake @('--build', (Join-Path $build 'cjxl'), '--config', 'Release', '--target', 'cjxl-oracle', '--', '-m', '-v:m', '-nologo')
} finally {
    # The configure step writes into the source tree: zlib's CMake renames zconf.h to zconf.h.included, libjxl's
    # third_party/CMakeLists.txt copies libpng's pnglibconf.h. Undo both, also after an error.
    $zlib = Join-Path $vendor 'third_party\zlib'
    if (Test-Path (Join-Path $zlib 'zconf.h.included')) { Move-Item -Force (Join-Path $zlib 'zconf.h.included') (Join-Path $zlib 'zconf.h') }
    $pngConf = Join-Path $vendor 'third_party\libpng\pnglibconf.h'
    if (Test-Path $pngConf) { Remove-Item -Force $pngConf }
}
Get-Item (Join-Path $build 'cjxl\Release\cjxl-oracle.exe') | Select-Object FullName, Length
