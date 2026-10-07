# ctOpus: Opus, bit for bit, one file on many cores

**ctOpus** is an Opus encoder for Windows x64 that writes *exactly* the same `.opus` files as `opusenc` (opus-tools 0.2 with libopusenc 0.2.1 and libopus 1.6.1), byte for byte, and spreads the encoding of a **single file** over all cores. It is a hand-written C++ port of the Opus encoder and of the opusenc frontend, built on **ThinkMeta.ConcurrentTasks**, a fiber-based task scheduler for Windows.

The parallelization and optimizations were done **AI-assisted**, using ThinkMeta.ConcurrentTasks: AI coding agents wrote, measured and tuned the code under the direction of ThinkMeta's developers, and every step was checked byte for byte against the reference.

| One file, 254 s of 44.1 kHz stereo, 128 kbit/s | time | vs opusenc `--comp 0` |
|---|---:|---:|
| `opusenc --comp 0` | 0.99 s | 1.00× |
| `opusenc --comp 10` | 2.21 s | 0.45× |
| `ctOpus --comp 10 --sequential` (one thread) | 2.00 s | 0.50× |
| **`ctOpus --comp 10`** | **0.50 s** | **2.0×** |
| **`ctOpus --comp 0`** | **0.31 s** | **3.2×** |

*Intel Core i9-11900K (8 cores, 16 threads), Windows 11, High Performance power plan, all variants interleaved in one run, median of 9, every output bit-identical to opusenc with the same settings.*

**ctOpus at maximum complexity is twice as fast as opusenc at minimum complexity**, on a single file and with output identical to opusenc at maximum complexity.

On a corpus of 343 music files (45.1 hours), `--comp 10 --bitrate 128`: **4.4× faster than opusenc** on an 8-core Core i9-11900K (561× real time) and 4.1× on a 4-core Xeon E3-1275 v6, all 343 files bit-identical on every machine ([details](#benchmarks)).

---

## Repository contents

| Folder / file | Contents |
|---|---|
| `bin\` | `ctOpus.exe` (encoder, same command line as opusenc), `ctOpusGui.exe` (live encoding and multi-stream load test), `ctOpusLiveBench.exe` (live latency benchmark), `opusenc.exe` (the reference, built from `opus\`) and `ThinkMeta.ConcurrentTasks.Core.dll` (scheduler core) |
| `opus\` | Unmodified sources of the reference: opus-tools 0.2, libopusenc 0.2.1, libogg 1.3.6 and libopus 1.6.1, with Visual Studio projects ([The reference](#the-reference)) |
| `opus.slnx` | Builds `opusenc.exe` from these sources (Visual Studio 2026, x64; output in `opus\build\Release\bin`) |
| `benchmark\` | `Compare-Folder.ps1` and `run.cmd`: speed and bit identity on your own WAV files |
| `New-BenchPackage.ps1` | Packs `bin\` and the benchmark into a zip for other PCs |

---

## Try it

No installation: the programs in `bin\` are built with the static C runtime and need only Windows and the scheduler core next to them. Requirements: Windows 10 or 11, or Windows Server 2016 or later, x64, and a CPU with AVX2 (Intel from Haswell, 2013; AMD from Excavator and Zen). The encoder is built for AVX2 together with FMA, BMI1/BMI2, LZCNT, MOVBE and F16C; the programs check for all of them, and for an operating system that saves the AVX registers, before anything else runs. Without them they stop with a message instead of crashing: `ctOpus` and `ctOpusLiveBench` on the console (exit code 1), `ctOpusGui` in a message box. `opusenc.exe` runs on any x64 CPU.

```cmd
bin\opusenc --serial 1 --comp 10 input.wav ref.opus
bin\ctOpus  --serial 1 --comp 10 input.wav ct.opus
fc /b ref.opus ct.opus
```

`fc /b` finds no differences. (`--serial 1` fixes the Ogg stream serial number, which opusenc otherwise picks at random.) ctOpus takes opusenc's command line and prints the same messages, see [Command line](#command-line).

### Benchmark on your own files

```cmd
benchmark\run.cmd "D:\Music"
benchmark\run.cmd "D:\Music" -EncodeArgs "--comp 10" -ReferenceArgs "--comp 0"
```

The script encodes every file with opusenc and ctOpus, measures the wall time and checks for every file that the ctOpus output is identical to opusenc with the same settings. It switches to the High Performance power plan during the run (or a temporary copy where the plan is hidden) and restores the previous plan afterwards; if Windows refuses the switch, for example on Windows Server without administrator rights, the report says so and the run goes on with the current plan (`-KeepPowerPlan` leaves the plan alone). The script runs on Windows PowerShell 5.1 and later. Report and CSV go to `benchmark\results\`; exit code 0 means every file was encoded and is bit-identical, 2 that at least one file differs or was not encoded, 1 other errors (missing programs, bad parameters, abort). Results of your hardware are welcome.

---

## Benchmarks

<details>
<summary><b>Machines, method and corpus</b></summary>

### Test machines

| Computer | CPU | Cores / threads | Architecture |
|---|---|---|---|
| Desktop | Intel Core i9-11900K @ 3.50 GHz | 8 / 16 | Rocket Lake |
| Server | Intel Xeon E3-1275 v6 @ 3.80 GHz | 4 / 8 | Kaby Lake |
| Notebook | Intel Core Ultra 7 155H | 16 / 22 (6 P + 8 E + 2 LP-E) | Meteor Lake |

Windows, High Performance power plan during the run (switched by the script where it was not active). The single-file numbers above and the live numbers below are from the desktop.

<!-- BENCHMARK: RAM, drive, Windows version per machine -->

### Method

Measured with `benchmark\Compare-Folder.ps1`: the wall time of the whole process per file (start, reading, resampling, encoding, Ogg packetization, writing), opusenc and ctOpus one after the other per file, round by round, the median per file; totals are the sums of the medians. Every ctOpus output is compared byte for byte with opusenc with the same settings (`--serial 1` for all programs).

Corpus runs: `benchmark\run.cmd <folder> -Rounds 3`, i.e. `--comp 10 --bitrate 128` for both programs, 3 rounds, all threads for ctOpus.

These numbers were measured with the previous build of both programs (dynamic C runtime). The binaries in `bin\` now use the static C runtime; they give the same bits, and on the desktop, one file, all variants interleaved, median of 9, they are as fast or slightly faster: opusenc `--comp 0` −1.4 %, `--comp 10` −0.2 %, ctOpus `--sequential` −0.1 %, ctOpus `--comp 10` −6.3 %, `--comp 0` −2.1 %.

### Corpus

343 WAV files of music, 45.09 hours (162,329 s) in total, mostly stereo CD rips at 44.1 kHz (resampled to 48 kHz by both programs): 314 files of up to 17 min, mostly single tracks of 3 to 6 min (22.0 hours), and 29 whole albums or concerts in one file of 32 to 79 min each (23.1 hours).

<!-- BENCHMARK: sample rate and channel breakdown of the corpus, if wanted -->

</details>

### Complexity

Desktop (i9-11900K), whole corpus, 128 kbit/s; time is the sum of the per-file medians:

| `--comp` | opusenc | ctOpus (all threads) | vs opusenc | identical |
|---|---:|---:|---:|---:|
| 10 | 1,267.1 s (128× real time) | **289.5 s (561× real time)** | **4.38×** | 343 / 343 |

<!-- BENCHMARK: --comp 0 and 5, the --sequential column, and ctOpus --comp 10 against opusenc --comp 0 (-ReferenceArgs "--comp 0") on the corpus -->

### Scaling over the number of threads

<!-- BENCHMARK: --comp 10, 128 kbit/s, ctOpus --threads 1, 2, 4, 6, 7, 8, 12, 16 (and the machine's maximum): time and speedup vs --threads 1 and vs opusenc -->

ctOpus uses its speculation only from 7 threads on (one level at 7, three levels from 8, extra helpers for the levels from 12); below that the gain comes from the analysis chain and the input stage alone.

### Other CPUs

Same corpus, `--comp 10 --bitrate 128`, 3 rounds:

| Computer | opusenc | ctOpus | vs opusenc | identical |
|---|---:|---:|---:|---:|
| Desktop, i9-11900K, 8 / 16 | 1,267.1 s (128×) | **289.5 s (561×)** | **4.38×** | 343 / 343 |
| Server, Xeon E3-1275 v6, 4 / 8 | 1,801.8 s (90×) | **436.9 s (372×)** | **4.12×** | 343 / 343 |
| Notebook, Core Ultra 7 155H, 16 / 22 | 1,399.6 s (116×) | **555.0 s (292×)** | **2.52×** | 343 / 343 |

<details>
<summary><b>Split by file length</b>: the notebook slows down under sustained load</summary>

By file length (the 314 files up to 17 min against the 29 album-length files):

| Computer | tracks (22.0 h) | albums (23.1 h) |
|---|---:|---:|
| Desktop | 4.28× | 4.48× |
| Server | 4.04× | 4.21× |
| Notebook | 3.53× | 1.89× |

On the desktop and the server the long files scale as well as the tracks. On the notebook a track takes about a second or less, but an album file keeps all cores busy for 8 to 19 seconds, and the album files come last in the run, and there ctOpus drops to 1.9× while opusenc on one core stays as fast as on the tracks; the most likely cause is the notebook's sustained power limit, which has not been verified.

</details>

<!-- BENCHMARK: further CPUs (AMD Zen, other hybrid CPUs) -->

### Live latency

<details>
<summary><b>Live latency</b>: encoding time per frame and many streams in real time</summary>

Encoding time per frame for one stream, frames back to back (`ctOpusLiveBench --mode single`, stereo, 64 kbit/s; p50 / p99 in µs, i9-11900K):

| Frame | `--comp 0` | `--comp 5` | `--comp 10` |
|---|---:|---:|---:|
| 2.5 ms | 7.6 / 24 | 15 / 43 | 17 / 50 |
| 10 ms | 21 / 52 | 36 / 78 | 48 / 97 |
| 20 ms | 36 / 71 | 64 / 114 | 89 / 164 |

Many streams in real time (`ctOpusLiveBench --mode threads|tasks`, 20 ms frames, `--comp 10`; latency from a frame's hand-over to its packet):

| Streams | one thread per stream: p50 / p99 / max | misses | one task per stream: p50 / p99 / max | misses |
|---|---:|---:|---:|---:|
| 64 | 0.16 / 0.27 / 1.5 ms | 0 | 0.16 / 0.27 / 1.1 ms | 0 |
| 512 | 0.25 / 0.40 / 1.2 ms | 0 | 0.23 / 0.39 / 1.3 ms | 0 |
| 1,024 | 0.28 / 0.56 / 30.9 ms | 46 | 0.28 / 0.46 / 10.0 ms | 0 |

</details>

<!-- BENCHMARK: live latency on other machines; ctOpusGui mouth-to-ear latency with real devices (shared and exclusive mode) -->

---

## Why this is hard

Parallelizing an Opus encoder is easy if you may change its output: cut the audio into pieces, encode them independently and glue the packets together. ctOpus may not. Its output has to be exactly opusenc's, and Opus is sequential by design:

- every frame continues the **range coder** of the frame before it; which symbols a frame codes, and how many bits it may spend, depends on how many bits all earlier frames used;
- the **energy quantization** predicts each band's energy from the previous frame, and the **bit allocation** and the VBR reservoir carry over from frame to frame;
- the **filter memories** (pre-emphasis, pre-filter, MDCT overlap) and the **tonality analysis** (a small recurrent network) carry state from frame to frame;
- at complexity 8 and above the encoder **tries rounding choices** in the stereo angle search and keeps the cheaper one, so even within a frame the coded result depends on trial encodings.

And "the same output" means the same bits, so every floating point operation must happen in the same order as in libopus, including its SSE code paths and the SSE inner product of the Speex resampler in libopusenc, which sums in a different order than the scalar code.

## How it works

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/graph-dark.svg">
  <img alt="Read on the I/O scheduler feeds Resample ×N; the blocks go to Encode and to the chain Analyse → Signal → Front, whose front frames go to Encode, with a resync back from Encode; Encode hands trials to Theta and clones of the next frames to Speculate ×3, which returns symbol logs" src="docs/images/graph-light.svg">
</picture>

<sub>Rendered with Mermaid's ELK layout; source: [docs/images/graph.mmd](docs/images/graph.mmd).</sub>

**[How the graph grew, step by step →](docs/task-graph.md)** from opusenc's single loop to this graph, with the reason and the measured gain of each step.

Opus is a sequential codec: every frame depends on the state the previous frame left behind (filter memories, energy prediction, bit reservoir, the range coder). ctOpus keeps that sequential backbone exact and takes everything else off it:

1. **Input stage:** WAV reading and the Speex resampler of opusenc (44.1 → 48 kHz) run in parallel blocks, bit-identical to the streaming resampler.
2. **Analysis chain:** tonality analysis, pitch search, transient analysis and the CELT front end (pre-filter, MDCT, band energies) depend only on the signal and run ahead of the encoder on tasks of their own.
3. **Speculative backbone:** while the encoder quantizes frame N, up to three speculation tasks already quantize frames N+1 to N+3 on cloned encoder states, with the outcome of the frames before them guessed. The encoder checks every input of the speculation; if it matches, it replays the recorded range coder symbols instead of computing the frame (about 96 % of the speculated frames). A wrong guess is aborted at the next band.
4. **Theta search helpers:** at complexity 8 and above, the second rounding candidate of the stereo angle search runs on a helper task at the same time as the first.
5. **Bit identity:** the same floating point operations in the same order as libopus, including its SSE code paths. Every change is checked against more than 2,400 reference files (all complexities, many bit rates and options, mono, stereo and surround, SILK, hybrid and CELT modes, several sample rates) and against itself with 1 to 16 threads.

The speedup on one file is limited by the backbone that remains sequential; with fewer than 7 threads ctOpus uses less speculation.

---

## The reference

`bin\opusenc.exe` is the unmodified **opus-tools 0.2** frontend with **libopusenc 0.2.1**, **libogg 1.3.6** and **libopus 1.6.1**, built from the sources in `opus\` with Visual Studio 2026 for x64 (`opus.slnx`): `/O2` with whole program optimization, the static C runtime, no `/arch` switch. Two build choices matter for the bits:

- libopusenc's Speex resampler is built with `__SSE__`, i.e. with its SSE inner product, as GCC and Clang builds of opusenc always are on x64 (the official opus-tools release uses the SSE path as well);
- `opus-tools-0.2\msvc\config.h` is upstream's `win32\config.h` without FLAC support.

ctOpus is checked against this build: more than 2,400 reference encodes (complexities 0 to 10, bit rates, VBR, CVBR and CBR, frame sizes, mono, stereo and surround up to 10 channels, SILK, hybrid and CELT modes, 8 to 32-bit-float input at 32, 44.1 and 48 kHz) must give the same SHA-256, with 1 to 16 threads and with `--sequential`.

---

## Command line

ctOpus takes the command line of opusenc 0.2 and prints the same messages and exit codes; `ctOpus --help` lists everything.

```
ctOpus [options] <input.wav | -> <output.opus | ->
```

`-` stands for standard input or output. Options can be abbreviated as long as they stay unique, and take their value as `--opt value` or `--opt=value`, as in opusenc.

<details>
<summary><b>opusenc's options</b>, all of which ctOpus accepts</summary>

| Option | Meaning |
|---|---|
| `--bitrate n` | target bit rate in kbit/s, 6 to 256 per channel; default 64 for mono, 96 for stereo at 44.1 / 48 kHz (lower for lower sample rates) |
| `--vbr`, `--cvbr`, `--hard-cbr` | variable bit rate (default), constrained VBR, hard constant bit rate |
| `--comp n` (`--complexity n`) | complexity 0 (fastest) to 10 (slowest, default) |
| `--framesize n` | maximum frame size in ms: 2.5, 5, 10, 20 (default), 40, 60 |
| `--music`, `--speech` | tune low bit rates for music or speech (default: automatic) |
| `--expect-loss n` | expected packet loss in percent, 0 (default) to 100 |
| `--downmix-mono`, `--downmix-stereo`, `--no-downmix` | downmix to mono, to stereo (more than 2 channels), or never |
| `--no-phase-inv` | no phase inversion for intensity stereo |
| `--max-delay n` | maximum container delay in ms, 0 to 1000 (default) |
| `--title`, `--artist`, `--album`, `--tracknumber`, `--genre`, `--date` | metadata; `--artist` and `--genre` may be given several times |
| `--comment tag=value` | an extra comment, may be given several times |
| `--padding n` | extra bytes reserved for metadata (default 512) |
| `--discard-comments`, `--discard-pictures` | do not keep metadata or pictures of the input |
| `--ignorelength` | ignore the data length in the WAV header |
| `--serial n` | Ogg stream serial number (default: random) |
| `--set-ctl-int x=y` | pass the encoder control x with value y; `s:x=y` for stream s of a multistream file |
| `--quiet` | no progress output |
| `-h`, `--help`, `--help-picture`, `-V`, `--version`, `--version-short` | help and version |

</details>

Options of ctOpus only (not written to the `ENCODER_OPTIONS` tag, so the output stays identical):

| Option | Meaning |
|---|---|
| `--threads n` | number of worker threads; default: all logical processors |
| `--sequential` | encode on one thread (the reference path) |

Not supported, with an error message and exit code 1: input other than WAV (AIFF, FLAC, raw PCM, so also `--raw`, `--raw-bits`, `--raw-rate`, `--raw-chan`, `--raw-endianness`), `--picture` and `--save-range`. WAV input covers the formats opusenc reads (PCM 8, 16 and 24 bit, 32-bit float, WAVE_FORMAT_EXTENSIBLE, any sample rate, up to 255 channels).

<details>
<summary><b>ctOpusGui and ctOpusLiveBench</b>: their command lines</summary>

### ctOpusGui

`ctOpusGui.exe` is used from its window; the command line is for unattended runs:

| Option | Meaning |
|---|---|
| `--autostart` | start the live stream with the default settings (test signal, no playback) |
| `--load n` | start the multi-stream load test with n streams |
| `--engine i` | entry i of the encoder list (live: 0 ctOpus, 1 libopus; load test: 0 to 3) |
| `--verify` | compare every ctOpus packet byte for byte with libopus |
| `--exclusive` | open the audio devices in exclusive mode (falls back to shared mode) |
| `--playback` | play the decoded stream on the default output device |
| `--silent` | with `--playback`: run the whole playback path, but send silence to the device |
| `--seconds s` | stop and close after s seconds |
| `--report file` | write a summary to the file when closing |

### ctOpusLiveBench

```
ctOpusLiveBench <input.wav> [options]
```

| Option | Meaning |
|---|---|
| `--mode m` | `single`: one stream, frames back to back (the encoding time); `threads`: one OS thread per stream; `tasks`: one task per stream on the scheduler; `resample`: self-test of the live resampler. Default `tasks` |
| `--streams n` | number of streams in real time (default 64) |
| `--frame ms` | frame size in ms (default 20) |
| `--comp n` | complexity (default 10) |
| `--bitrate n` | bit rate per stream in kbit/s (default 64) |
| `--seconds s` | length of the run (default 10) |
| `--threads n` | scheduler threads for `tasks` (default: all logical processors) |
| `--helper` | `single`: use the theta search helper |
| `--gap ms` | `single`: pause between the frames (with `--spin 0` busy instead of sleeping) |
| `--spin ms` | `threads`: busy-wait this long before a frame is due |
| `--csv` | one CSV line instead of the report |

Input for ctOpusLiveBench: 16-bit stereo WAV.

</details>

---

## Live encoding (`ctOpusGui.exe`, `ctOpusLiveBench.exe`)

Live encoding has no look-ahead, so the techniques above do not apply. What counts there is the time from a frame's arrival to its packet:

* **Encoding time per frame** (one stream, comp 10, i9-11900K): about 17 µs for 2.5 ms frames and 90 µs for 20 ms frames. The audio device period (3 ms exclusive, 10 ms shared on typical hardware) and the frame length dominate the latency, not the encoder.
* **Many streams:** one task per stream on the scheduler is at least as good as one thread per stream; with 1,024 streams of 20 ms frames, tasks had no deadline misses, threads 46 (outliers up to 31 ms).
* **`ctOpusGui.exe`:** live encoding from a microphone, a WAV file or a test signal (WASAPI, shared or exclusive mode), monitoring through the decoder with clock drift compensation, latency breakdown, and a load test with N streams (ctOpus or libopus, tasks or threads).
* **`ctOpusLiveBench.exe`:** command line latency benchmark, e.g. `ctOpusLiveBench input.wav --mode tasks --streams 256 --frame 20`.

---

## Room for improvement

ctOpus is not finished; the measurements point to these next steps:

* **The sequential backbone:** the quantization of the bands (`quant_all_bands` with the range coder) stays sequential from frame to frame. Speculation runs it ahead, but on the i9-11900K ctOpus still kept only about 3.7 of the 8 cores busy on average (VTune, a build with two speculation levels), and one thread is only 10 % faster than opusenc (2.00 s against 2.21 s at `--comp 10`).
* **Turbo clock:** every additional busy core lowers the clock of all of them. The band quantization itself took about 5 % longer with 2 busy cores and 10 % longer with 6; more speculation levels or helpers therefore pay off only up to a point (three levels from 8 threads, helpers for the levels only from 12).
* **Fewer than 7 threads:** below 7 threads speculation costs more than it gains and is switched off, so CPUs with 4 cores / 4 threads get only the analysis chain and the input stage.
* **SILK and hybrid modes:** the speculation covers CELT frames only; speech at low bit rates (SILK, hybrid) runs the backbone without it. These modes are bit-identical but their speedup has not been measured separately.
* **Notebooks under sustained load:** on the Core Ultra 7 155H ctOpus is 3.5× faster than opusenc on single tracks but only 1.9× on album-length files; with fewer threads ctOpus might hold a higher clock for longer. This has not been measured yet.
* **CPUs without AVX2:** `ctOpus.exe` is built for AVX2; a build for older CPUs has not been made or checked.
* **Live encoding:** a frame takes 20 to 90 µs, too little to split over cores profitably; the theta helper brought no gain there. A frame that arrives after an idle pause takes 1.4 to 2 times longer than one encoded back to back (the core's caches are cold and its clock is low). Encoding the tonality analysis next to the CELT front end of the same frame would save at most about 15 % at 20 ms frames.
* **Tried and dropped:** wider SIMD in the pulse search and the band rotation was bit-identical but slower, because the bands are short; profile-guided optimization gained less than 1 %.

Every step keeps the output bit-identical to opusenc.

---

## ThinkMeta.ConcurrentTasks

### Why fibers here

Encoding one file runs about a dozen stages at once: the resampler blocks, the analysis chain that runs ahead of the encoder, up to three speculation levels and the theta helpers. Each is written as a plain sequential loop that waits on an event for its next frame or job; while it waits, its worker thread runs another stage, so the stages share one thread per logical processor instead of needing a thread each. A speculation that turns out wrong is not cancelled from outside: the task checks an abort flag at every band boundary and goes back to waiting for its next frame. For live encoding, one task per stream was measured against one thread per stream (`ctOpusLiveBench`, 1,024 stereo streams at 64 kbit/s, 20 ms frames, `--comp 10`, i9-11900K): tasks had no deadline misses, threads 46, with outliers up to 31 ms.

`ctOpus` is a showcase for **ThinkMeta.ConcurrentTasks**, a fiber-based task scheduler and concurrency runtime by ThinkMeta Software GmbH. Tasks run on lightweight user-mode fibers: a task waiting for a frame, an event or I/O yields without blocking its worker thread, and a task switch costs a few hundred nanoseconds. That makes it practical to split a frame of 50 to 200 µs into pipeline stages and speculative work.

👉 **Get in touch:** [www.thinkmeta.com](https://www.thinkmeta.com)

---

## License

* **Reference sources and `bin\opusenc.exe`** (`opus\`): BSD-style licenses of Xiph.Org and the Opus contributors ([`COPYING`](COPYING) and the files in each source folder).
* **ctOpus binaries and scheduler core** (`bin\ctOpus.exe`, `bin\ctOpusGui.exe`, `bin\ctOpusLiveBench.exe`, `bin\ThinkMeta.ConcurrentTasks.Core.dll`): proprietary, Copyright (c) 2026 ThinkMeta Software GmbH, under the same provisional terms ([`LICENSE-Core.txt`](LICENSE-Core.txt)): free for private and non-commercial use, commercial use requires a license from ThinkMeta Software GmbH. The ctOpus programs contain code ported from libopus and opus-tools, whose BSD notices are in [`COPYING`](COPYING). The source code of ctOpus is planned to be published under the MIT License.
* **Benchmark scripts** (`benchmark\`, `New-BenchPackage.ps1`): MIT License, Copyright (c) 2026 ThinkMeta Software GmbH ([`LICENSE.txt`](LICENSE.txt)).

**Benchmarks are explicitly permitted:** anyone may run benchmark tests of any kind with the programs in `bin\`, on any hardware and for any purpose, commercial evaluation included, and publish the results, including comparisons with other software, without asking ThinkMeta Software GmbH first (point 2 of [`LICENSE-Core.txt`](LICENSE-Core.txt)).
