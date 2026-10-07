# ThinkMeta.ConcurrentTasks Samples

Encoders built on **ThinkMeta.ConcurrentTasks**, a fiber-based task scheduler for Windows. Each
one is a hand-written C++ port of a well-known reference encoder, writes *exactly* the same
output as that reference (byte for byte) and spreads the work on a **single file** over all cores.

The parallelization and optimizations were done **AI-assisted**, using ThinkMeta.ConcurrentTasks:
AI coding agents wrote, measured and tuned the code under the direction of ThinkMeta's developers,
and every step was checked byte for byte against the reference.

| Folder | Reference | Contents |
|---|---|---|
| [`ctLame\`](ctLame) | LAME 4.0 (MP3, VBR) | reference build, benchmark; `ctLame.exe` follows |
| [`ctOpus\`](ctOpus) | opusenc (opus-tools 0.2, libopus 1.6.1) | prebuilt binaries, benchmark, live encoding |
| [`ctDeflate\`](ctDeflate) | libdeflate 1.26 (gzip) | prebuilt binaries, benchmark |
| [`ctJpeg\`](ctJpeg) | jpegli (JPEG, google/jpegli @ 031a007) | prebuilt binaries, benchmark |

## Benchmarks at a glance

| Encoder | Workload | Reference | ct… | Speedup |
|---|---|---:|---:|---:|
| [ctLame](ctLame#benchmarks) | 343 WAV files, 45.1 h of CD audio, `-V 4` | 1,168.8 s | 83.0 s | **14.1×** |
| [ctOpus](ctOpus#benchmarks) | 343 WAV files, 45.1 h of music, 128 kbit/s, `--comp 10` | 1,267.1 s | 289.5 s | **4.4×** |
| [ctDeflate](ctDeflate#benchmarks) | enwik9, one file of 1 GB, level 6 / level 9 | 106 / 61 MB/s | 392 / 303 MB/s | **3.7× / 4.9×** |
| [ctJpeg](ctJpeg#benchmarks) | one image, 39 MP, `-q 85` | 0.750 s | 0.108 s | **6.9×** |

*Intel Core i9-11900K (8 cores, 16 threads), Windows 11, High Performance power plan, wall time
of the whole program run (median or best of several runs, see each README), reference and ct…
encoder with the same settings, every output bit-identical to the reference. Details, more
machines and the limits of each encoder are in the README of each folder.*

## Why fibers

In all four encoders the reference is sequential, and the parallel version has to reproduce its
result exactly. In ctLame, ctOpus and ctDeflate that takes many small jobs that wait for each
other in the middle of their work: for the frame before them, for the next job of their stage,
for a chunk of the file to arrive. ctJpeg is simpler, a fixed sequence of fork-join phases; there
only the encoder itself waits, once per phase. ThinkMeta.ConcurrentTasks runs every job as a task
on a fiber:

* **Waiting does not block a thread.** A task that waits is switched out, and its worker thread
  goes on with the next task. A thread pool has two choices instead: block the worker, which
  leaves a core idle, or cut the job into callbacks or a state machine at every point where it
  might wait.
* **The code stays sequential.** Because a task can simply wait, a ported function reads like the
  original, line for line. For output that has to be bit-identical, that is what makes the port
  checkable.
* **Fine-grained work pays off.** A task switch costs a few hundred nanoseconds, so steps of a few
  microseconds are worth running as tasks of their own: with 16 threads, ctLame finishes a frame
  about every 13 µs.
* **I/O without extra threads.** Files can be read and written asynchronously over I/O completion
  ports (ctDeflate does); the task waiting for the data is resumed when it arrives.

The scheduler core is a DLL of about 15 KB that depends on nothing but Windows. What fibers do in
each encoder is described in the README of each folder.

Every folder is self-contained: its own README, prebuilt Windows x64 binaries in `bin\`, the
reference source with a Visual Studio solution (for jpegli a CMake build script), and a benchmark
script that compares speed and bit identity on your own files. Results of your hardware are
welcome.

**Requirements:** Windows 10/11 or Windows Server 2016 and later, x64; no installation, the C
runtime is linked statically. ctOpus and ctDeflate need a CPU with AVX2 (Intel Haswell, AMD
Excavator or later) and say so at start-up on older CPUs; ctJpeg runs on any x64 CPU and uses
AVX2 where available; ctLame runs on any x64 CPU and chooses scalar, SSE4.1 or AVX2 code at run
time. The benchmark scripts run on Windows PowerShell 5.1 and later.

## Licenses

* **ctOpus, ctDeflate, ctJpeg:** the binaries and `ThinkMeta.ConcurrentTasks.Core.dll` are
  proprietary, under the provisional terms in `LICENSE-Core.txt` of each folder: free for private
  and non-commercial use, commercial use requires a license from ThinkMeta Software GmbH. Their
  source code is planned to be published under the MIT License. Each port includes the license
  notice of its reference (`COPYING`).
* **ctLame:** a port of LAME and therefore under the GNU LGPL. `ctLame.exe` is not included yet:
  it will be added once the LAME project has agreed to publishing the binary ahead of its source
  code, which will follow under the GNU LGPL.
* **Benchmarks are explicitly permitted:** anyone may benchmark the programs, on any hardware and
  for any purpose, commercial evaluation included, and publish the results, including comparisons
  with other software, without asking us first (point 2 of `LICENSE-Core.txt`).
* **Reference sources and benchmark scripts:** the licenses of the reference projects (`COPYING`
  in each folder) and the MIT License for the scripts.
