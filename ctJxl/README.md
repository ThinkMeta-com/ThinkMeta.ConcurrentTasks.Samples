# ctJxl: libjxl's cjxl, bit for bit, one image on many cores

**ctJxl** is a JPEG XL encoder for Windows and Linux (x64) that writes *exactly* the same `.jxl` files as **cjxl** from
libjxl 0.13.0, byte for byte, for every option cjxl has, and spreads the encoding of a **single image** (or a single
animation) over all cores. It is libjxl itself, linked with eleven of its encoder files replaced by marked copies,
running on **ThinkMeta.ConcurrentTasks**, a fiber-based task scheduler.

ctJxl was created **AI-assisted**, using ThinkMeta.ConcurrentTasks: AI coding agents wrote the changes, measured and
tuned them under the direction of ThinkMeta's developers, an AI reviewer checked every step, and every step was
checked byte for byte against the reference. **An in-depth code review is still pending:** the output is verified to
be identical to the reference, but the changes have not yet been reviewed line by line by a human.

Status: **preliminary** (version 0.1.0), Windows and Linux (x64; on Windows a CPU with AVX2). The numbers below are
from Windows: the benchmark package on three machines and shorter development measurements.

## Try it

`bin\win-x64\` and `bin/linux-x64/` hold the programs, ready to run (no installation):

| | Windows (`bin\win-x64\`) | Linux (`bin/linux-x64/`) |
|---|---|---|
| Programs | `ctJxl.exe`, `cjxl.exe`, `ThinkMeta.ConcurrentTasks.Core.dll` | `ctJxl`, `cjxl`, `libThinkMeta.ConcurrentTasks.Core.so.2` |
| System | Windows 10 or 11, or Windows Server 2016 and later, x64; a CPU with AVX2, FMA and BMI1/2 | Linux x64 with **glibc 2.38** or later (Ubuntu 24.04, Debian 13, Fedora 39, RHEL 10 or newer), any x64 CPU |

```cmd
bin\win-x64\ctJxl.exe photo.png photo.jxl -q 90
bin\win-x64\cjxl.exe photo.png reference.jxl -q 90
fc /b photo.jxl reference.jxl
```

```sh
bin/linux-x64/ctJxl photo.png photo.jxl -q 90
bin/linux-x64/cjxl photo.png reference.jxl -q 90
cmp photo.jxl reference.jxl
```

`ctJxl` takes cjxl's command line. `cjxl` is the reference: the unmodified cjxl 0.13.0, built with the same compiler
from `jxl\` (see [The reference](#the-reference)). Both write the same file. If the Linux programs lost their executable
bit on the way (an archive unpacked on Windows, for example): `chmod +x bin/linux-x64/*`.

**Benchmark on your own images** (Windows): `benchmark\run.cmd D:\Images` runs ctJxl and cjxl on every image of a
folder (PPM, PGM, PAM, PFM, PNG, APNG, GIF, JPEG), image by image, the two programs alternating, each image read cold
(a fresh unbuffered copy before every run), 5 rounds; it reports the time per image and checks every output byte for
byte against cjxl's. `benchmark\README.txt` has the options (`-Cases "-q 90;-d 0"`, `-Rounds`, `-Threads`, ...).

## Benchmarks

The benchmark package on three machines (2026-11-19, Windows, the programs in `bin\win-x64\`): every image read cold
(a fresh unbuffered copy before every run), whole process wall time, 5 runs per image and program with best and worst
dropped, the mean of the other three; the factor is the sum of cjxl's times over the sum of ctJxl's. cjxl with
`--target avx2`. All 66 outputs bit-identical to cjxl on every machine.

| CPU | conformance set, `-q 90` | `-d 0` | 6 photos, `-q 90` | `-d 0` | 2 animations, `-q 90` | `-d 0` |
|---|---:|---:|---:|---:|---:|---:|
| Intel Core i9-11900K, 8 cores / 16 threads | **1.73×** | **2.13×** | **1.60×** | 1.20× | **2.15×** | **2.22×** |
| Intel Xeon E3-1275 v6, 4 cores / 8 threads | 1.63× | 1.72× | 1.61× | 1.13× | 1.83× | 1.63× |
| Intel Core Ultra 7 155H, 6P + 8E + 2 LP-E cores, 22 threads | 1.70× | 1.83× | 1.46× | 1.17× | 2.17× | 1.83× |

The sets: the 25 images of the JPEG XL conformance set (PNG, APNG, GIF, PFM; 0.01 to 11 megapixels), 6 photos of 3.4
to 39 megapixels (PPM, the images of the ctJpeg benchmark) and 2 animations of 30 frames (APNG 1280×720, GIF 640×360).
The photos gain 1.5 to 1.7× at `-q 90` on every machine (39 megapixels on the i9: cjxl 3.37 s, ctJxl 1.98 s), but only
1.1 to 1.3× lossless. The GIF animation gains the most: 7.2× (i9), 4.2× (Xeon) and 6.5× (155H) at `-d 0`, while the
larger APNG animation reaches 1.2 to 1.3× lossless.

Development measurements (Intel Core i9-11900K, 8 cores, 16 threads; whole process wall time with reading and
writing, file in the cache; mostly 5 runs with best and worst dropped, some cases the median of 3):

| Case | ctJxl vs cjxl |
|---|---:|
| Photo 6000×4000, `-q 90` (default effort 7) | 1.5× |
| PNG 6000×4000, `-q 90` | 1.5× |
| Photo, lossless `-d 0` | 1.15–1.2× |
| Photo, `-q 50` | 1.65× |
| 2560×2304, `-q 90 -e 9` | 1.9–2.0× |
| 1024×768, lossless `-e 10` | 4.4–4.7× |
| Photo, modular `-m 1 -q 80` | 3.2–3.5× |
| GIF animation 640×360, 30 frames, `-d 0` | 6.5–7.3× |
| JPEG transcoding, `-e 1` | 1.0× |
| The 25 images of the JPEG XL conformance set, `-q 90` / `-d 0` (sum, one run) | 1.7× / 2.1× |

*cjxl 0.13.0 built with the same compiler (MSVC, Visual Studio 2026) and its own thread pool on all 16 threads. ctJxl
is built with `/arch:AVX2`, cjxl with its own settings (its AVX2 code is chosen at run time); see [Building](#building).
Small images with a single DC group and lossless encoding gain nothing (about 1.0×). There are no Linux measurements
yet.*

cjxl is already parallel: libjxl splits its kernels into parallel regions. What remains serial are the gaps between
the regions, a few kernels without a pool, and the order of the work: one DC group, one tree, one frame after another.
ctJxl keeps libjxl's kernels and changes how the work is ordered.

## What ctJxl does differently

- **A runner on ConcurrentTasks.** libjxl's parallel regions (`RunOnPool`) run as worker tasks; the caller waits by
  suspending its task, so regions may run inside regions. libjxl's own pool forbids that. A task budget keeps the
  runnable tasks of nested regions within the scheduler's queue.
- **DC groups as tasks** (default lossy and lossless paths of large images): four group tasks with their own working
  state encode the 2048×2048 DC groups at the same time. A gate hands group 0's global state (quantization matrices,
  block context map) to the others; four turns keep what must stay in order in order: reading the input, the
  coefficient orders, the histograms, the output.
- **Reading overlaps encoding.** PNG is decoded row by row in its own task while the groups encode; PNM is read from a
  mapped file by the rows each group needs.
- **Kernels split further**, each bit-identical by construction: the block error loop of the AR heuristics, the
  connected components of the patch search (union-find in stripes), Butteraugli in tiles with borders (efforts 8 and 9),
  and in the modular path the MA tree (properties in parallel, subtrees as tasks), the tree data (precomputed per
  channel, inserted serially in the original order), LZ77 and histogram configurations per stream, squeeze and
  quantization per row.
- **Animation frames encoded ahead.** Once all frames are queued, ctJxl encodes the next frames in parallel, each into
  its own buffer, and appends them in order.
- **Less memory where it is free:** three buffers are released early or not made at all (photo `-q 90`: 855 → 710 MB).

How the task graph grew, step by step and with the measurements: [docs/task-graph.md](docs/task-graph.md).

## The reference

The reference is libjxl 0.13.0, unchanged, with its cjxl, built with MSVC (Visual Studio 2026), the static C runtime
and the input decoders of the official Windows release (giflib, libjpeg-turbo; no OpenEXR): `bin\win-x64\cjxl.exe`,
built by `jxl\cjxl-oracle\Build-Oracle.ps1`. It has one more option than cjxl, `--target sse2|avx2|...`, that fixes
libjxl's SIMD target; libjxl writes different bytes on different targets. ctJxl reproduces the AVX2 target, so compare
with `cjxl.exe --target avx2` (or without `--target` on an AVX2 CPU, where cjxl picks AVX2 itself).

On Linux the reference is the same libjxl and cjxl built with clang (`bin/linux-x64/cjxl`, built by
`jxl/cjxl-oracle/Build-Oracle.sh`, no FMA contraction outside libjxl's SIMD code), and ctJxl, like cjxl, chooses its
SIMD target at run time: on an AVX2 CPU both write the bytes of the AVX2 target. The two compilers do not round every
floating-point step alike, so for part of the cases (about a third of the test matrix) the Linux file differs from the
Windows file; each platform's ctJxl writes its own platform's cjxl file, and `verify\` has the tables of both.

The official cjxl release is built with clang-cl. For lossy encoding it writes other bytes of the same size
(floating point, compiler), and it is about 30 % faster than the same libjxl built with MSVC for lossy VarDCT (about
5–15 % for lossless and modular); the factors above compare programs from the same compiler.

## Command line

```cmd
ctJxl input.png output.jxl [cjxl options] [--pool ct|libjxl]
```

- Every cjxl 0.13.0 option works as in cjxl (`-d`, `-q`, `-e`, `-m`, `--num_threads`, `--buffering`, `--container`,
  `--streaming_input`, `--streaming_output`, ...); the output is the same file.
- On Windows ctJxl is an AVX2 program and writes the bytes of cjxl's AVX2 target. On a CPU without AVX2, FMA and
  BMI1/2 it stops at once with exit code 3. On Linux it chooses its SIMD target at run time (SSE2 to AVX2), as cjxl.
- `--pool libjxl` runs libjxl's own thread pool instead of ConcurrentTasks (for comparisons; the task graph is then
  off and ctJxl encodes as cjxl does).

## Building

Visual Studio 2026 with the C++ tools, x64, and [NASM](https://www.nasm.us) on the PATH (libjpeg-turbo's SIMD code,
for JPEG input). Clone the repository with its submodule (see the [repository README](../README.md)), then:

```cmd
msbuild ctJxl.slnx /p:Configuration=Release /p:Platform=x64 -m
```

The program: `build\Release\bin\ctJxl.exe` (with the scheduler core from the API submodule next to it).

- `jxl\libjxl-0.13.0\` holds libjxl unchanged apart from marked additions to a few headers (`ctJxl:`; the reference
  builds and computes unchanged with them). `jxl\libjxl-0.13.0\msvc\` has Visual Studio projects generated from
  libjxl's CMake build, with its compiler options (the same floating-point semantics as the reference), plus
  `/arch:AVX2` for the whole program: without it MSVC compiles the code around libjxl's AVX2 intrinsics as legacy SSE;
  with it the default `-q 90` case is about 15 % faster and stays bit-identical to the reference's AVX2 target. With
  MSVC, libjxl's SIMD library (Highway) then builds only the AVX2 target, so ctJxl has no SSE2 variant.
- `dct-inl.h` has one more marked change: the small DCTs (8×8 and smaller) are flattened into one function each,
  which MSVC otherwise leaves as many small functions (5–7 % on `-q 90`); the projects enable it, the reference's
  CMake build does not.
- `bin\win-x64\ctJxl.exe` is built the same way, plus the static C runtime, whole program optimization over libjxl and
  profile-guided optimization trained on test images (1–8 % faster). Your build is not that file byte for byte, but its
  output must be the one of cjxl.
- **No fused multiply-add beyond the reference.** FMA rounds once where a multiply and an add round twice, so the
  bytes would change. libjxl's AVX2 kernels use FMA as in the reference; everywhere else the compiler must not fuse
  (MSVC `/fp:precise`, no `/fp:contract`), and the one scalar function libjxl runs on its static SIMD target (the log2
  of the LZ77 cost estimate) is computed without FMA, as the reference does. Performance may therefore not be optimal.

The reference: `jxl\cjxl-oracle\Build-Oracle.ps1` builds `cjxl-oracle.exe` with CMake and Visual Studio 2026 (it is
`bin\win-x64\cjxl.exe`).

### Building on Linux

Requirements: **clang 19 or later** (tested with clang 20), **CMake 3.28 or later**, **Ninja** and **NASM**; on Ubuntu
24.04 `sudo apt install cmake ninja-build nasm` and a clang from [apt.llvm.org](https://apt.llvm.org) (the
distribution's clang 18 is too old).

```sh
git clone --recurse-submodules https://github.com/ThinkMeta-com/ThinkMeta.ConcurrentTasks.Samples.git
cd ThinkMeta.ConcurrentTasks.Samples/ctJxl
cmake -S . -B build/linux-release -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/linux-release
```

The output: `build/linux-release/bin/ctJxl` (it finds the scheduler core in the API submodule,
`../ThinkMeta.ConcurrentTasks.API/Core/bin/linux-x64`). `CMakeLists.txt` builds libjxl through its own CMake with the
eleven files of `src/Encoder/` in place of libjxl's, and the input decoders (zlib, libpng, giflib, libjpeg-turbo) from
the sources in `jxl/`, as the Visual Studio projects do; nothing is written into `jxl/`. There is no `-march`: ctJxl
picks its SIMD target at run time, as cjxl. The unit tests (`src/Tests`) are Windows only. `bin/linux-x64/ctJxl` links
libstdc++ statically and finds the core next to itself (`-DCT_SAMPLE_PUBLISH=ON` builds it that way); no profile-guided
optimization on Linux. Here, too, your build is not byte for byte that file, but its output must be the one of cjxl.

The reference on Linux: `bash jxl/cjxl-oracle/Build-Oracle.sh` builds `jxl/cjxl-oracle/build/linux-release/cjxl-oracle`
(it is `bin/linux-x64/cjxl`), with giflib and libjpeg-turbo from `jxl/` as static libraries.

### Verifying

`verify\` checks a ctJxl build against cjxl, on Windows and Linux. By default the scripts take your build
(`build\Release\bin\` on Windows, `build/linux-release/bin/` on Linux); `-Exe bin\win-x64\ctJxl.exe` or
`-Exe bin/linux-x64/ctJxl` checks the published one. On Linux run them with `pwsh`
([PowerShell 7 for Linux](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-linux)).

1. **Inputs.** Download the 8-bit RGB set of [imagecompression.info](https://imagecompression.info/test_images/)
   (`rgb8bit`) and unpack it; `cathedral.ppm` and `big_building.ppm` are used. Then:
   ```cmd
   powershell -ExecutionPolicy Bypass -File verify\Prepare-Inputs.ps1 -Photos D:\Downloads\rgb8bit
   ```
   ```sh
   pwsh verify/Prepare-Inputs.ps1 -Photos ~/Downloads/rgb8bit
   ```
   It makes the test images in `testdata\` (crops in sizes around the encoder's path decisions, 16 bit, float, gray,
   alpha, PNG and APNG variants, JPEG, metadata; JPEG with `cjpegli` of [`..\ctJpeg`](../ctJpeg)), copies four GIFs
   of giflib, downloads the JPEG XL conformance set at a fixed commit, makes a GIF animation with ffmpeg if it is
   installed, and checks every input against `verify\Input-Hashes.csv`. Windows and Linux make the same inputs.
2. **Bit compare** (needs PowerShell 7, `pwsh`, and an AVX2 CPU): every output must have the SHA-256 that
   `cjxl --target avx2` of the same platform gave (`verify\Reference-Hashes-<Matrix>-avx2-win-x64.csv`,
   `-linux-x64.csv`; the script takes the one of the platform it runs on).
   ```cmd
   pwsh verify\Verify-Jxl.ps1 -Matrix Quick
   pwsh verify\Verify-Jxl.ps1 -Matrix Conformance
   ```
   ```sh
   pwsh verify/Verify-Jxl.ps1 -Matrix Quick -Exe bin/linux-x64/ctJxl
   ```
   `Quick` runs 232 cases over every path and option of the encoder, `Conformance` 50 (the conformance images at
   `-q 90` and `-d 0`), `-Matrix Smoke` a short subset for other builds (`-Exe` of a Debug build),
   `-ExtraArgs '--pool libjxl'` libjxl's own pool, `-Repeat 3` every case three times (output that depends on
   thread timing). The Verify scripts of all samples take the same parameters: `-Mode Compare` (default) or
   `Reference`, `-Matrix`, `-Exe`, `-Oracle`, `-HashFile`, `-ExtraArgs`, `-Repeat`, `-ThrottleLimit`, `-WorkDir`,
   `-TimeoutSec`.
3. **The task graph in other orders:** `src\Tests` (`GraphOrderTests`) runs the graph on **one** scheduler thread in
   other orders: worker tasks started reversed or shuffled, a yield after each claimed item, every second (or a
   random) item with high priority, so a later item overtakes the one before; the DC groups yield after each turn,
   the PNG reader after each band; and with the task budget used up. Every encoding must equal the serial encoder's
   file. Set `CTJXL_TEST_IMAGES` to `testdata\images`; without it the tests are skipped.

## Repository contents

| Folder / file | Contents |
|---|---|
| `src\` | ctJxl: `App\` (the program, with a marked copy of cjxl's `cjxl_main.cc` and of libjxl's PNG decoder that reads rows on demand), `Encoder\` (the eleven replaced libjxl encoder files), `Tasks\` (the ConcurrentTasks side: runner, `GroupSync`, `InputBands`, `TaskPool`, behind a C++17 interface, so libjxl's code needs no C++ modules), `Tests\`; the code all samples share is in [`..\Shared`](../Shared/README.md) |
| `ctJxl.slnx` | Builds ctJxl, its tests and libjxl with Visual Studio 2026 |
| `CMakeLists.txt` | The same on Linux with clang, CMake and Ninja (without the tests); see [Building on Linux](#building-on-linux) |
| `ThinkMeta.SolutionFiles\` | Shared build settings (from the API submodule) and the `.clang-format` of the sources |
| `jxl\libjxl-0.13.0\` | libjxl 0.13.0 (unchanged apart from marked header additions) with its bundled third-party libraries, and `msvc\` with the generated Visual Studio projects |
| `jxl\giflib-5.2.2\`, `jxl\libjpeg-turbo-3.2.0\` | The GIF and JPEG decoders of cjxl, unchanged |
| `jxl\cjxl-oracle\` | The wrapper that builds the reference cjxl with `--target` (CMake), `Build-Oracle.ps1` (Windows) and `Build-Oracle.sh` (Linux) |
| `bin\win-x64\` | `ctJxl.exe`, `cjxl.exe` (the reference) and `ThinkMeta.ConcurrentTasks.Core.dll` (the scheduler core), all with the static C runtime |
| `bin/linux-x64/` | Linux: `ctJxl`, `cjxl` and the core as `libThinkMeta.ConcurrentTasks.Core.so.2`, libstdc++ linked in |
| `benchmark\` | `Compare-Folder.ps1` and `run.cmd`: speed and bit identity on your own images (Windows) |
| `verify\` | `Prepare-Inputs.ps1`, `New-TestImages.ps1`, `Verify-Jxl.ps1` and the reference hashes |
| `docs\` | [How the task graph grew](docs/task-graph.md) |
| `COPYING`, `LICENSE-Core.txt`, `LICENSE.txt`, `THIRD-PARTY-NOTICES.txt` | the licenses, see [License](#license) |

## Reading the code

Every change to libjxl's files is marked `ctJxl:` and names the development step it came from (e.g. `D1.5`); the rest
of each file is the original. Read it in this order:

1. **`src\App\Application.cpp`**: ctJxl's own options (`--pool`), then cjxl's main (`CjxlMain.cc`) with a runner
   factory: `TaskPool` (`src\Tasks\TaskPool.cpp`) creates the scheduler and the `ParallelRunner` once cjxl knows its
   thread count.
2. **`src\Tasks\ParallelRunner.cpp`**: libjxl's `JxlParallelRunner` on tasks. A region forks worker tasks that claim
   the items in ascending order from a shared counter; the caller is worker 0 and waits in a `TaskGroup_s`.
3. **`src\Encoder\enc_frame.cc`**, `EncodeFrameStreaming`: the DC groups as tasks. Each task holds one working state
   and takes the next group from a counter; `GroupSteps` and `src\Tasks\GroupSync.cpp` (turns and gate) keep the order
   where the bytes depend on it.
4. **`src\Tasks\InputBands.cpp`** with `src\App\ctjxl_apng.cc` and `CjxlMain.cc`: the PNG reader task and the bands the
   encoder waits for.
5. **`src\Encoder\enc_ma.cc`, `enc_encoding.cc`, `enc_ans.cc`, `enc_lz77.cc`**: the modular path (tree, tree data,
   histograms, LZ77), each split so that the result is the serial one.
6. **`src\Encoder\encode.cc`**: animation frames encoded ahead.

## Room for improvement

- The EPF passes of a DC group as one region each; the number of group tasks coupled to cores and free memory; an
  earlier gate for machines with 32 or more cores, where one DC group no longer fills the workers.
- The serial insertion of the MA tree data (order-dependent deduplication); histogram building and the symbol cost
  estimator (one serial pass over all tokens each); the dynamic programming of optimal LZ77 per stream; the middle
  nodes of the tree at `-e 10`; the background search of the patch finder.
- JPEG transcoding stays at cjxl's speed: the Huffman decoding of a JPEG without restart markers is serial. Overlapping
  decoding and encoding (baseline JPEGs with one scan) or decoding in parallel at restart markers would help.
- Fast lossless (`-e 1`): skip the copy of PNM input. Small lossless images with one DC group gain nothing yet.
- Animation: the input (APNG, GIF) is decoded before encoding starts; the window of frames encoded ahead (16
  megapixels) could follow the free memory. Peak memory of an APNG `-d 0`: 530 MB against cjxl's 220 MB.
- Memory in general: four DC-group working states and Butteraugli tiles cost memory (`-q 90`: 710 MB against 476 MB;
  `-e 9` on 2560×2304: 2.2 GB against 1.5 GB).
- MSVC against clang-cl: the rest of the gap is MSVC's code for the larger DCTs and the entropy estimate of the AC
  strategy search.
- Linux: no measurements yet, and the benchmark script is Windows only. The Linux programs need glibc 2.38 because
  libstdc++ is linked in from Ubuntu 24.04; a build against an older glibc would let them run on older distributions.

## License

ctJxl's own code: MIT (`LICENSE.txt`). libjxl: BSD-3-Clause, with a patent grant (`COPYING`), including the replaced
files in `src\Encoder\` and the two marked copies in `src\App\`, which keep their headers. The licenses of the other
libraries in `jxl\` and in the programs: `THIRD-PARTY-NOTICES.txt`. The ConcurrentTasks core DLL is proprietary
(non-commercial use; benchmarks explicitly allowed, see `LICENSE-Core.txt`). Copyright ThinkMeta Software GmbH.
