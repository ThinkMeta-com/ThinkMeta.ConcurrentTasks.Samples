# ctDeflate: gzip, bit for bit, one file on many cores

**ctDeflate** is a gzip compressor for Windows x64 that writes *exactly* the same `.gz` files as `gzip` from **libdeflate 1.26** (compression levels 1 to 9), byte for byte, and spreads the compression of a **single file** over all cores. It is a new C++ port (rewritten, not linked) of libdeflate's compressor, built on **ThinkMeta.ConcurrentTasks**, a fiber-based task scheduler for Windows.

ctDeflate was created **AI-assisted**, using ThinkMeta.ConcurrentTasks: AI coding agents wrote the port, its parallelization and optimizations, measured and tuned the code under the direction of ThinkMeta's developers, and every step was checked byte for byte against the reference. **An in-depth code review is still pending:** the output is verified to be identical to the reference, but the code itself has not yet been reviewed line by line.

| i9-11900K, 16 threads | enwik9 (1 GB, one file) | | | Silesia corpus (12 files, 212 MB) | | |
|---|---:|---:|---:|---:|---:|---:|
| | gzip.exe | **ctDeflate** | factor | gzip.exe | **ctDeflate** | factor |
| level 1 | 200 MB/s | **833 MB/s** | **4.2×** | 122 MB/s | **193 MB/s** | **1.6×** |
| level 6 (default) | 89 MB/s | **366 MB/s** | **4.1×** | 78 MB/s | **135 MB/s** | **1.7×** |
| level 9 | 58 MB/s | **296 MB/s** | **5.1×** | 38 MB/s | **109 MB/s** | **2.9×** |

*Wall time of the whole program run including reading the input cold from the disk and writing the `.gz`, median of 3, every output bit-identical to `gzip.exe` (the official libdeflate 1.26 build). Details, all files and more CPUs under [Benchmarks](#benchmarks).*

---

## Repository contents

| Folder / file | Contents |
|---|---|
| `bin\` | `ctDeflate.exe` (the compressor), `gzip.exe` (the reference: the official libdeflate 1.26 Windows x64 release build), `ThinkMeta.ConcurrentTasks.Core.dll` (scheduler core) |
| `src\` | The source code of ctDeflate: the engine (the port of libdeflate's compressor), the task graph, the program and the unit tests |
| `ctDeflate.slnx` | Builds ctDeflate, its unit tests and `gzip.exe` with Visual Studio 2026 (see [Building](#building)) |
| `verify\` | `Verify-Deflate.ps1` and its reference hashes: checks a ctDeflate.exe byte for byte against `gzip.exe` |
| `libdeflate\` | The libdeflate 1.26 sources with Visual Studio projects for the library and `gzip.exe` |
| `props\` | `StaticCrt.props`: the build with the static C runtime, as for `bin\ctDeflate.exe` |
| `benchmark\` | `Compare-Gzip.ps1` and `run.cmd`: speed and bit identity on your own files |
| `New-BenchPackage.ps1` | Packs `bin\` and the benchmark into a zip for other PCs |

The scheduler's API (headers, C++ modules, the core) comes from the submodule `ThinkMeta.ConcurrentTasks.API` at the root of this repository.

More on the reference under [The reference](#the-reference).

---

## Try it

No installation and no runtime to install: `ctDeflate.exe` is linked with the static C runtime and needs nothing but Windows and the scheduler core next to it.

| | |
|---|---|
| System | Windows 10 or 11, or Windows Server 2016 or later, x64 |
| CPU | x64 with AVX2, FMA, BMI2 and PCLMUL (Intel since Haswell, 2013; AMD since Excavator and all Zen). ctDeflate checks this at start, before any of its other code runs; on an older CPU it names the missing instruction sets and exits with code 3 |
| `gzip.exe` | the official libdeflate build uses the Universal C Runtime, which is part of Windows 10 and Server 2016 and later |

```cmd
copy input.bin a.bin
copy input.bin b.bin
bin\gzip -6 a.bin
bin\ctDeflate -6 b.bin
fc /b a.bin.gz b.bin.gz
```

`fc /b` finds no differences. (`gzip` replaces `a.bin` by `a.bin.gz`; `ctDeflate` writes `b.bin.gz` and keeps `b.bin`. All options under [Command line](#command-line).)

### Benchmark on your own files

```cmd
benchmark\run.cmd "D:\Data\big.bin" "D:\Logs"
benchmark\run.cmd "D:\Logs" -Recurse -Levels 1,6,9 -Rounds 5
```

The script compresses every file with gzip.exe and ctDeflate at every level, measures the wall time of each run and checks that every output is identical. It switches to the High Performance power plan during the run (or a temporary copy where the plan is hidden) and restores the previous plan afterwards; if Windows refuses the switch, for example on Windows Server without administrator rights, the report says so and the run goes on with the current plan (`-KeepPowerPlan` leaves the plan alone). The script runs on Windows PowerShell 5.1 and later. Report and CSV go to `benchmark\results\`; the report ends with whether the previous power plan was restored. Exit code of the script and of `run.cmd`: 0 if every output was identical, 2 if at least one differed or a run failed, 1 for other errors (a missing program, wrong parameters, a CPU ctDeflate does not run on, an abort), checked before the power plan is switched where possible. `New-BenchPackage.ps1 -Corpus <folders>` packs the programs, the script and test files for other PCs; `run.cmd` without arguments then benchmarks those files. Results of your hardware are welcome.

---

## Benchmarks

<details>
<summary><b>Machines and method</b></summary>

* Main machine: Intel Core i9-11900K (8 cores, 16 threads), Windows 11, High Performance power plan. Further CPUs below.
* `benchmark\Compare-Gzip.ps1`: every file and level compressed by `gzip.exe` (libdeflate 1.26, one thread) and by ctDeflate (all logical processors), the programs interleaved round by round; measured is the wall time of the whole program run, including program start, reading the input and writing the `.gz`. Median of 3 runs. Every output is compared with `gzip.exe`'s byte for byte: 117 of 117 runs on the three machines were identical.
* Measured cold (2026-10-17, `bin\` of this repository): before every run the script writes a fresh copy of the input without the file cache (`robocopy /J`), so each program reads the file from the disk, as it would a file it has not seen before; the copy is not timed. `-Warm` reads from the file cache instead; earlier versions of this README showed warm numbers, which are higher for the small files.

</details>

### Corpus

* **enwik9:** the first 10<sup>9</sup> bytes of the English Wikipedia XML dump, the test file of the [Large Text Compression Benchmark](http://mattmahoney.net/dc/textdata.html), one large file.
* **Silesia corpus:** 12 files of 5 to 51 MB, 212 MB in all (text, XML, databases, executables, images), the [usual corpus](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) of compressor benchmarks.

`New-BenchPackage.ps1 -Corpus` packs them with the programs and the script; `run.cmd` without arguments then benchmarks them.

### Results per file and level

<details>
<summary><b>Per file</b>: the gain grows with the file size and the level</summary>

Intel Core i9-11900K, 16 threads, gzip.exe → ctDeflate:

| File | size | level 1 | level 6 | level 9 |
|---|---:|---:|---:|---:|
| enwik9 (Wikipedia XML text) | 1,000 MB | 200 → 833 MB/s (**4.2×**) | 89 → 366 MB/s (**4.1×**) | 58 → 296 MB/s (**5.1×**) |
| mozilla (tar of executables) | 51 MB | 202 → 650 MB/s (**3.2×**) | 113 → 332 MB/s (**2.9×**) | 37 → 214 MB/s (**5.8×**) |
| webster (dictionary text) | 41 MB | 57 → 68 MB/s (**1.2×**) | 42 → 60 MB/s (**1.4×**) | 24 → 55 MB/s (**2.3×**) |
| nci (chemical database) | 34 MB | 333 → 925 MB/s (**2.8×**) | 198 → 572 MB/s (**2.9×**) | 68 → 343 MB/s (**5.0×**) |
| samba (tar of source code) | 22 MB | 214 → 498 MB/s (**2.3×**) | 111 → 245 MB/s (**2.2×**) | 48 → 168 MB/s (**3.5×**) |
| dickens (English text) | 10 MB | 150 → 280 MB/s (**1.9×**) | 69 → 145 MB/s (**2.1×**) | 36 → 113 MB/s (**3.1×**) |
| osdb (MySQL database) | 10 MB | 149 → 337 MB/s (**2.3×**) | 89 → 196 MB/s (**2.2×**) | 80 → 185 MB/s (**2.3×**) |
| mr (MRI image) | 10 MB | 178 → 330 MB/s (**1.9×**) | 86 → 119 MB/s (**1.4×**) | 41 → 79 MB/s (**1.9×**) |
| x-ray (X-ray image) | 8 MB | 105 → 219 MB/s (**2.1×**) | 79 → 97 MB/s (**1.2×**) | 69 → 93 MB/s (**1.3×**) |
| sao (star catalog, binary) | 7 MB | 100 → 205 MB/s (**2.1×**) | 59 → 107 MB/s (**1.8×**) | 46 → 93 MB/s (**2.0×**) |
| reymont (Polish text, PDF) | 7 MB | 113 → 195 MB/s (**1.7×**) | 64 → 124 MB/s (**1.9×**) | 20 → 61 MB/s (**3.0×**) |
| ooffice (executable) | 6 MB | 81 → 119 MB/s (**1.5×**) | 58 → 82 MB/s (**1.4×**) | 42 → 77 MB/s (**1.8×**) |
| xml (XML files) | 5 MB | 64 → 74 MB/s (**1.2×**) | 54 → 60 MB/s (**1.1×**) | 37 → 54 MB/s (**1.4×**) |

The gain grows with the file size and the level. ctDeflate parses the input in 1 MiB chunks, so a file of 6 to 10 MB keeps only 6 to 10 of the 16 threads busy, and its run is partly program start and I/O. Cold, `webster` and `xml` are bound by reading the file: ctDeflate needs about the same time at every level there (webster 0.61 to 0.75 s), so the factor stays small. Level 9 spends the most time in the match search, the part that runs in parallel. `mr` (an MRI image) is the hard case at level 6: the minimum match length libdeflate chooses changes all the time there, and the resolver has to re-parse parts of the file on its own thread.

</details>

<!-- BENCHMARK: ctDeflate --sequential (one thread) per file and level, if wanted as a further column -->

### Other CPUs

Factor against gzip.exe on the same machine, all logical processors, same corpus and method:

| CPU | enwik9 level 1 | level 6 | level 9 | Silesia level 1 | level 6 | level 9 |
|---|---:|---:|---:|---:|---:|---:|
| Intel Core i9-11900K, 8 cores / 16 threads, Windows 11 | 4.2× | 4.1× | 5.1× | 1.6× | 1.7× | 2.9× |
| Intel Core Ultra 7 155H (notebook), 16 cores (6 performance, 10 efficiency) / 22 threads | 2.4× | 1.6× ² | 2.2× ² | 1.7× | 1.8× | 2.8× |
| Intel Xeon E3-1275 v6, 4 cores / 8 threads | 5.4× ¹ | 2.9× | 3.3× | 2.3× | 1.7× | 2.6× |

All three machines 2026-10-17, cold, 39 of 39 runs bit-identical on each.

¹ On the Xeon machine `gzip.exe` compressed enwik9 at level 1 with only 73 MB/s, while it reached 142 MB/s at level 1 on the Silesia files there and 149 to 200 MB/s on enwik9 on the other machines. So that run was limited by reading and writing the 1 GB file rather than by compression, and the level 1 factor overstates the gain.
² On the Core Ultra notebook ctDeflate reached only 128 MB/s on enwik9 at level 6 and 100 MB/s at level 9 (the i9 machine: 366 and 296 MB/s), although the notebook has more cores. Not yet explained; see Room for improvement.

### Threads

<!-- BENCHMARK: speed by number of threads for enwik9; the numbers below are from a file of our own and a build before the last parser change -->
Level 9, a 163 MB database file of our own: 1 / 2 / 4 / 8 / 12 / 16 threads give 38 / 69 / 137 / 252 / 341 / 380 MB/s. Up to 8 threads (one per core) the speed grows almost linearly (6.6×); the 8 extra hyperthreads add another 1.5×.

### Memory

ctDeflate reads and writes the files in streaming mode; only the chunks in flight are in memory. Peak working set with 16 threads, measured on files of our own:

| Input | level 1 | level 6 | level 9 |
|---|---:|---:|---:|
| database, 163 MB | 58 MB | 84 MB | 80 MB |
| the database 32 times, 5.2 GB | 78 MB | 144 MB | |

The 5.2 GB file takes 7.3 s at level 6 against 28.8 s for `gzip.exe` (4.0×), with the same output.

<!-- BENCHMARK: peak working set (ctDeflate -v) for enwik9 -->

---

## Why this is hard

Parallel gzip tools such as pigz cut the input into pieces and compress them independently: the result is valid gzip, but not the same bytes. ctDeflate may not do that. Its output has to be exactly libdeflate's, and libdeflate's compressor is sequential in three ways:

- **block boundaries:** libdeflate ends a block when the statistics of the symbols parsed since the block began change enough, so where a block ends depends on everything before it in the block;
- **the parse:** at levels 2 to 9 the minimum match length (3 to 9) is chosen from the first 4 KiB of each block and, at the lazy levels, recalculated from the block's literal counts as the parse goes on, and the match search sees the 32 KiB before every position, so the same position is parsed differently depending on where its block began;
- **the bit stream:** every block starts at the bit where the previous one ended, and whether a block is stored uncompressed depends on that bit offset.

ctDeflate keeps all of that exact and still runs the heavy part, the match search, in parallel:

```mermaid
flowchart LR
    subgraph io1 ["I/O scheduler"]
        R["Read<br/>overlapped ReadFile"]
    end
    subgraph cpu ["compute scheduler · one thread per logical processor"]
        P["Match search ×N<br/>speculative chunks"]
        E["Huffman coding ×N<br/>same workers"]
        S["Assemble<br/>in order"]
    end
    subgraph own ["the calling thread"]
        V["Resolve<br/>in order"]
    end
    subgraph io2 ["I/O scheduler"]
        W["Write<br/>overlapped WriteFile"]
    end
    R -- "input window" --> P
    P -- "chunk parses" --> V
    V -- "block ring" --> E
    E --> S
    S -- "output window" --> W
```

**[How the graph grew, step by step →](docs/task-graph.md)** from libdeflate's single call to this
graph, with the reason and the measured gain of each step.

1. **Speculative chunk parses:** worker tasks parse 1 MiB chunks of the input in parallel, each as if a block started there, with libdeflate's matchfinder rebuilt from the 32 KiB before the chunk. Where the minimum match length is uncertain, a chunk follows several parse paths at once; they share the match searches and split only where the decision differs.
2. **Resolver:** one serial stage, on the calling thread, replays libdeflate's block logic over the chunk parses in order. Where a chunk's parse meets the true parse (same position, same minimum match length) it takes over; where none does, the resolver re-parses exactly. The result is libdeflate's exact sequence of literals and matches and its exact block boundaries.
3. **Encode and stitch:** resolved blocks are Huffman-coded in parallel while the resolver goes on, and appended to the output in order; an uncompressed block is chosen by the actual bit offset, as libdeflate does.
4. **Streaming:** reading and writing are tasks of their own with overlapped file I/O on an I/O scheduler; only the chunks in flight are in memory (a 5.2 GB file compresses with less than 150 MB, enwik9 with 130 to 165 MB against 1.3 GB for `gzip.exe`). Back-pressure keeps it so: Resolve waits when the ring of blocks is full, assembling waits when 16 MB of output are not yet written.
5. **Bit identity:** every change is checked against the reference on a corpus of real files at all levels and with 1 to 16 threads.

---

## The reference

`bin\gzip.exe` is the gzip program of **libdeflate 1.26** from the project's official Windows x64 release (built with GCC/MinGW), SHA-256 `376e331df9da3b8aa465dd7aaa5139adc8c60d9a1dd528cfb0ddeee55d19aaec`. Every bit comparison and every benchmark in this README uses it.

`libdeflate\libdeflate-1.26\` holds the libdeflate sources (`lib\`, `programs\`); `ctDeflate.slnx` also builds the library and the same `gzip.exe` with Visual C++ (see [Building](#building)). The result, `libdeflate\build\Release\bin\gzip.exe`, writes the same files as the official build (checked at levels 1, 6 and 9).

What ctDeflate reproduces is the output of `libdeflate_gzip_compress()`, which `gzip.exe` writes: the gzip header without file name and with modification time 0, the DEFLATE stream of the chosen level, CRC-32 and size. libdeflate has the levels 0 to 12; ctDeflate implements 0 to 9 (`gzip.exe` itself starts at 1). The levels 10 to 12 use libdeflate's near-optimal parser and are not supported yet.

---

## Building

You need **Visual Studio 2026** with the workload "Desktop development with C++" (MSVC v145, x64). ctDeflate's own code is classic headers and `.cpp` files; the scheduler's library is consumed as C++ modules from the API submodule.

Clone to a short path such as `C:\src`; deeply nested folders can exceed the 260-character limit of the build tools.

```cmd
git clone --recurse-submodules https://github.com/ThinkMeta-com/ThinkMeta.ConcurrentTasks.Samples.git
cd ThinkMeta.ConcurrentTasks.Samples\ctDeflate
msbuild ctDeflate.slnx /p:Configuration=Release /p:Platform=x64
```

Run `msbuild` from a "Developer Command Prompt for VS 2026", or open `ctDeflate.slnx` in Visual Studio. The output:

| File | Folder |
|---|---|
| `ctDeflate.exe`, the unit tests `ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests.dll` and the scheduler core `ThinkMeta.ConcurrentTasks.Core.dll` | `build\Release\bin\` |
| `gzip.exe` (built from the libdeflate sources) | `libdeflate\build\Release\bin\` |

This build uses the DLL C runtime (part of Visual Studio). `bin\ctDeflate.exe` is linked with the static C runtime; to build it that way:

```cmd
msbuild src\App\ThinkMeta.ConcurrentTasks.Samples.ctDeflate.vcxproj /p:Configuration=Release /p:Platform=x64 /p:ForceImportAfterCppProps=%CD%\props\StaticCrt.props
```

(output in `build\ReleaseStatic\bin\`). A program you build yourself is not byte for byte the same file as `bin\ctDeflate.exe` (compiler version, paths, time stamps), but its **output must be bit-identical** to `gzip.exe`'s, like that of `bin\ctDeflate.exe`.

### Checking a build

`verify\Verify-Deflate.ps1` compresses test inputs with your `ctDeflate.exe` at every level and compares each `.gz` byte for byte with libdeflate's. It needs PowerShell 7 (`pwsh`); the exit code is 0 if everything is identical.

```cmd
pwsh verify\Verify-Deflate.ps1 -Exe build\Release\bin\ctDeflate.exe
pwsh verify\Verify-Deflate.ps1 -Exe build\Release\bin\ctDeflate.exe -ExtraArgs "--threads 2 --chunk 4096"
pwsh verify\Verify-Deflate.ps1 -Exe build\Release\bin\ctDeflate.exe -Files D:\Data -Levels 1,6,9
```

| `-Matrix` | Inputs | Reference |
|---|---|---|
| `Generated` (default) | synthetic data the script generates itself (random, periodic, text-like, sparse; 22 MB), levels 1 to 9 | `verify\Reference-Hashes-Generated.csv` |
| `Quick` | the [Silesia corpus](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) in `verify\corpus\silesia\` (the 12 files), levels 1, 2, 5, 6, 8, 9 | `verify\Reference-Hashes-Quick.csv` |
| `Full` | Silesia and [enwik9](http://mattmahoney.net/dc/textdata.html) (`verify\corpus\enwik9`), levels 1 to 9 | `verify\Reference-Hashes-Full.csv` |
| `-Files` | your own files or folders | `bin\gzip.exe`, run on the same files |

The reference hashes were made with `bin\gzip.exe`. `-ExtraArgs` passes options to ctDeflate, so the same inputs can be checked with other thread counts and chunk sizes. The unit tests run in Visual Studio's Test Explorer or with `vstest.console build\Release\bin\ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests.dll`; two of them use files you name in the environment variables `CTDEFLATE_TEST_FILES` (paths separated by `;`) and `CTDEFLATE_SPEED_FILE`, and skip without them.

---

## Command line

```
ctDeflate [options] FILE
```

ctDeflate compresses one file to `FILE.gz` (or the file given with `-o`) and keeps the input.

| Option | Meaning |
|---|---|
| `-0` … `-9` | compression level, as in libdeflate; default `-6`. `-0` stores the data uncompressed, `-1` is the fastest, `-9` the strongest |
| `-o FILE` | output file; default: the input name plus `.gz`. An existing file is overwritten |
| `--threads n` | worker threads; default: all logical processors |
| `--sequential` | the plain port as one compressor task between asynchronous reading and writing (the single-threaded reference path, about 20 MB of memory for any file size) |
| `-v` | print sizes, time, speed, peak memory and the counters of the parallel pipeline (below) |

<details>
<summary><b>Options for tuning and diagnosis</b></summary>

| Option | Meaning |
|---|---|
| `--in-memory` | parallel, but read the whole file first and write the output at the end, instead of streaming |
| `--chunk n` | size of the speculative parse chunks in bytes; default 1048576 (1 MiB), at least 4096 |
| `--ahead n` | how many chunks the parses may run ahead of the resolver; default threads + 4. More can be a few percent faster and costs memory |

</details>

Differences from `gzip.exe`:

| gzip.exe | ctDeflate |
|---|---|
| any number of files per call | one file per call |
| replaces the input by `FILE.gz` unless `-k` | always keeps the input |
| refuses to overwrite an existing `FILE.gz` unless `-f` | overwrites the output |
| `-c` (to standard output), reading from standard input | files only |
| `-d` (decompress, also as `gunzip`), `-t` (test) | compression only: decompress or test with `gzip.exe` |
| `-S SUF`, `-q`, `-V`, `-h` | not supported (`-o` instead of `-S`) |
| levels 1 to 12 | levels 0 to 9 |

The exit code is 0 on success, 1 on an error (unknown option, a file that cannot be read or written, the output being the input file itself) and 3 if the CPU lacks an instruction set ctDeflate needs; after an error in the default streaming mode ctDeflate deletes the incomplete output it created, and never a file it did not create.

<details>
<summary><b>Example of <code>-v</code></b></summary>

```
D:\silesia\dickens: 10192446 -> 3859454 bytes, 0.060 s, 170.9 MB/s, peak memory 66 MB
  chunks 10, blocks 46, syncs 9, repairs 8 (32701 bytes), max sync distance 22; parse+resolve 0.050 s (resolver waited 0.034 s), encode 0.001 s, stitch 0.000 s
  repairs by cause: minLen 6, parse ended 2; iterations by minLen: 3:6678/117KB 4:478/9816KB 5:2/19KB
```

The first line: input and output size, the time including reading and writing the files, the speed and the peak working set. The second: the chunks of the speculative parse, the deflate blocks written, how often the resolver took over a chunk's parse (syncs) and how often and over how many bytes it had to re-parse on its own (repairs), the time until the last block was resolved and how much of it the resolver waited for chunk parses. The third: why it re-parsed (the chunk had no parse path for the true minimum match length, or its paths ended), and per minimum match length the iterations the resolver checked one by one and the input they covered. With `--sequential` only the first line is printed.

</details>

---

## Reading the code

The parallel version adds a task layer around a port that does not know about tasks. Read it in this order:

1. **`src\Tasks\DeflateGraph.h`**: the graph as a diagram, then `DeflateGraph::RunParallel` in `DeflateGraph.cpp`: it builds the edges, creates the nodes, starts them and waits for them. This is where ThinkMeta.ConcurrentTasks is used: `TaskGroup`, `ITaskScheduler::AddTask`, the main thread running Resolve.
2. **The edges** (`src\Tasks\`): `InputWindow` (Read → Parse), `ChunkLogs` (Parse → Resolve), `BlockRing` (Resolve → Encode → Stitch), `OutputWindow` (Stitch → Write). Each class starts with the nodes it connects; its methods follow one vocabulary: `Acquire…` may wait and hands out the next piece of work, `TryAcquire…` claims one without waiting, `Complete…` hands the result on, `Await…` waits for a condition.
3. **The nodes** (`DeflateNodes.h`): `ReadTask`, `WorkerTask` (Parse and Encode), `ResolveNode`, `Stitch`, `WriteTask`, `SequentialCompressTask`. Each is a plain loop that waits on an edge.
4. **`src\App\main.cpp`**: `WithSchedulers` is the setup of ThinkMeta.ConcurrentTasks, the same in every sample (convert the thread, enable async I/O, an I/O and a compute scheduler).
5. **The engine** (`src\Encoder\`), which has no task code: the port of libdeflate (`Compressor`, `Matchfinder.h`, `BlockWriter`, `Huffman`, `BlockRules.h`, `Gzip.h`, `Checksums`) and, new in ctDeflate, the speculative parse and its resolver (`Parser`, `MultiParser`, `OrderedMatchfinder.h`, `BlockTracker`, `Resolver`, `ChunkedDeflate`). Each ported function names its libdeflate original in a comment; [docs/porting.md](docs/porting.md) maps them all.

Why the edges are classes of their own and not the library's `StreamedFileReader` and `OrderedFileWriter`: the input is read only as far as Resolve allows (the read allowance), it wakes the parse of each chunk separately, the bit stream is written in place into the output window without a copy, and both windows release memory behind the slowest stage. The library's readers and writers stream through fixed blocks in order; they have neither the allowance nor the per-chunk wake-up.

### Error handling

ctDeflate stops at the first error and reports it; it does not try to recover. A read or write error ends the graph normally (every node finishes with what it has) and the program reports the Windows error and deletes the output it created. If memory runs out where the graph cannot go on (committing the output window), the process ends at once; the sample does not unwind its graph for that case.

---

## Room for improvement

ctDeflate is not finished; the measurements point to these next steps:

* **Serial parts at low levels:** one task replays libdeflate's block logic over the chunk parses, in order, and the file is read and written in order. At level 1 the parallel parse is so fast that these serial parts take most of the time.
* **Re-parsing where the minimum match length changes:** libdeflate chooses a minimum match length per block from the block's own data and adjusts it within the block. On data where it changes often (source code, logs, the MRI image of the Silesia corpus), a chunk follows several parse paths for the candidate values, and every extra path costs up to 20 % of parse speed; where none of them meets the true parse, the resolver re-parses on its own thread. A better prediction of the value at chunk starts would save both.
* **Per-thread overhead:** with one worker, the parallel pipeline was about 18 % slower than ctDeflate `--sequential` in our last measurement: every chunk rebuilds the matchfinder from the 32 KiB before it, parses 16 KiB past its end, and the paths need bookkeeping. Larger chunks or reusing the matchfinder across neighbouring chunks would reduce that.
* **Small files:** a file of 6 to 10 MB has only 6 to 10 chunks of 1 MiB, fewer than the threads of most CPUs; smaller chunks for small files would keep all threads busy. Below about 20 MB the program start and the file I/O also take a large share of the time, and the parse-path selection needs a few resolved chunks before it settles on one path.
* **Hybrid CPUs:** on the Core Ultra 7 155H (performance and efficiency cores) enwik9 ran at only 128 MB/s at level 6 and 100 MB/s at level 9, a third of the i9 machine with half its cores, while the other machines gain most at level 9. Whether the scheduling of the parse tasks onto efficiency cores, power limits of the notebook or memory is the cause is still open.
* **Compiler:** the single-threaded match search runs at 92 to 100 % of libdeflate built with the same compiler (MSVC), but the official `gzip.exe` is built with GCC and is up to 10 % faster on one core.
* **Levels 10 to 12:** libdeflate's near-optimal parser carries its cost model from block to block, a serial chain; ctDeflate does not support these levels yet.
* **What did not work:** compressing chunks independently and concatenating them (as parallel gzip tools do) can never be bit-identical, because libdeflate's parse and block boundaries depend on everything before; choosing the parse paths only from the values of the last blocks caused many re-parses on databases. Both led to the speculative parse with an exact resolver.

Every step keeps the output bit-identical to libdeflate.

---

## ThinkMeta.ConcurrentTasks

`ctDeflate` is a showcase for **ThinkMeta.ConcurrentTasks**, a fiber-based task scheduler and concurrency runtime by ThinkMeta Software GmbH. Tasks run on lightweight user-mode fibers: a task waiting for a chunk, an event or I/O yields without blocking its worker thread, and a task switch costs a few hundred nanoseconds.

### Why fibers here

In ctDeflate the stages wait on each other all the time:
* The resolver waits for the parse of the next chunk.
* The workers wait for resolved blocks to encode.
* The reader and writer tasks wait for their asynchronous reads and writes through the I/O completion port, the reader also for room in the window of chunks in flight and the writer for finished output. Only those chunks are in memory, which is how a 5.2 GB file compresses with less than 150 MB.

On fibers each of these waits is a plain blocking call (`Wait()` on an event or a semaphore) in sequential code, and the worker thread meanwhile runs other parses and encodes. A thread pool would have to block one of its threads for each such wait, or split the resolver and the reader into callbacks that resume at every chunk boundary and every completed read.

The library's API (headers, C++ modules and the scheduler core) is in [ThinkMeta.ConcurrentTasks.API](https://github.com/ThinkMeta-com/ThinkMeta.ConcurrentTasks.API), included in this repository as the submodule `ThinkMeta.ConcurrentTasks.API`. The `bin\` programs must be used with the `ThinkMeta.ConcurrentTasks.Core.dll` next to them: a newer or older core does not run them.

👉 **Get in touch:** [www.thinkmeta.com](https://www.thinkmeta.com)

---

## License

* **ctDeflate** (the source code in `src\`, `bin\ctDeflate.exe`, the scripts in `verify\` and `benchmark\`, `New-BenchPackage.ps1`, the project files): MIT License, Copyright (c) 2026 ThinkMeta Software GmbH ([`LICENSE.txt`](LICENSE.txt)). The engine contains code ported from libdeflate, whose MIT notice is in [`COPYING`](COPYING).
* **libdeflate sources and `bin\gzip.exe`** (`libdeflate\`): MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC ([`COPYING`](COPYING)).
* **Scheduler core** (`bin\ThinkMeta.ConcurrentTasks.Core.dll`, and the core in the API submodule): proprietary, Copyright (c) 2026 ThinkMeta Software GmbH ([`LICENSE-Core.txt`](LICENSE-Core.txt)): free for private and non-commercial use, commercial use requires a license from ThinkMeta Software GmbH.

**Benchmarks are explicitly permitted:** anyone may run benchmark tests of any kind with the programs in `bin\`, on any hardware and for any purpose, commercial evaluation included, and publish the results, including comparisons with other software, without asking ThinkMeta Software GmbH first (point 2 of [`LICENSE-Core.txt`](LICENSE-Core.txt)).
