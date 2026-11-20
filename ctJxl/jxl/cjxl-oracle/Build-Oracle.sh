#!/usr/bin/env bash
# Builds the reference cjxl on Linux (bin/linux-x64/cjxl): the unmodified libjxl 0.13.0 of ../libjxl-0.13.0 with its
# unmodified tools/cjxl_main.cc, plus --target to fix the SIMD target (oracle_main.cc), and its input decoders giflib
# 5.2.2 (../giflib-5.2.2, through giflib/CMakeLists.txt) and libjpeg-turbo 3.2.0 (../libjpeg-turbo-3.2.0) as static
# libraries. Needs clang (19 or later), CMake, Ninja and NASM. As the published program: no contraction of a*b+c into
# FMA outside Highway's own code (-ffp-contract=off, as MSVC on Windows), libstdc++ linked in.
# Output: build/linux-release/cjxl-oracle
# libjxl's configure writes into ../libjxl-0.13.0 (zlib renames third_party/zlib/zconf.h, libjxl copies libpng's
# pnglibconf.h); both are undone afterwards, also after an error.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build="$here/build"
vendor="$here/../libjxl-0.13.0"
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        "-DCMAKE_C_FLAGS=-ffp-contract=off" "-DCMAKE_CXX_FLAGS=-ffp-contract=off"
        "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc")
restore() {
    [ -f "$vendor/third_party/zlib/zconf.h.included" ] && mv -f "$vendor/third_party/zlib/zconf.h.included" "$vendor/third_party/zlib/zconf.h"
    rm -f "$vendor/third_party/libpng/pnglibconf.h"
}
trap restore EXIT
trap 'exit 130' INT TERM # through the EXIT trap
cmake -S "$here/giflib" -B "$build/giflib" "${common[@]}" "-DCMAKE_INSTALL_PREFIX=$build/deps" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$build/giflib" --target install
cmake -S "$here/../libjpeg-turbo-3.2.0" -B "$build/libjpeg-turbo" "${common[@]}" "-DCMAKE_INSTALL_PREFIX=$build/deps" \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DREQUIRE_SIMD=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$build/libjpeg-turbo" --target install
# AWK=OFF: the bundled libpng takes its prebuilt pnglibconf.h, as in the Windows build
cmake -S "$here" -B "$build/linux-release" "${common[@]}" "-DCMAKE_PREFIX_PATH=$build/deps" -DAWK=OFF
cmake --build "$build/linux-release" --target cjxl-oracle
echo "built: $build/linux-release/cjxl-oracle"
