#!/usr/bin/env bash
# Builds cjpegli-oracle (bin/linux-x64/cjpegli) on Linux: the unmodified jpegli of ../jpegli
# (google/jpegli @ 031a007) with its unmodified tools/cjpegli.cc, plus --target to choose the
# SIMD target. Needs clang (19 or later), CMake and Ninja. As the published program: no
# contraction of a*b+c into FMA outside Highway's own code (-ffp-contract=off, as MSVC on
# Windows), libstdc++ linked in.
# Output: build/linux-release/cjpegli-oracle
# Note: zlib's CMake renames ../jpegli/third_party/zlib/zconf.h to zconf.h.included, so git
# shows that file as changed after a build ("git checkout -- ." restores it).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
jpegli="$here/../jpegli"
# jpegli's CMake takes a bundled third_party library only if its folder has a .git entry (a git
# submodule); git does not keep these entries in this repository, so they are created here.
for d in highway lcms libjpeg-turbo libpng skcms zlib; do
    [ -e "$jpegli/third_party/$d/.git" ] || : >"$jpegli/third_party/$d/.git"
done
# AWK=OFF: the bundled libpng takes its prebuilt pnglibconf.h, as in the Windows build
cmake -S "$here" -B "$here/build/linux-release" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    "-DCMAKE_C_FLAGS=-ffp-contract=off" "-DCMAKE_CXX_FLAGS=-ffp-contract=off" \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc" -DAWK=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$here/build/linux-release" --target cjpegli-oracle
echo "built: $here/build/linux-release/cjpegli-oracle"
