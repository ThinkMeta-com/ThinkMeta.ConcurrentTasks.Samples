# Shared

Code that every sample in this repository uses. The samples build it from source as a static library
(`src\Shared\ThinkMeta.ConcurrentTasks.Samples.Shared.vcxproj`, part of each sample's solution).

| File | Contents |
|---|---|
| `src\Shared\Console.h` | `Console`: text to stdout or stderr, printf-style. A console gets UTF-16 (`WriteConsoleW`), a file or pipe UTF-8 with `\r\n` line ends. |
| `src\Shared\Formatter.h` | `Formatter`: printf-style formatting into a buffer (narrow UTF-8 and wide), the same result as the C runtime for the conversions the samples use, floating point included. |
| `src\Shared\SchedulerHandle.h` | `SchedulerHandle`: owns a scheduler of ThinkMeta.ConcurrentTasks and destroys it at the end of its lifetime; Start and Stop stay explicit. A header of its own: it uses the core. |
| `src\Shared\TaskRuntime.h` | `TaskRuntime`: the setup of ThinkMeta.ConcurrentTasks of every sample (thread to task, async I/O, a started I/O scheduler), undone in reverse order by `End` or the destructor; `RunningScheduler` for the compute scheduler; `LogicalProcessors` as the default thread count. A header of its own: it uses the core. |
| `src\Shared\File.h` | `File`: whole files at once (`Size`, `Read`, `Write`), `Same` (the same file under two names) and `ErrorText` (the text of a Windows error code). |
| `src\Shared\RunStats.h` | `RunStats`: the first line of `--stats`, the same in every sample: total time split into process start, setup, work (the task graph with its file I/O) and rest, the thread count and the peak memory. |
| `src\Shared\Diagnostics.h` | `Diagnostics`: `Now` (performance counter), `SinceProcessStart` and `PeakMemoryMB` for the statistics of `--stats`. With `CT_SAMPLES_NO_TIMING` defined, the time functions return 0 and read no clock. |
| `src\Shared\VersionInfo.h`, `src\Shared\VersionInfo.rc2` | The version resource of every program: company and copyright in `VersionInfo.h`; the `.rc` of a program defines its version, name and description and includes `VersionInfo.rc2`. |
| `src\Shared\TestEnvironment.h` | `TestEnvironment`: the environment variables of the unit tests, the same scheme in every sample: `CT<SAMPLE>_TEST_<WHAT>` names input of a correctness test (file, folder or list), `CT<SAMPLE>_BENCH_<WHAT>` switches on a measurement; without them the test logs `skipped: set NAME (...)`. |
| `src\Shared\ParseResult.h` | `ParseResult`: what reading the command line of a sample leads to (run, exit with 0, exit with 1). |
| `src\Shared\CpuFeatures.h` | `CpuFeatures`: what the CPU and the operating system support (CPUID, XGETBV); the base of `CpuCheck` and of the SIMD level a sample chooses, and the line of `--features`. |
| `src\Shared\CpuCheck.h` | `CpuCheck` and the entry points `CpuCheckedConsoleStartup` / `CpuCheckedWindowsStartup` (linker `/ENTRY`) of the programs built with `/arch:AVX2`: they check the instruction sets before the C runtime starts and stop with exit code 3 on an older CPU. |
| `src\Shared\DebugReports.h` | `DebugReports`: in debug builds, assertions of the C runtime go to stderr instead of a dialog. |
| `src\Tests\` | Unit tests: `Formatter` against `snprintf` and `swprintf` of the C runtime, with millions of values. |

The library calls no function of the C runtime, only the Windows API, so it links into programs
with the static or the DLL runtime. `DebugReports`, `SchedulerHandle`, `TaskRuntime`, `Diagnostics`,
`RunStats` and `TestEnvironment` are headers only for that reason: they call the C runtime or the core
of ThinkMeta.ConcurrentTasks, or depend on a setting of the program (`CT_SAMPLES_NO_TIMING`), and are
compiled into the program. `ParseResult` is only an enumeration.

The interfaces are preliminary.

## License

MIT, see [LICENSE.txt](LICENSE.txt).
