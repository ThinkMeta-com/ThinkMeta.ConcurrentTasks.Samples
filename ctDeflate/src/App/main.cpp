//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include <Windows.h>

#include <Psapi.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>

#include "Compressor.h"

// headers with imports last
#include "DeflateGraph.h"
#include "SchedulerHandle.h"

// ctDeflate: the gzip compression of libdeflate 1.26 as a bit-identical C++ port on ThinkMeta.ConcurrentTasks, one
// file on all cores. The task graph is DeflateGraph (DeflateGraph.h); this file parses the command line, sets up the
// schedulers and runs the graph once.

using namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate;
namespace Ct = ThinkMeta::ConcurrentTasks;

namespace {

    void PrintUsage()
    {
        std::fwprintf(stderr,
                      L"Usage: ctDeflate [-0..-9] [-o OUTPUT] [--threads N] [--sequential] [--in-memory] [--chunk BYTES] [--ahead N] [-v] FILE\n"
                      L"Compresses FILE to FILE.gz (or OUTPUT), bit-identical to libdeflate 1.26 gzip.\n"
                      L"  -LEVEL        compression level 0..9 (default 6)\n"
                      L"  -o            output file\n"
                      L"  --threads N   worker threads (default: all logical processors)\n"
                      L"  --sequential  the sequential graph: one compressor task, streaming\n"
                      L"  --in-memory   read the whole file first and write the output at the end\n"
                      L"  --chunk BYTES speculative parse chunk size (default 1 MiB, at least 4096)\n"
                      L"  --ahead N     chunks parsed ahead of the resolver (default threads + 4)\n"
                      L"  -v            print size, time (including file I/O), peak memory and pipeline counters\n");
    }

    struct Options {
        std::int32_t   level      = 6;
        const wchar_t* input      = nullptr;
        const wchar_t* output     = nullptr; // FILE.gz unless -o
        std::uint32_t  threads    = 0;       // 0: one per logical processor
        bool           sequential = false;
        bool           inMemory   = false;
        bool           verbose    = false;
        ChunkOptions   chunks;
    };

    /// <summary>False on an unknown option or a missing file name.</summary>
    bool ParseArgs(std::int32_t argc, wchar_t** argv, Options& o)
    {
        for (std::int32_t i = 1; i < argc; ++i) {
            const wchar_t* arg = argv[i];
            if (arg[0] == L'-' && arg[1] >= L'0' && arg[1] <= L'9' && arg[2] == 0)
                o.level = arg[1] - L'0';
            else if (std::wcscmp(arg, L"-o") == 0 && i + 1 < argc)
                o.output = argv[++i];
            else if (std::wcscmp(arg, L"--threads") == 0 && i + 1 < argc)
                o.threads = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
            else if (std::wcscmp(arg, L"--chunk") == 0 && i + 1 < argc)
                o.chunks.chunkSize = static_cast<std::size_t>(std::wcstoull(argv[++i], nullptr, 10));
            else if (std::wcscmp(arg, L"--ahead") == 0 && i + 1 < argc)
                o.chunks.ahead = static_cast<std::size_t>(std::wcstoull(argv[++i], nullptr, 10));
            else if (std::wcscmp(arg, L"--sequential") == 0)
                o.sequential = true;
            else if (std::wcscmp(arg, L"--in-memory") == 0)
                o.inMemory = true;
            else if (std::wcscmp(arg, L"-v") == 0)
                o.verbose = true;
            else if (arg[0] == L'-' || o.input != nullptr)
                return false;
            else
                o.input = arg;
        }
        if (o.threads == 0)
            o.threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        return o.input != nullptr && o.chunks.chunkSize >= 4096;
    }

    /// <summary>True if both paths name the same existing file (same volume and file ID, so also through links).</summary>
    bool SameFile(const wchar_t* a, const wchar_t* b)
    {
        BY_HANDLE_FILE_INFORMATION info[2]{};
        const wchar_t*             paths[2] = {a, b};
        for (std::int32_t i = 0; i < 2; ++i) {
            HANDLE file = CreateFileW(paths[i], 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            const bool ok = GetFileInformationByHandle(file, &info[i]) != 0;
            CloseHandle(file);
            if (!ok)
                return false;
        }
        return info[0].dwVolumeSerialNumber == info[1].dwVolumeSerialNumber && info[0].nFileIndexHigh == info[1].nFileIndexHigh
               && info[0].nFileIndexLow == info[1].nFileIndexLow;
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

    void PrintStats(const ParallelStats& stats)
    {
        std::fwprintf(stderr,
                      L"  chunks %zu, blocks %zu, syncs %zu, repairs %zu (%zu bytes), max sync distance %zu; parse+resolve %.3f s (resolver waited %.3f s), "
                      L"encode %.3f s, stitch %.3f s\n",
                      stats.chunks, stats.blocks, stats.resolve.syncs, stats.resolve.repairs, stats.resolve.repairedBytes, stats.resolve.maxSyncDistance,
                      stats.parseResolve, stats.resolveWait, stats.encode, stats.stitch);
        std::fwprintf(stderr, L"  repairs by cause: minLen %zu, parse ended %zu; iterations by minLen:", stats.resolve.minLenMismatches,
                      stats.resolve.parseExhausted);
        for (std::int32_t m = 0; m <= static_cast<std::int32_t>(MaxMinLen); ++m)
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

    /// <summary>
    /// The setup of ThinkMeta.ConcurrentTasks, the same in every sample; runs work(io, compute) in it. False if a part of
    /// the setup failed.
    /// <list type="number">
    /// <item>ConvertThreadToTask: this thread becomes a task, so it can wait for the graph (TaskGroup::Wait) and run its
    /// serial node Resolve, which waits on events.</item>
    /// <item>EnableAsyncIo: the core's completion port, for the overlapped file I/O of the nodes Read and Write.</item>
    /// <item>An I/O scheduler for Read and Write and a compute scheduler for the workers. The I/O scheduler has two
    /// threads here: a write that extends the file runs synchronously in NTFS and must not hold up the reads.</item>
    /// <item>Start both, run the graph, stop both once every task has exited; then undo 2 and 1.</item>
    /// </list>
    /// </summary>
    template <typename Work>
    bool WithSchedulers(std::uint32_t computeThreads, Work&& work)
    {
        Ct::ConvertThreadToTask();
        const bool asyncIo = Ct::AsyncIo::EnableAsyncIo();
        bool       ok      = false;
        if (asyncIo) {
            SchedulerHandle io      = SchedulerHandle::Create(Ct::SchedulerKind::Default, 2);
            SchedulerHandle compute = SchedulerHandle::Create(Ct::SchedulerKind::Default, computeThreads);
            ok                      = io && compute;
            if (ok) {
                io->Start();
                compute->Start();
                work(*io.Get(), *compute.Get());
                compute->Stop();
                io->Stop();
            }
            Ct::AsyncIo::DisableAsyncIo();
        }
        Ct::ConvertTaskToThread();
        return ok;
    }

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options o;
    if (!ParseArgs(argc, argv, o)) {
        PrintUsage();
        return 1;
    }
    wchar_t defaultOutput[MAX_PATH + 4];
    if (o.output == nullptr) {
        if (std::swprintf(defaultOutput, MAX_PATH + 4, L"%s.gz", o.input) < 0) {
            std::fwprintf(stderr, L"ctDeflate: path too long: %s\n", o.input);
            return 1;
        }
        o.output = defaultOutput;
    }
    if (SameFile(o.input, o.output)) {
        std::fwprintf(stderr, L"ctDeflate: %s: input and output are the same file\n", o.input);
        return 1;
    }

    const DeflateGraph::Settings settings{.level = o.level, .workers = o.threads, .chunks = o.chunks, .sequential = o.sequential};
    ParallelStats                stats;
    DeflateGraph::Result         result;
    bool                         created = false; // this run created the output: delete it on an error, and nothing else
    LARGE_INTEGER                freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    if (o.inMemory) {
        // The same graph with the input and output in memory: no Read and Write nodes.
        std::unique_ptr<std::uint8_t[]> in;
        std::size_t                     size = 0;
        if (!ReadWholeFile(o.input, in, size)) {
            std::fwprintf(stderr, L"ctDeflate: cannot read %s\n", o.input);
            return 1;
        }
        auto out = std::make_unique_for_overwrite<std::uint8_t[]>(Compressor::GzipBound(size) + 16);
        QueryPerformanceCounter(&t0);
        const bool ran = WithSchedulers(o.sequential ? 1 : o.threads, [&](Ct::ITaskScheduler& io, Ct::ITaskScheduler& compute) {
            DeflateGraph graph(settings);
            graph.AttachInput(in.get(), size);
            graph.AttachOutput(out.get());
            result = graph.Run(io, compute, &stats);
        });
        QueryPerformanceCounter(&t1);
        if (!ran) {
            std::fwprintf(stderr, L"ctDeflate: cannot set up the schedulers\n");
            return 1;
        }
        if (!WriteWholeFile(o.output, out.get(), static_cast<std::size_t>(result.outSize))) {
            std::fwprintf(stderr, L"ctDeflate: cannot write %s\n", o.output);
            return 1;
        }
    } else {
        // The task graph from file to file: DeflateGraph.h.
        QueryPerformanceCounter(&t0);
        const bool ran = WithSchedulers(o.sequential ? 1 : o.threads, [&](Ct::ITaskScheduler& io, Ct::ITaskScheduler& compute) {
            DeflateGraph graph(settings);
            result.error = graph.OpenInput(o.input);
            if (result.error == 0) {
                result.error = graph.OpenOutput(o.output);
                created      = result.error == 0;
            }
            if (result.error == 0)
                result = graph.Run(io, compute, &stats);
        });
        QueryPerformanceCounter(&t1);
        if (!ran) {
            std::fwprintf(stderr, L"ctDeflate: cannot set up the schedulers\n");
            return 1;
        }
        if (result.error != 0) {
            wchar_t message[256] = L"";
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, result.error, 0, message, 256, nullptr);
            std::fwprintf(stderr, L"ctDeflate: %s -> %s: error %u %s", o.input, o.output, result.error, message);
            if (created)
                DeleteFileW(o.output);
            return 1;
        }
    }

    if (o.verbose) {
        const double seconds = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
        std::fwprintf(stderr, L"%s: %llu -> %llu bytes, %.3f s, %.1f MB/s, peak memory %.0f MB\n", o.input, result.inSize, result.outSize, seconds,
                      seconds > 0 ? static_cast<double>(result.inSize) / seconds / 1e6 : 0.0, PeakMemoryMB());
        if (stats.chunks != 0) // the parallel graph ran
            PrintStats(stats);
    }
    return 0;
}
