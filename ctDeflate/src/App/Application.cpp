//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>
#include <cwchar>
#include <memory>

#include "Application.h"
#include "Compressor.h"
#include "Console.h"
#include "DebugReports.h"
#include "Diagnostics.h"
#include "File.h"
#include "Formatter.h"

// headers with imports last
#include "DeflateGraph.h"
#include "TaskRuntime.h"

// ctDeflate: the gzip compression of libdeflate 1.26 as a bit-identical C++ port on ThinkMeta.ConcurrentTasks, one
// file on all cores. The task graph is DeflateGraph (DeflateGraph.h); this file parses the command line, sets up the
// schedulers and runs the graph once.

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    using Shared::Console;
    using Shared::Formatter;
    using Shared::Stream;

    Application::Application()
    {
        Shared::DebugReports::ToStderr();
    }

    bool Application::ParseArgs(std::int32_t argc, wchar_t** argv)
    {
        stats_.Start();
        result_ = CommandLine::Parse(argc, argv, options_);
        return result_ == Shared::ParseResult::Run;
    }

    std::int32_t Application::Execute()
    {
        const wchar_t* output = options_.output;
        wchar_t        defaultOutput[MAX_PATH + 4];
        if (output == nullptr) {
            if (Formatter::Format(defaultOutput, MAX_PATH + 4, L"%s.gz", options_.input) >= MAX_PATH + 4) {
                Console::Print(Stream::Error, L"ctDeflate: path too long: %s\n", options_.input);
                return 1;
            }
            output = defaultOutput;
        }
        if (Shared::File::Same(options_.input, output)) {
            Console::Print(Stream::Error, L"ctDeflate: %s: input and output are the same file\n", options_.input);
            return 1;
        }

        // --in-memory: the same graph with the input and output in memory, without the nodes Read and Write
        std::unique_ptr<std::uint8_t[]> in, out;
        std::uint64_t                   size = 0;
        if (options_.inMemory) {
            bool read = Shared::File::Size(options_.input, size);
            if (read) {
                in   = std::make_unique_for_overwrite<std::uint8_t[]>(static_cast<std::size_t>(size) + 1);
                read = Shared::File::Read(options_.input, in.get(), static_cast<std::size_t>(size));
            }
            if (!read) {
                Console::Print(Stream::Error, L"ctDeflate: cannot read %s\n", options_.input);
                return 1;
            }
            out = std::make_unique_for_overwrite<std::uint8_t[]>(Compressor::GzipBound(static_cast<std::size_t>(size)) + 16);
        }

        const std::uint32_t          threads = options_.threads > 0 ? options_.threads : Shared::TaskRuntime::LogicalProcessors();
        const DeflateGraph::Settings settings{.level = options_.level, .workers = threads, .chunks = options_.chunks, .sequential = options_.sequential};
        ParallelStats                stats;
        DeflateGraph::Result         result;
        bool                         created = false; // this run created the output: delete it on an error, and nothing else
        {
            // two I/O threads: a write that extends the file runs synchronously in NTFS and must not hold up the reads;
            // the compute scheduler for the workers (one for the sequential graph)
            Shared::TaskRuntime      runtime(2);
            Shared::RunningScheduler compute(SchedulerKind::Default, options_.sequential ? 1 : threads);
            if (!runtime || !compute) {
                Console::Print(Stream::Error, L"ctDeflate: cannot set up the schedulers\n");
                return 1;
            }
            // the task graph: DeflateGraph.h
            DeflateGraph graph(settings);
            if (options_.inMemory) {
                graph.AttachInput(in.get(), static_cast<std::size_t>(size));
                graph.AttachOutput(out.get());
            } else {
                result.error = graph.OpenInput(options_.input);
                if (result.error == 0) {
                    result.error = graph.OpenOutput(output);
                    created      = result.error == 0;
                }
            }
            if (result.error == 0) {
                stats_.WorkBegins();
                result = graph.Run(*runtime.Io(), *compute, &stats);
                stats_.WorkEnds();
            }
        }

        if (options_.inMemory) {
            if (!Shared::File::Write(output, out.get(), static_cast<std::size_t>(result.outSize))) {
                Console::Print(Stream::Error, L"ctDeflate: cannot write %s\n", output);
                return 1;
            }
        } else if (result.error != 0) {
            wchar_t message[256];
            Shared::File::ErrorText(result.error, message, 256);
            Console::Print(Stream::Error, L"ctDeflate: %s -> %s: error %u %s\n", options_.input, output, result.error, message);
            if (created)
                DeleteFileW(output);
            return 1;
        }

        if (options_.stats) {
            const double seconds = stats_.Work();
            stats_.Print(options_.sequential ? 1 : threads, options_.sequential);
            Console::Print(Stream::Error, L"  %s: %llu -> %llu bytes, %.1f MB/s\n", options_.input, result.inSize, result.outSize,
                           seconds > 0 ? static_cast<double>(result.inSize) / seconds / 1e6 : 0.0);
            if (stats.chunks != 0) // the parallel graph ran
                PrintStats(stats);
        }
        return 0;
    }

    std::int32_t Application::ParseError()
    {
        return result_ == Shared::ParseResult::Exit ? 0 : 1;
    }

    void Application::PrintStats(const ParallelStats& stats)
    {
        Console::Print(Stream::Error,
                       L"  chunks %zu, blocks %zu, syncs %zu, repairs %zu (%zu bytes), max sync distance %zu; parse+resolve %.3f s (resolver waited %.3f s), "
                       L"encode %.3f s, stitch %.3f s\n",
                       stats.chunks, stats.blocks, stats.resolve.syncs, stats.resolve.repairs, stats.resolve.repairedBytes, stats.resolve.maxSyncDistance,
                       stats.parseResolve, stats.resolveWait, stats.encode, stats.stitch);
        Console::Print(Stream::Error, L"  repairs by cause: minLen %zu, parse ended %zu; iterations by minLen:", stats.resolve.minLenMismatches,
                       stats.resolve.parseExhausted);
        for (std::int32_t m = 0; m <= static_cast<std::int32_t>(MaxMinLen); ++m)
            if (stats.resolve.minLenIterations[m] != 0)
                Console::Print(Stream::Error, L" %d:%zu/%zuKB", m, stats.resolve.minLenIterations[m], stats.resolve.minLenBytes[m] >> 10);
        Console::Print(Stream::Error, L"\n");
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
