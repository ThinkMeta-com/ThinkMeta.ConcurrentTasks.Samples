// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>
#include <cwchar>

#include "ByteBuffer.h"
#include "Coefficients.h"
#include "CommandLine.h"
#include "Console.h"
#include "DebugReports.h"
#include "Diagnostics.h"
#include "Encoder.h"
#include "EncoderOptions.h"
#include "EncoderSetup.h"
#include "File.h"
#include "Image.h"
#include "SimdFeatures.h"

// headers with imports last
#include "AsyncInput.h"
#include "AsyncOutput.h"
#include "EncodeGraph.h"
#include "TaskRuntime.h"

// after the headers above: Application.h imports a module, and none of their textual includes may follow it
#include "Application.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

// ctJpeg: jpegli (google/jpegli @ 031a007) as a bit-identical C++ port on ThinkMeta.ConcurrentTasks,
// one image on all cores. The task graph (EncodeGraph.h): the nodes Read and
// Write file on an I/O scheduler, the encoder on a compute scheduler; --sequential is the reference
// path in one task.

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    using Shared::Console;
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
        // The feature sets give different output, so a requested one must be available; without
        // --simd the best one of this CPU is used.
        const CpuFeatures features = CpuFeatures::Detect();
        const SimdLevel   simd     = options_.simdGiven ? options_.simd : HighestSimdLevel(features);
        if (ChooseSimdLevel(simd, features) != simd) {
            Console::Print(Stream::Error, L"ctJpeg: feature set %ls is not supported by this CPU\n", SimdLevelName(simd));
            return 1;
        }
        if (options_.features) {
            wchar_t line[128];
            (void)features.FormatLine(SimdLevelName(simd), line, static_cast<std::int32_t>(sizeof(line) / sizeof(line[0])));
            Console::Print(Stream::Error, L"ctJpeg features: %ls\n", line);
        }

        wchar_t            error[512];
        const std::int32_t errorSize = static_cast<std::int32_t>(sizeof(error) / sizeof(error[0]));
        Image              image;
        if (options_.dumpPath != nullptr) { // check tool: the whole file mapped
            if (!image.ReadPnm(options_.inPath, error, errorSize)) {
                Console::Print(Stream::Error, L"ctJpeg: %ls\n", error);
                return 1;
            }
            return DumpCoefficients(image, options_.encoder, simd, options_.dumpPath);
        }

        // The setup of ThinkMeta.ConcurrentTasks (Shared::TaskRuntime) with one I/O thread for Read and Write file; this
        // task waits for the header and for the task groups of the graph. The compute scheduler follows once the header
        // is in: threads from --threads, else one per logical processor (the sequential graph has one node and gets one).
        const std::uint32_t          workers = options_.sequential ? 1u
            : options_.threads > 0                                 ? static_cast<std::uint32_t>(options_.threads)
                                                                   : Shared::TaskRuntime::LogicalProcessors();
        Shared::TaskRuntime          runtime(1);
        ByteBuffer                   output;
        AsyncInput                   input;
        AsyncOutput                  file(options_.outPath, output);
        Synchronization::TaskGroup_s ioGroup; // Read and Write file; one waiter: this task
        const auto                   endIo = [&]() noexcept {
            if (runtime.Io())
                ioGroup.Wait(); // before ~AsyncInput: Read sets its events until it ends
            runtime.End();
        };
        if (!runtime) {
            endIo();
            Console::Print(Stream::Error, L"ctJpeg: cannot set up the asynchronous I/O\n");
            return 1;
        }

        if (!StartRead(input, *runtime.Io(), ioGroup, options_, image, error, errorSize)) {
            endIo();
            if (error[0] != L'\0')
                Console::Print(Stream::Error, L"ctJpeg: %ls\n", error);
            else
                Console::Print(Stream::Error, L"ctJpeg: cannot read '%ls' (error %u)\n", options_.inPath, input.Error());
            return 1;
        }
        const std::int32_t width    = image.Width();
        const std::int32_t height   = image.Height();
        const double       tRead    = Shared::Diagnostics::Now();
        double             tStarted = tRead, tEncoded = tRead;

        // The graph (EncodeGraph.h) on its own compute scheduler; Write file runs inside the stage graph, after the
        // sequential one.
        EncodeStatus status = EncodeStatus::Ok;
        PhaseTimes   times;
        const bool   fileInGraph = !options_.sequential;
        {
            Shared::RunningScheduler compute(SchedulerKind::Default, workers);
            if (!compute) {
                endIo();
                Console::Print(Stream::Error, L"ctJpeg: cannot create the task scheduler\n");
                return 1;
            }
            tStarted = Shared::Diagnostics::Now();
            stats_.WorkBegins();
            EncodeGraph::Settings settings;
            settings.options          = options_.encoder;
            settings.simd             = simd;
            settings.workers          = workers;
            settings.sequential       = options_.sequential;
            settings.split.bandRows   = options_.bandRows;
            settings.split.scanBands  = options_.scanBands;
            settings.split.stripWidth = options_.stripWidth;
            settings.input            = &input;
            settings.outputFile       = fileInGraph ? &file : nullptr;
            EncodeGraph graph(image, settings, output, &times);
            status   = graph.Run(*runtime.Io(), *compute);
            tEncoded = Shared::Diagnostics::Now();
            stats_.WorkEnds();
            compute.Stop();
        }
        if (status == EncodeStatus::Ok && !fileInGraph) {
            ioGroup.AddTask(&file);
            runtime.Io()->AddTask(&file, TaskPriority::Normal);
        }
        endIo();
        const double tStopped = Shared::Diagnostics::Now();

        if (status == EncodeStatus::ReadFailed) {
            Console::Print(Stream::Error, L"ctJpeg: cannot read '%ls' (error %u)\n", options_.inPath, input.Error());
            return 1;
        }
        if (status != EncodeStatus::Ok) {
            Console::Print(Stream::Error, L"ctJpeg: %ls\n", EncodeStatusText(status));
            return 1;
        }
        if (file.Failed()) {
            Console::Print(Stream::Error, L"ctJpeg: cannot write '%ls'\n", options_.outPath);
            return 1;
        }
        if (!options_.quiet)
            Console::Print(Stream::Error, L"Encoded %dx%d to %zu bytes (%ls, %ls)\n", width, height, output.Size(), SimdLevelName(simd),
                           options_.sequential ? L"sequential" : L"parallel");
        if (options_.stats) {
            stats_.Print(workers, options_.sequential);
            if (!options_.sequential) // the phases of the stage graph
                PrintTimes(times, stats_.Setup() - (tStarted - tRead), tStarted - tRead, tEncoded - tStarted, tStopped - tEncoded);
        }
        return 0;
    }

    std::int32_t Application::ParseError()
    {
        return result_ == Shared::ParseResult::Exit ? 0 : 1;
    }

    // --dump-coefficients: the quantized coefficients for comparing with another encoder, as
    // "CTJC", the component count (int32), per component its width and height in blocks (int32
    // each) and its quantization table (64 x uint16), then the blocks of each component in row
    // order, 64 int16 each in natural (not zigzag) order; all little-endian.
    int Application::DumpCoefficients(const Image& image, const EncoderOptions& options, SimdLevel simd, const wchar_t* path)
    {
        EncoderSetup       setup;
        Coefficients       coefficients;
        const EncodeStatus status = ComputeAllCoefficients(image, options, simd, setup, coefficients);
        if (status != EncodeStatus::Ok) {
            Console::Print(Stream::Error, L"ctJpeg: %ls\n", EncodeStatusText(status));
            return 1;
        }
        ByteBuffer out;
        auto       put32 = [&out](std::int32_t v) { out.Append(reinterpret_cast<const std::uint8_t*>(&v), sizeof(v)); };
        out.Append(reinterpret_cast<const std::uint8_t*>("CTJC"), 4);
        put32(coefficients.Components());
        for (std::int32_t c = 0; c < coefficients.Components(); ++c) {
            put32(coefficients.WidthInBlocks(c));
            put32(coefficients.HeightInBlocks(c));
            const auto& table = setup.quant[static_cast<std::size_t>(setup.frame.component[static_cast<std::size_t>(c)].quantTable)];
            out.Append(reinterpret_cast<const std::uint8_t*>(table.values.data()), sizeof(table.values));
        }
        for (std::int32_t c = 0; c < coefficients.Components(); ++c) {
            for (std::int32_t by = 0; by < coefficients.HeightInBlocks(c); ++by) {
                for (std::int32_t bx = 0; bx < coefficients.WidthInBlocks(c); ++bx) {
                    const std::int16_t* zigzag = coefficients.Block(c, bx, by);
                    std::int16_t        natural[DctSize2];
                    for (std::int32_t k = 0; k < DctSize2; ++k)
                        natural[NaturalOrder[static_cast<std::size_t>(k)]] = zigzag[k];
                    out.Append(reinterpret_cast<const std::uint8_t*>(natural), sizeof(natural));
                }
            }
        }
        if (!Shared::File::Write(path, out.Data(), out.Size())) {
            Console::Print(Stream::Error, L"ctJpeg: cannot write '%ls'\n", path);
            return 1;
        }
        return 0;
    }

    // Read: starts the node on the I/O scheduler and parses the header; the pixels come later, when a band needs them
    // (the sequential graph waits for the whole file). False on an error, with the message in error (empty: a read error,
    // its code in input.Error()).
    bool Application::StartRead(AsyncInput& input, ITaskScheduler& io, Synchronization::TaskGroup_s& ioGroup, const Options& frontend, Image& image,
                                wchar_t* error, std::int32_t errorSize)
    {
        error[0] = L'\0';
        if (!input.Open(frontend.inPath, ReadStep))
            return false;
        ioGroup.AddTask(&input);
        io.AddTask(&input, TaskPriority::Normal);
        // the header from the first step that arrives; a longer one (comments) from the whole file
        input.WaitFor(1);
        if (input.Failed())
            return false;
        if (!image.ParsePnmView(input.Data(), input.ArrivedAfter(1), input.Size(), error, errorSize)) {
            input.WaitFor(input.Size());
            error[0] = L'\0';
            if (input.Failed() || !image.ParsePnmView(input.Data(), input.Size(), input.Size(), error, errorSize))
                return false;
        }
        error[0] = L'\0';
        if (frontend.sequential) { // the reference path reads the image whole
            input.WaitFor(input.Size());
            return !input.Failed();
        }
        return true;
    }

    void Application::PrintTimes(const PhaseTimes& times, double header, double schedulerStart, double encode, double endOfIo)
    {
        Console::Print(Stream::Error, L"  phases: setup %.3f, pixels %.3f, tokenize %.3f, huffman %.3f, write %.3f, assemble %.3f, release %.3f s\n",
                       times.setup, times.pixels, times.tokenize, times.huffman, times.write, times.assemble, times.release);
        // the serial rest around the encode (the start before wmain is in the stats line): until the header has arrived,
        // the scheduler, the rest of the encode call, the end of the I/O (the file is written during Release)
        const double phases = times.setup + times.pixels + times.tokenize + times.huffman + times.write + times.assemble + times.release;
        Console::Print(Stream::Error, L"  serial: header %.3f, scheduler start %.3f, rest of the encode %.3f, end of I/O %.3f s\n", header, schedulerStart,
                       encode - phases, endOfIo);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
