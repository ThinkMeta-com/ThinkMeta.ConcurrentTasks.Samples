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

    ctJpeg::ByteBuffer   output;
    ctJpeg::EncodeStatus status = ctJpeg::EncodeStatus::Ok;
    ctJpeg::PhaseTimes   times;
    // One compute scheduler for this encode: threads from --threads, else one per logical
    // processor; the sequential graph has one node and gets one thread. The nodes Read and Write
    // file run on an I/O scheduler of one thread; their overlapped I/O completes on the core's
    // completion port.
    const std::uint32_t workers = frontend.sequential ? 1u
        : frontend.threads > 0                        ? static_cast<std::uint32_t>(frontend.threads)
                                                      : static_cast<std::uint32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    Ct::ConvertThreadToTask();
    const bool                     asyncIo = Ct::AsyncIo::EnableAsyncIo();
    ctJpeg::SchedulerHandle        io      = ctJpeg::SchedulerHandle::Create(Ct::SchedulerKind::Default, 1);
    ctJpeg::AsyncInput             input;
    ctJpeg::AsyncOutput            file(frontend.outPath, output);
    Ct::Synchronization::TaskGroup ioGroup;
    bool                           ioStarted = false;
    const auto                     endIo     = [&]() noexcept {
        if (ioStarted)
            ioGroup.Wait();
        if (io)
            io->Stop();
        io.Reset();
        if (asyncIo)
            Ct::AsyncIo::DisableAsyncIo();
        Ct::ConvertTaskToThread();
    };

    // Read: starts the reading; the encoder needs the header now, the pixels when a band needs them
    bool readOk = asyncIo && io && input.Open(frontend.inPath, 1u << 20);
    error[0]    = L'\0';
    if (readOk) {
        io->Start();
        ioStarted = true;
        ioGroup.AddTask(&input);
        io->AddTask(&input, Ct::TaskPriority::Normal);
        input.WaitFor(65536);
        readOk = !input.Failed() && image.ParsePnmView(input.Data(), input.Size(), error, errorSize);
        if (!readOk && !input.Failed()) { // a header longer than 64 KB (comments): parse it whole
            input.WaitFor(input.Size());
            readOk = !input.Failed() && image.ParsePnmView(input.Data(), input.Size(), error, errorSize);
        }
        if (readOk && frontend.sequential) { // the reference path reads the image whole
            input.WaitFor(input.Size());
            readOk = !input.Failed();
        }
    }
    if (!readOk) {
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

    const bool fileInGraph = !frontend.sequential;
    {
        ctJpeg::SchedulerHandle scheduler = ctJpeg::SchedulerHandle::Create(Ct::SchedulerKind::Default, workers);
        if (!scheduler) {
            endIo();
            std::fwprintf(stderr, L"ctJpeg: cannot create the task scheduler\n");
            return 1;
        }
        scheduler->Start();
        tStarted = Now();
        ctJpeg::EncodeGraph::Settings settings;
        settings.options            = options;
        settings.simd               = simd;
        settings.workers            = workers;
        settings.sequential         = frontend.sequential;
        settings.split.bandRows     = frontend.bandRows;
        settings.split.scanBands    = frontend.scanBands;
        settings.split.stripWidth   = frontend.stripWidth;
        settings.split.releaseInput = &image;
        settings.input              = &input;
        settings.outputFile         = fileInGraph ? &file : nullptr;
        settings.io                 = io.Get();
        ctJpeg::EncodeGraph graph(image, settings, output, &times);
        status   = graph.Run(*scheduler.Get());
        tEncoded = Now();
        scheduler->Stop();
    }
    // Write file after the sequential graph (the stage graph writes it while it frees its buffers)
    if (status == ctJpeg::EncodeStatus::Ok && !fileInGraph) {
        ioGroup.AddTask(&file);
        io->AddTask(&file, Ct::TaskPriority::Normal);
    }
    endIo();
    const double tStopped = Now();
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
    if (frontend.times && !frontend.sequential) {
        std::fwprintf(stderr, L"times: setup %.3f, pixels %.3f, tokenize %.3f, huffman %.3f, write %.3f, assemble %.3f, release %.3f s (%u workers)\n",
                      times.setup, times.pixels, times.tokenize, times.huffman, times.write, times.assemble, times.release, workers);
        // the serial rest around the encode: the start before wmain, until the header has
        // arrived, the scheduler, the rest of the encode call, the end of the I/O (the file is
        // written during Release)
        const double phases = times.setup + times.pixels + times.tokenize + times.huffman + times.write + times.assemble + times.release;
        std::fwprintf(stderr, L"serial: start %.3f, header %.3f, scheduler start %.3f, rest of the encode %.3f, end of I/O %.3f s\n", startup, tRead - t0,
                      tStarted - tRead, tEncoded - tStarted - phases, tStopped - tEncoded);
    }
    return 0;
}
