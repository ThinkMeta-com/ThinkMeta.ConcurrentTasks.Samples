# Shared

Code that every sample in this repository uses, on Windows and Linux (x64). The samples build it from source as a
static library: on Windows `src\Shared\ThinkMeta.ConcurrentTasks.Samples.Shared.vcxproj` (part of each sample's
solution), on Linux `CMakeLists.txt` of this folder, which the `CMakeLists.txt` of a sample adds through
`Standalone.cmake`.

Text is UTF-16 (`wchar_t`) on Windows and UTF-8 (`char`) on Linux: `Text.h` defines the character type `Char`, the
literal macro `CT_TEXT("...")` and `CT_MAIN` (`wmain` or `main`), so the samples have one source for both.

| File | Contents |
|---|---|
| `src\Shared\Text.h` | `Char`, `CT_TEXT`, `CT_MAIN`, and `Text`: length, compare, find, copy and number parsing of `Char` text, the same results on both platforms. |
| `src\Shared\Console.h` | `Console`: text to stdout or stderr, printf-style. On Windows a console gets UTF-16 (`WriteConsoleW`), a file or pipe UTF-8 with `\r\n` line ends; on Linux UTF-8 with `\n`. |
| `src\Shared\Formatter.h` | `Formatter`: printf-style formatting into a buffer (narrow UTF-8 and wide), the same result as the C runtime (UCRT on Windows, glibc on Linux) for the conversions the samples use, floating point included. |
| `src\Shared\Memory.h` | `Heap` (allocate, reallocate, free) and `AddressSpace` (reserve, commit, decommit, release) over the Windows heap and `VirtualAlloc`, or `malloc` and `mmap` on Linux. |
| `src\Shared\Compiler.h` | `CT_FORCEINLINE` for MSVC and clang. |
| `src\Shared\SchedulerHandle.h` | `SchedulerHandle`: owns a scheduler of ThinkMeta.ConcurrentTasks and destroys it at the end of its lifetime; Start and Stop stay explicit. A header of its own: it uses the core. |
| `src\Shared\TaskRuntime.h` | `TaskRuntime`: the setup of ThinkMeta.ConcurrentTasks of every sample (thread to task, async I/O, a started I/O scheduler), undone in reverse order by `End` or the destructor; `RunningScheduler` for the compute scheduler; `LogicalProcessors` as the default thread count. A header of its own: it uses the core. |
| `src\Shared\File.h` | `File`: whole files at once (`Size`, `Read`, `Write`, `Delete`), `Same` (the same file under two names) and `ErrorText` (the text of a Windows error code or an `errno` value). |
| `src\Shared\RunStats.h` | `RunStats`: the first line of `--stats`, the same in every sample: total time split into process start, setup, work (the task graph with its file I/O) and rest, the thread count and the peak memory. |
| `src\Shared\Diagnostics.h` | `Diagnostics`: `Now` (performance counter or `CLOCK_MONOTONIC`), `SinceProcessStart` and `PeakMemoryMB` for the statistics of `--stats`. With `CT_SAMPLES_NO_TIMING` defined, the time functions return 0 and read no clock. |
| `src\Shared\VersionInfo.h`, `src\Shared\VersionInfo.rc2` | The version resource of every Windows program: company and copyright in `VersionInfo.h`; the `.rc` of a program defines its version, name and description and includes `VersionInfo.rc2`. |
| `src\Shared\TestEnvironment.h` | `TestEnvironment`: the environment variables of the unit tests, the same scheme in every sample: `CT<SAMPLE>_TEST_<WHAT>` names input of a correctness test (file, folder or list), `CT<SAMPLE>_BENCH_<WHAT>` switches on a measurement; without them the test logs `skipped: set NAME (...)`. |
| `src\Shared\ParseResult.h` | `ParseResult`: what reading the command line of a sample leads to (run, exit with 0, exit with 1). |
| `src\Shared\CpuFeatures.h` | `CpuFeatures`: what the CPU and the operating system support (CPUID, XGETBV); the base of `CpuCheck` and of the SIMD level a sample chooses, and the line of `--features`. |
| `src\Shared\CpuCheck.h` | `CpuCheck` for the programs built for AVX2: it checks the instruction sets before any other code of the program runs and stops with exit code 3 on an older CPU. On Windows through the entry points `CpuCheckedConsoleStartup` / `CpuCheckedWindowsStartup` (linker `/ENTRY`), on Linux through a function in `.preinit_array` (`CpuCheckConsole.cpp`, the CMake target `ThinkMeta.ConcurrentTasks.Samples.Shared.CpuCheck`). |
| `src\Shared\DebugReports.h` | `DebugReports`: in Windows debug builds, assertions of the C runtime go to stderr instead of a dialog. |
| `src\Tests\` | Unit tests: `Formatter` against `snprintf` and `swprintf` of the C runtime, with millions of values, and `Text`. |
| `CMakeLists.txt` | The Linux build of the library, its tests and the build rules all samples share (no FMA contraction, the AVX2 switches, how a program links the core). |
| `Standalone.cmake` | The setup of a sample built on its own on Linux: C++ standard, warnings, ThinkMeta.ConcurrentTasks from the API submodule, this folder, the unit tests. |

On Windows the library calls no function of the C runtime, only the Windows API, so it links into programs with the
static or the DLL runtime; on Linux it uses the C library and POSIX. `DebugReports`, `SchedulerHandle`,
`TaskRuntime`, `Diagnostics`, `RunStats` and `TestEnvironment` are headers only for that reason: they call the C
runtime or the core of ThinkMeta.ConcurrentTasks, or depend on a setting of the program (`CT_SAMPLES_NO_TIMING`), and
are compiled into the program. `ParseResult` is only an enumeration.

The interfaces are preliminary.

## License

MIT, see [LICENSE.txt](LICENSE.txt).
