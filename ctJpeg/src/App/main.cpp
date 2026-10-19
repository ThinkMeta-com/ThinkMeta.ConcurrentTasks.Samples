// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

#include "ByteBuffer.h"
#include "Coefficients.h"
#include "CommandLine.h"
#include "Encoder.h"
#include "EncoderOptions.h"
#include "EncoderSetup.h"
#include "Image.h"
#include "SimdFeatures.h"

// headers with imports last
#include "AsyncInput.h"
#include "AsyncOutput.h"
#include "EncodeGraph.h"
#include "SchedulerHandle.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

// ctJpeg: jpegli (google/jpegli @ 031a007) as a bit-identical C++ port on ThinkMeta.ConcurrentTasks,
// one image on all cores. The task graph (EncodeGraph.h): the nodes Read and
// Write file on an I/O scheduler, the encoder on a compute scheduler; --sequential is the reference
// path in one task.

namespace ctJpeg = ThinkMeta::ConcurrentTasks::Samples::ctJpeg;
namespace Ct     = ThinkMeta::ConcurrentTasks;

namespace {

    /// <summary>The input arrives in steps of 1 MB: a Pixel tile wakes when the step holding its last row is in.</summary>
    constexpr std::size_t ReadStep = std::size_t{1} << 20;

    double Now() noexcept
    {
        LARGE_INTEGER counter, frequency;
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
    }

    // Seconds from the creation of this process to now (--times: the start before wmain).
    double SinceProcessStart() noexcept
    {
        FILETIME creation, exitTime, kernel, user, now;
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user))
            return 0.0;
        GetSystemTimePreciseAsFileTime(&now);
        const auto ticks = [](const FILETIME& f) noexcept { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        return static_cast<double>(ticks(now) - ticks(creation)) * 1e-7;
    }

    bool WriteFileAll(const wchar_t* path, const ctJpeg::ByteBuffer& data)
    {
        const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        std::size_t done = 0;
        bool        ok   = true;
        while (ok && done < data.Size()) {
            DWORD      written = 0; // DWORD out parameter of WriteFile
            const auto chunk   = static_cast<DWORD>(data.Size() - done < 0x40000000u ? data.Size() - done : 0x40000000u);
            ok                 = WriteFile(file, data.Data() + done, chunk, &written, nullptr) && written == chunk;
            done += written;
        }
        return CloseHandle(file) && ok;
    }

    // --dump-coefficients: the quantized coefficients for comparing with another encoder, as
    // "CTJC", the component count (int32), per component its width and height in blocks (int32
    // each) and its quantization table (64 x uint16), then the blocks of each component in row
    // order, 64 int16 each in natural (not zigzag) order; all little-endian.
    int DumpCoefficients(const ctJpeg::Image& image, const ctJpeg::EncoderOptions& options, ctJpeg::SimdLevel simd, const wchar_t* path)
    {
        ctJpeg::EncoderSetup       setup;
        ctJpeg::Coefficients       coefficients;
        const ctJpeg::EncodeStatus status = ctJpeg::ComputeAllCoefficients(image, options, simd, setup, coefficients);
        if (status != ctJpeg::EncodeStatus::Ok) {
            std::fwprintf(stderr, L"ctJpeg: %ls\n", ctJpeg::EncodeStatusText(status));
            return 1;
        }
        ctJpeg::ByteBuffer out;
        auto               put32 = [&out](std::int32_t v) { out.Append(reinterpret_cast<const std::uint8_t*>(&v), sizeof(v)); };
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
                    std::int16_t        natural[ctJpeg::DctSize2];
                    for (std::int32_t k = 0; k < ctJpeg::DctSize2; ++k)
                        natural[ctJpeg::NaturalOrder[static_cast<std::size_t>(k)]] = zigzag[k];
                    out.Append(reinterpret_cast<const std::uint8_t*>(natural), sizeof(natural));
                }
            }
        }
        if (!WriteFileAll(path, out)) {
            std::fwprintf(stderr, L"ctJpeg: cannot write '%ls'\n", path);
            return 1;
        }
        return 0;
    }

    // Read: starts the node on the I/O scheduler and parses the header; the pixels come later, when a band needs them
    // (the sequential graph waits for the whole file). False on an error, with the message in error (empty: a read error,
    // its code in input.Error()).
    bool StartRead(ctJpeg::AsyncInput& input, Ct::ITaskScheduler& io, Ct::Synchronization::TaskGroup_s& ioGroup, const ctJpeg::FrontendOptions& frontend,
                   ctJpeg::Image& image, wchar_t* error, std::int32_t errorSize)
    {
        error[0] = L'\0';
        if (!input.Open(frontend.inPath, ReadStep))
            return false;
        ioGroup.AddTask(&input);
        io.AddTask(&input, Ct::TaskPriority::Normal);
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

    void PrintTimes(const ctJpeg::PhaseTimes& times, std::uint32_t workers, double startup, double header, double schedulerStart, double encode,
                    double endOfIo)
    {
        std::fwprintf(stderr, L"times: setup %.3f, pixels %.3f, tokenize %.3f, huffman %.3f, write %.3f, assemble %.3f, release %.3f s (%u workers)\n",
                      times.setup, times.pixels, times.tokenize, times.huffman, times.write, times.assemble, times.release, workers);
        // the serial rest around the encode: the start before wmain, until the header has arrived, the scheduler, the
        // rest of the encode call, the end of the I/O (the file is written during Release)
        const double phases = times.setup + times.pixels + times.tokenize + times.huffman + times.write + times.assemble + times.release;
        std::fwprintf(stderr, L"serial: start %.3f, header %.3f, scheduler start %.3f, rest of the encode %.3f, end of I/O %.3f s\n", startup, header,
                      schedulerStart, encode - phases, endOfIo);
    }

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const double            startup = SinceProcessStart();
    const double            t0      = Now();
    ctJpeg::EncoderOptions  options;
    ctJpeg::FrontendOptions frontend;
    const std::int32_t      parsed = ctJpeg::ParseCommandLine(argc, argv, options, frontend);
    if (parsed != 0)
        return parsed == 2 ? 0 : 1;

    // The feature sets give different output, so a requested one must be available; without
    // --simd the best one of this CPU is used.
    const ctJpeg::CpuFeatures features = ctJpeg::DetectCpuFeatures();
    const ctJpeg::SimdLevel   simd     = frontend.simdGiven ? frontend.simd : ctJpeg::HighestSimdLevel(features);
    if (ctJpeg::ChooseSimdLevel(simd, features) != simd) {
        std::fwprintf(stderr, L"ctJpeg: feature set %ls is not supported by this CPU\n", ctJpeg::SimdLevelName(simd));
        return 1;
    }

    wchar_t            error[512];
    const std::int32_t errorSize = static_cast<std::int32_t>(sizeof(error) / sizeof(error[0]));
    ctJpeg::Image      image;
    if (frontend.dumpPath != nullptr) { // check tool: the whole file mapped
        if (!image.ReadPnm(frontend.inPath, error, errorSize)) {
            std::fwprintf(stderr, L"ctJpeg: %ls\n", error);
            return 1;
        }
        return DumpCoefficients(image, options, simd, frontend.dumpPath);
    }

    // The setup of ThinkMeta.ConcurrentTasks, the same in every sample:
    // 1. ConvertThreadToTask: this thread becomes a task, so it can wait (for the header, for the graph's task groups).
    // 2. EnableAsyncIo: the core's completion port, for the overlapped I/O of the nodes Read and Write file.
    // 3. An I/O scheduler of one thread for Read and Write file, and a compute scheduler for the graph: threads from
    //    --threads, else one per logical processor (the sequential graph has one node and gets one thread).
    // 4. Start, run the graph, stop once every task has exited; then undo 2 and 1.
    const std::uint32_t workers = frontend.sequential ? 1u
        : frontend.threads > 0                        ? static_cast<std::uint32_t>(frontend.threads)
                                                      : static_cast<std::uint32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    Ct::ConvertThreadToTask();
    const bool                       asyncIo = Ct::AsyncIo::EnableAsyncIo();
    ctJpeg::SchedulerHandle          io      = ctJpeg::SchedulerHandle::Create(Ct::SchedulerKind::Default, 1);
    ctJpeg::ByteBuffer               output;
    ctJpeg::AsyncInput               input;
    ctJpeg::AsyncOutput              file(frontend.outPath, output);
    Ct::Synchronization::TaskGroup_s ioGroup; // Read and Write file; one waiter: this task
    const auto                       endIo = [&]() noexcept {
        if (io) {
            ioGroup.Wait(); // before ~AsyncInput: Read sets its events until it ends
            io->Stop();
        }
        io.Reset();
        if (asyncIo)
            Ct::AsyncIo::DisableAsyncIo();
        Ct::ConvertTaskToThread();
    };
    if (!asyncIo || !io) {
        endIo();
        std::fwprintf(stderr, L"ctJpeg: cannot set up the asynchronous I/O\n");
        return 1;
    }
    io->Start();

    if (!StartRead(input, *io.Get(), ioGroup, frontend, image, error, errorSize)) {
        endIo();
        if (error[0] != L'\0')
            std::fwprintf(stderr, L"ctJpeg: %ls\n", error);
        else
            std::fwprintf(stderr, L"ctJpeg: cannot read '%ls' (error %u)\n", frontend.inPath, input.Error());
        return 1;
    }
    const std::int32_t width    = image.Width();
    const std::int32_t height   = image.Height();
    const double       tRead    = Now();
    double             tStarted = tRead, tEncoded = tRead;

    // The graph (EncodeGraph.h) on its own compute scheduler; Write file runs inside the stage graph, after the
    // sequential one.
    ctJpeg::EncodeStatus status = ctJpeg::EncodeStatus::Ok;
    ctJpeg::PhaseTimes   times;
    const bool           fileInGraph = !frontend.sequential;
    {
        ctJpeg::SchedulerHandle compute = ctJpeg::SchedulerHandle::Create(Ct::SchedulerKind::Default, workers);
        if (!compute) {
            endIo();
            std::fwprintf(stderr, L"ctJpeg: cannot create the task scheduler\n");
            return 1;
        }
        compute->Start();
        tStarted = Now();
        ctJpeg::EncodeGraph::Settings settings;
        settings.options          = options;
        settings.simd             = simd;
        settings.workers          = workers;
        settings.sequential       = frontend.sequential;
        settings.split.bandRows   = frontend.bandRows;
        settings.split.scanBands  = frontend.scanBands;
        settings.split.stripWidth = frontend.stripWidth;
        settings.input            = &input;
        settings.outputFile       = fileInGraph ? &file : nullptr;
        settings.io               = io.Get();
        ctJpeg::EncodeGraph graph(image, settings, output, &times);
        status   = graph.Run(*compute.Get());
        tEncoded = Now();
        compute->Stop();
    }
    if (status == ctJpeg::EncodeStatus::Ok && !fileInGraph) {
        ioGroup.AddTask(&file);
        io->AddTask(&file, Ct::TaskPriority::Normal);
    }
    endIo();
    const double tStopped = Now();

    if (status == ctJpeg::EncodeStatus::ReadFailed) {
        std::fwprintf(stderr, L"ctJpeg: cannot read '%ls' (error %u)\n", frontend.inPath, input.Error());
        return 1;
    }
    if (status != ctJpeg::EncodeStatus::Ok) {
        std::fwprintf(stderr, L"ctJpeg: %ls\n", ctJpeg::EncodeStatusText(status));
        return 1;
    }
    if (file.Failed()) {
        std::fwprintf(stderr, L"ctJpeg: cannot write '%ls'\n", frontend.outPath);
        return 1;
    }
    if (!frontend.quiet)
        std::fwprintf(stderr, L"Encoded %dx%d to %zu bytes (%ls, %ls)\n", width, height, output.Size(), ctJpeg::SimdLevelName(simd),
                      frontend.sequential ? L"sequential" : L"parallel");
    if (frontend.times && !frontend.sequential)
        PrintTimes(times, workers, startup, tRead - t0, tStarted - tRead, tEncoded - tStarted, tStopped - tEncoded);
    return 0;
}
