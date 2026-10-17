# ThinkMeta.ConcurrentTasks Samples

Encoders built on **ThinkMeta.ConcurrentTasks**, a fiber-based task scheduler for Windows. Each
one is a new C++ port (rewritten, not linked) of a well-known reference encoder, writes *exactly*
the same output as that reference (byte for byte) and spreads the work on a **single file** over
all cores.

All four encoders were created **AI-assisted**, using ThinkMeta.ConcurrentTasks: AI coding
agents wrote the port, its parallelization and optimizations, measured and tuned the code under
the direction of ThinkMeta's developers, and every step was checked byte for byte against the
reference. **An in-depth code review is still pending:** the output is verified to be identical
to the reference, but the code itself has not yet been reviewed line by line.

| Folder | Reference | Contents |
|---|---|---|
| [`ctLame\`](ctLame) | LAME 4.0 (MP3, VBR) | source code, prebuilt binaries, benchmark |
| [`ctOpus\`](ctOpus) | opusenc (opus-tools 0.2, libopus 1.6.1) | source code, prebuilt binaries, benchmark, live encoding |
| [`ctDeflate\`](ctDeflate) | libdeflate 1.26 (gzip) | source code, prebuilt binaries, benchmark |
| [`ctJpeg\`](ctJpeg) | jpegli (JPEG, google/jpegli @ 031a007) | source code, prebuilt binaries, benchmark |
| [`ThinkMeta.ConcurrentTasks.API\`](https://github.com/ThinkMeta-com/ThinkMeta.ConcurrentTasks.API) | - | the public API of the scheduler, as a submodule ([below](#thinkmetaconcurrenttasks-api)) |

Clone with the submodule:

```
git clone --recurse-submodules https://github.com/ThinkMeta-com/ThinkMeta.ConcurrentTasks.Samples.git
```

In an existing clone, `git submodule update --init --recursive` fetches it. The prebuilt binaries
and benchmark scripts in the `ct…` folders run without it; building from source needs it.

## Benchmarks at a glance

| Encoder | Workload | Reference | ct… | Speedup |
|---|---|---:|---:|---:|
| [ctLame](ctLame#benchmarks) | 343 WAV files, 45.1 h of CD audio, `-V 4` | 1,195.2 s | 85.2 s | **14.0×** |
| [ctOpus](ctOpus#benchmarks) | 343 WAV files, 45.1 h of music, 128 kbit/s, `--comp 10` | 1,278.6 s | 290.3 s | **4.4×** |
| [ctDeflate](ctDeflate#benchmarks) | Silesia corpus + enwik9, 1.2 GB, level 6 / level 9 | 87 / 53 MB/s | 282 / 228 MB/s | **3.3× / 4.3×** |
| [ctJpeg](ctJpeg#benchmarks) | 6 photos, 99 MP, `-q 85`, AVX2 | 2.109 s | 0.425 s | **5.0×** |

*Intel Core i9-11900K (8 cores, 16 threads), Windows 11, High Performance power plan, cold runs
(fresh copy of the input before every run), wall time of each program run, sum of the medians
over all files, reference and ct… encoder with the same settings, every output bit-identical to
the reference. Details, two more machines (Xeon E3-1275 v6, Core Ultra 7 155H) and the limits of
each encoder are in the README of each folder.*

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

The scheduler core is a DLL of about 25 KB that depends on nothing but Windows. What fibers do in
each encoder is described in the README of each folder.

Every folder is self-contained: its own README, prebuilt Windows x64 binaries in `bin\`, the
source code of the ct… program with its tests and verification scripts, the reference source, a
Visual Studio solution (for jpegli a CMake build script), and a benchmark script that compares
speed and bit identity on your own files. The "Building" section of each README says how to build
the programs yourself; they build against the API submodule. Results of your hardware are
welcome.

**Requirements:** Windows 10/11 or Windows Server 2016 and later, x64; no installation, the C
runtime is linked statically. ctOpus and ctDeflate need a CPU with AVX2 (Intel Haswell, AMD
Excavator or later) and say so at start-up on older CPUs; ctJpeg runs on any x64 CPU and uses
AVX2 where available; ctLame runs on any x64 CPU and chooses scalar, SSE4.1 or AVX2 code at run
time. The benchmark scripts run on Windows PowerShell 5.1 and later.

## ThinkMeta.ConcurrentTasks API

The folder `ThinkMeta.ConcurrentTasks.API\` is the public API repository
[ThinkMeta.ConcurrentTasks.API](https://github.com/ThinkMeta-com/ThinkMeta.ConcurrentTasks.API),
included as a submodule. It holds what you need to build your own programs on the scheduler:

* `Core.h`, the ABI of the scheduler core;
* the library as C++ modules: tasks, synchronization primitives, collections, virtual-memory
  pipes, asynchronous files and sockets;
* the prebuilt core, `ThinkMeta.ConcurrentTasks.Core.dll`, with the .props file that binds it in;
* small samples and the samples of the online documentation.

The API is preliminary and may still change. Introduction, concepts, guides and the API reference
are at [www.thinkmeta.com/concurrenttasks](https://www.thinkmeta.com/concurrenttasks).

### The minimal core and its limits

The core in this repository is the minimal build: its own fiber switch carries only what the ct…
programs need, and that is what makes a task switch cheap. All scheduler threads are set up
alike, so a task may continue on another thread after every wait, and the following is per
thread, not per task:

* **Floating-point control state** (MXCSR, x87 control word: rounding mode, denormals,
  exception masks). A task that changes it must restore it before it waits or yields.
* **Fiber-local storage** (`FlsAlloc`) and `thread_local` variables, including per-thread state
  of the C runtime such as `errno`: after a wait the task may see another thread's values.
* **Activation contexts** (`ActivateActCtx`, side-by-side manifests) and the stack guarantee
  (`SetThreadStackGuarantee`): set them per thread, not inside a task.
* **Windows fiber functions** (`ConvertThreadToFiber`, `SwitchToFiber`, `GetCurrentFiber` …) must
  not be used inside tasks.

Every task stack has the size given in the header of the executable (linker option `/STACK`).
CET shadow stacks are detected at start-up and switched along; the ct… programs are not linked as
CET-compatible (`/CETCOMPAT`), so they run without shadow stacks, and the shadow-stack path of
the core has not been tested yet. Builds of the core that keep the floating-point state, the activation
context or the stack guarantee per task exist as compile-time options and are not part of this
release.

## Parallelize your own algorithm with an AI agent

The four encoders were made by AI coding agents along the same route: keep the sequential
reference as an oracle, port it, make the port bit-identical, then parallelize step by step and
check every step against the oracle. You can give an agent (Claude Code, Codex, Gemini CLI or
similar, with access to a shell and Visual Studio 2026) the same task for your own code. A
prompt to start with, adjust the parts in angle brackets:

```text
Goal: a parallel version of <my program / function> on ThinkMeta.ConcurrentTasks (Windows x64,
C++ modules, Visual Studio 2026) that produces exactly the same output as the current
sequential version, and is as fast as possible on one input.

1. Clone https://github.com/ThinkMeta-com/ThinkMeta.ConcurrentTasks.Samples with
   --recurse-submodules. Read README.md, ThinkMeta.ConcurrentTasks.API/README.md, the
   documentation at https://www.thinkmeta.com/concurrenttasks, and the README and
   docs/task-graph.md of the sample closest to my problem (ctLame, ctOpus, ctDeflate, ctJpeg).
2. Read my code in <path>. Describe its hot spots and its data dependencies, and propose a
   task graph before writing code. Note the limits of the minimal core in the Samples README.
3. List the calls in the hot paths that serialize or block when many tasks run at once:
   heap allocations (malloc/new share a lock), global or static state, stdio and logging,
   rand(), locale functions, and blocking OS calls (Sleep, WaitForSingleObject, synchronous
   file I/O), which stop a whole worker thread instead of one task. Replace them with
   preallocated or per-task buffers, the library's synchronization primitives and its
   asynchronous files, without changing the output.
4. Build an oracle first: the unmodified sequential build, a set of test inputs (generated or
   public, no private data) and a script that compares the output of any build byte for byte
   with the oracle (SHA-256 per input and option set). Every later step must pass it.
5. Port in small steps, each one built, tested and compared with the oracle. Parallelize only
   where the dependencies allow it; where the order matters, keep it (ordered writers,
   sequential tails). Keep floating-point operations in the same order.
6. Measure cold (fresh copy of the input before each run), on all cores and on one, and report
   the speedup with the numbers, including what did not help.
7. Use the scheduler as it is: the core is a prebuilt DLL, and the library modules of the API
   stay unchanged. If either seems to misbehave, write a small test that shows it and report
   it instead of working around it.
```

The core DLL is free for private and non-commercial use; a program that you build this way and
use commercially needs a license from ThinkMeta Software GmbH (see `LICENSE-Core.txt`).

## Licenses

* **ctOpus, ctDeflate, ctJpeg:** source code and binaries under the MIT License (`LICENSE.txt` in
  each folder). Each port includes the license notice of its reference (`COPYING`).
* **ctLame:** a port of LAME and therefore source code and `ctLame.exe` under the GNU LGPL
  (`COPYING` and `src\ctLame-LICENSE.md` in the folder).
* **`ThinkMeta.ConcurrentTasks.Core.dll`:** proprietary, under the provisional terms in
  `LICENSE-Core.txt` of each folder, the same terms as in the API repository: free for private
  and non-commercial use, also for your own programs and for passing them on with the unchanged
  DLL for private and non-commercial use; commercial use requires a license from ThinkMeta
  Software GmbH. All ct… programs need it to run.
* **Benchmarks are explicitly permitted:** anyone may benchmark the programs, on any hardware and
  for any purpose, commercial evaluation included, and publish the results, including comparisons
  with other software, without asking us first (point 2 of `LICENSE-Core.txt`).
* **Reference sources:** the licenses of the reference projects (`COPYING` in each folder).
