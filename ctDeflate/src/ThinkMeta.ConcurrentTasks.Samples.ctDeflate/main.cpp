//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include <Windows.h>

#include <Psapi.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>

#include "Compressor.h"
#include "DeflateGraph.h"
#include "ParallelDeflate.h"

using namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate;
namespace Ct = ThinkMeta::ConcurrentTasks;

namespace {

    void PrintUsage()
    {
        std::fwprintf(stderr,
                      L"Usage: ctDeflate [-0..-9] [-o OUTPUT] [--threads N] [--sequential] [--in-memory] [--chunk BYTES] [-v] FILE\n"
                      L"Compresses FILE to FILE.gz (or OUTPUT), bit-identical to libdeflate 1.26 gzip.\n"
                      L"  -LEVEL        compression level 0..9 (default 6)\n"
                      L"  -o            output file\n"
                      L"  --threads N   worker threads (default: all logical processors)\n"
                      L"  --sequential  the sequential graph: one compressor task, streaming\n"
                      L"  --in-memory   parallel, but read the whole file first and write the output at the end\n"
                      L"  --chunk BYTES speculative parse chunk size (default 1 MiB)\n"
                      L"  --ahead N     chunks parsed ahead of the resolver (default threads + 4)\n"
                      L"  --read-block KB, --read-depth N, --read-grow, --write-block KB, --write-depth N, --write-grow\n"
                      L"                block size and requests in flight of the reads and writes (default 1024 KB x 4);\n"
                      L"                grow: start with 64 KB and double up to the block size\n"
                      L"  --library-io  read and write with AsyncFile::ReadFile/WriteFile (their default, 1 MB x 5)\n"
                      L"  -v            print size, time (including file I/O), peak memory and pipeline counters\n");
    }

    bool ReadWholeFile(const wchar_t* path, std::unique_ptr<std::uint8_t[]>& data, std::size_t& size)
    {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER fileSize;
        bool          ok = GetFileSizeEx(file, &fileSize) != 0;
        if (ok) {
            size             = static_cast<std::size_t>(fileSize.QuadPart);
            data             = std::make_unique_for_overwrite<std::uint8_t[]>(size + 1);
            std::size_t done = 0;
            while (ok && done < size) {
                const auto    chunk = static_cast<unsigned long>(size - done < (1u << 30) ? size - done : (1u << 30));
                unsigned long read  = 0; // DWORD of ReadFile
                ok                  = ReadFile(file, data.get() + done, chunk, &read, nullptr) != 0 && read == chunk;
                done += read;
            }
        }
        CloseHandle(file);
        return ok;
    }

    bool WriteWholeFile(const wchar_t* path, const std::uint8_t* data, std::size_t size)
    {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        bool        ok   = true;
        std::size_t done = 0;
        while (ok && done < size) {
            const auto    chunk   = static_cast<unsigned long>(size - done < (1u << 30) ? size - done : (1u << 30));
            unsigned long written = 0; // DWORD of WriteFile
            ok                    = WriteFile(file, data + done, chunk, &written, nullptr) != 0 && written == chunk;
            done += written;
        }
        CloseHandle(file);
        return ok;
    }

    Ct::ITaskScheduler* StartScheduler(std::uint32_t threads)
    {
        Ct::SchedulerInfo info;
        info.numberOfThreads      = threads;
        Ct::ITaskScheduler* sched = Ct::CreateTaskScheduler(Ct::SchedulerKind::Default, &info);
        if (sched != nullptr)
            sched->Start();
        return sched;
    }

    void StopScheduler(Ct::ITaskScheduler* sched)
    {
        if (sched != nullptr) {
            sched->Stop();
            sched->Destroy();
        }
    }

    /// <summary>
    /// Runs work(io, compute) on two schedulers that live for this call: two threads for the file I/O, threads for the
    /// compression. False if they cannot be created.
    /// </summary>
    template <typename Work>
    bool OnSchedulers(std::uint32_t threads, Work&& work)
    {
        if (!Ct::AsyncIo::EnableAsyncIo()) // the AsyncFile completions of Read and Write
            return false;
        Ct::ConvertThreadToTask();
        // Two I/O threads: a write that extends the file runs synchronously in NTFS and must not hold up the reads.
        Ct::ITaskScheduler* io      = StartScheduler(2);
        Ct::ITaskScheduler* compute = StartScheduler(threads);
        const bool          ok      = io != nullptr && compute != nullptr;
        if (ok)
            work(*io, *compute);
        StopScheduler(compute);
        StopScheduler(io);
        Ct::ConvertTaskToThread();
        Ct::AsyncIo::DisableAsyncIo();
        return ok;
    }

    void PrintStats(const ParallelStats& stats)
    {
        std::fwprintf(stderr,
                      L"  chunks %zu, blocks %zu, syncs %zu, repairs %zu (%zu bytes), max sync distance %zu; parse+resolve %.3f s (resolver waited %.3f s), "
                      L"encode %.3f s, stitch %.3f s\n",
                      stats.chunks, stats.blocks, stats.resolve.syncs, stats.resolve.repairs, stats.resolve.repairedBytes, stats.resolve.maxSyncDistance,
                      stats.parseResolve, stats.resolveWait, stats.encode, stats.stitch);
        std::fwprintf(stderr, L"  repairs by cause: minLen %zu, parse ended %zu; iterations by minLen:", stats.resolve.minLenMismatches,
                      stats.resolve.parseExhausted);
        for (std::int32_t m = 0; m < 10; ++m)
            if (stats.resolve.minLenIterations[m] != 0)
                std::fwprintf(stderr, L" %d:%zu/%zuKB", m, stats.resolve.minLenIterations[m], stats.resolve.minLenBytes[m] >> 10);
        std::fwprintf(stderr, L"\n");
    }

    double PeakMemoryMB()
    {
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? static_cast<double>(counters.PeakWorkingSetSize) / 1e6 : 0.0;
    }

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::int32_t   level      = 6;
    const wchar_t* input      = nullptr;
    const wchar_t* output     = nullptr;
    bool           verbose    = false;
    bool           sequential = false;
    bool           inMemory   = false;
    bool           libraryIo  = false; // --library-io: AsyncFile::ReadFile/WriteFile instead of the own read and write loops
    IoShape        readShape;          // --read-block KB, --read-depth N, --read-grow (I/O measurements)
    IoShape        writeShape;         // --write-block KB, --write-depth N, --write-grow
    std::uint32_t  threads = 0;
    std::uint32_t  repeat  = 1; // --repeat N: compress N times (profiling)
    ChunkOptions   options;

    for (std::int32_t i = 1; i < argc; ++i) {
        const wchar_t* arg = argv[i];
        if (arg[0] == L'-' && arg[1] >= L'0' && arg[1] <= L'9' && arg[2] == 0) {
            level = arg[1] - L'0';
        } else if (std::wcscmp(arg, L"-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (std::wcscmp(arg, L"--threads") == 0 && i + 1 < argc) {
            threads = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (std::wcscmp(arg, L"--chunk") == 0 && i + 1 < argc) {
            options.chunkSize = static_cast<std::size_t>(std::wcstoull(argv[++i], nullptr, 10));
        } else if (std::wcscmp(arg, L"--ahead") == 0 && i + 1 < argc) {
            options.ahead = static_cast<std::size_t>(std::wcstoull(argv[++i], nullptr, 10));
        } else if (std::wcscmp(arg, L"--minlens") == 0 && i + 1 < argc) {
            options.minLens = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 0));
        } else if (std::wcscmp(arg, L"--repeat") == 0 && i + 1 < argc) {
            repeat = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (std::wcscmp(arg, L"--sequential") == 0) {
            sequential = true;
        } else if (std::wcscmp(arg, L"--in-memory") == 0) {
            inMemory = true;
        } else if (std::wcscmp(arg, L"--library-io") == 0) {
            libraryIo = true;
        } else if ((std::wcscmp(arg, L"--read-block") == 0 || std::wcscmp(arg, L"--write-block") == 0) && i + 1 < argc) {
            (arg[2] == L'r' ? readShape : writeShape).block = static_cast<std::size_t>(std::wcstoull(argv[++i], nullptr, 10)) << 10;
        } else if ((std::wcscmp(arg, L"--read-depth") == 0 || std::wcscmp(arg, L"--write-depth") == 0) && i + 1 < argc) {
            (arg[2] == L'r' ? readShape : writeShape).depth = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (std::wcscmp(arg, L"--read-grow") == 0 || std::wcscmp(arg, L"--write-grow") == 0) {
            (arg[2] == L'r' ? readShape : writeShape).grow = true;
        } else if (std::wcscmp(arg, L"-v") == 0) {
            verbose = true;
        } else if (arg[0] == L'-') {
            PrintUsage();
            return 1;
        } else if (input == nullptr) {
            input = arg;
        } else {
            PrintUsage();
            return 1;
        }
    }
    if (input == nullptr || options.chunkSize < 4096 || readShape.block < 4096 || writeShape.block < 4096 || readShape.depth - 1 >= IoShape::MaxDepth
        || writeShape.depth - 1 >= IoShape::MaxDepth) {
        PrintUsage();
        return 1;
    }
    if (threads == 0)
        threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);

    wchar_t defaultOutput[MAX_PATH + 4];
    if (output == nullptr) {
        if (std::swprintf(defaultOutput, MAX_PATH + 4, L"%s.gz", input) < 0) {
            std::fwprintf(stderr, L"ctDeflate: path too long: %s\n", input);
            return 1;
        }
        output = defaultOutput;
    }

    ParallelStats stats;
    LARGE_INTEGER freq;
    LARGE_INTEGER t0;
    LARGE_INTEGER t1;
    QueryPerformanceFrequency(&freq);
    std::uint64_t inSize  = 0;
    std::uint64_t outSize = 0;

    if (inMemory) {
        std::unique_ptr<std::uint8_t[]> in;
        std::size_t                     size = 0;
        if (!ReadWholeFile(input, in, size)) {
            std::fwprintf(stderr, L"ctDeflate: cannot read %s\n", input);
            return 1;
        }
        inSize   = size;
        auto out = std::make_unique_for_overwrite<std::uint8_t[]>(Compressor::GzipBound(size));
        QueryPerformanceCounter(&t0);
        for (std::uint32_t r = 0; r < (repeat != 0 ? repeat : 1); ++r) {
            if (!OnSchedulers(threads, [&](Ct::ITaskScheduler&, Ct::ITaskScheduler& sched) {
                    outSize = CompressGzipParallel(sched, threads, level, in.get(), size, out.get(), options, &stats);
                }))
                outSize = 0;
        }
        QueryPerformanceCounter(&t1);
        if (outSize == 0) {
            std::fwprintf(stderr, L"ctDeflate: compression failed\n");
            return 1;
        }
        if (!WriteWholeFile(output, out.get(), static_cast<std::size_t>(outSize))) {
            std::fwprintf(stderr, L"ctDeflate: cannot write %s\n", output);
            return 1;
        }
    } else {
        // The task graph from file to file: DeflateGraph.h.
        DeflateGraph::Result result;
        QueryPerformanceCounter(&t0);
        for (std::uint32_t r = 0; r < (repeat != 0 ? repeat : 1); ++r) {
            if (!OnSchedulers(sequential ? 1 : threads, [&](Ct::ITaskScheduler& io, Ct::ITaskScheduler& compute) {
                    DeflateGraph graph({.level      = level,
                                        .workers    = threads,
                                        .options    = options,
                                        .sequential = sequential,
                                        .libraryIo  = libraryIo,
                                        .readShape  = readShape,
                                        .writeShape = writeShape});
                    result.error = graph.OpenInput(input);
                    if (result.error == 0)
                        result.error = graph.OpenOutput(output);
                    if (result.error == 0)
                        result = graph.Run(io, compute, &stats);
                })) {
                std::fwprintf(stderr, L"ctDeflate: cannot create the scheduler\n");
                return 1;
            }
        }
        QueryPerformanceCounter(&t1);
        if (result.error != 0) {
            wchar_t message[256] = L"";
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, result.error, 0, message, 256, nullptr);
            std::fwprintf(stderr, L"ctDeflate: %s -> %s: error %u %s", input, output, result.error, message);
            DeleteFileW(output);
            return 1;
        }
        inSize  = result.inSize;
        outSize = result.outSize;
    }

    if (verbose) {
        const double seconds = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
        std::fwprintf(stderr, L"%s: %llu -> %llu bytes, %.3f s, %.1f MB/s, peak memory %.0f MB\n", input, inSize, outSize, seconds,
                      seconds > 0 ? static_cast<double>(inSize) / seconds / 1e6 : 0.0, PeakMemoryMB());
        if (!sequential)
            PrintStats(stats);
    }
    return 0;
}
