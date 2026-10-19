// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <optional>

#include "CommandLine.h"
#include "ConfigPrint.h"
#include "Console.h"
#include "DebugReports.h"
#include "Encoder.h"
#include "EncoderOptions.h"
#include "File.h"
#include "Input.h"
#include "Profiling.h"
#include "SampleStream.h"
#include "SessionConfig.h"
#include "TaskRuntime.h"
#include "WavFile.h"

// headers with imports last
#include "EncodeGraph.h"

// after the headers above: Application.h imports modules, and none of their textual includes may follow it
#include "Application.h"

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;

// Port of frontend/main.c and frontend/lame_main.c. The encoder runs as a task graph (EncodeGraph)
// on the compute scheduler between the WAV reader and the MP3 writer on the I/O scheduler.

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    using Shared::Console;
    using Shared::Stream;

    // --stats: what the encoding run measures
    struct Application::EncodeProfile {
        StageProfile             stages; // per stage of the encoder
        EncodeGraph::Profile     graph;  // per node of the task graph
        std::int32_t             threads = 0;
        std::array<wchar_t, 128> simd{}; // the feature line
    };

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
        // init_files
        if (std::wcscmp(options_.inPath.data(), options_.outPath.data()) == 0 || Shared::File::Same(options_.inPath.data(), options_.outPath.data())) {
            Console::Print(Stream::Error, L"Input file and Output file are the same. Abort.\n");
            return -1;
        }
        WavFile wavFile;
        if (!wavFile.Open(options_.inPath.data(), options_.encoder, options_.ignoreWavLength != 0, options_.silent)) {
            Console::Print(Stream::Error, L"Can't init infile '%ls'\n", options_.inPath.data());
            return -1;
        }
        HANDLE const outFile = InitOutfile(options_.outPath.data());
        if (outFile == INVALID_HANDLE_VALUE) {
            Console::Print(Stream::Error, L"Can't init outfile '%ls'\n", options_.outPath.data());
            return -1;
        }

        // Now that all the options are set, analyze them and set the internal options
        SessionConfig config;
        std::int32_t  ret = InitParams(options_.encoder, config);
        if (ret < 0) {
            if (ret == -1)
                CommandLine::DisplayBitrates(Stream::Error);
            Console::Print(Stream::Error, L"fatal error during initialization\n");
            CloseHandle(outFile);
            return ret;
        }

        // the rest of lame_init_params: bit stream, quantizer and psychoacoustic model (the encoder
        // with its tables is too large for the stack)
        const auto encoder = std::make_unique<Encoder>();
        // the SIMD level: the requested one (--simd, default the highest), at most what the CPU has
        const CpuFeatures        cpu  = CpuFeatures::Detect();
        const SimdLevel          simd = ChooseSimdLevel(options_.simd, cpu);
        std::array<wchar_t, 128> simdText;
        (void)cpu.FormatLine(SimdLevelName(simd), simdText.data(), static_cast<std::int32_t>(simdText.size()));
        if (options_.features != 0)
            Console::Print(Stream::Error, L"ctLame features: %ls\n", simdText.data());
        encoder->Initialize(config, simd);

        EncoderProgressBegin(config, options_, options_.inPath.data(), options_.outPath.data());

        // read_samples_pcm checks the sample width on the first read, after the configuration
        // has been printed; LAME then encodes no samples at all.
        const std::int32_t bitWidth = wavFile.PcmBitWidth();
        if (bitWidth != 8 && bitWidth != 16 && bitWidth != 24 && bitWidth != 32 && options_.silent < 10)
            Console::Print(Stream::Error, L"Only 8, 16, 24 and 32 bit input files supported \n");

        // the writer task opens the file again for overlapped I/O
        CloseHandle(outFile);
        wavFile.Close();

        EncodeGraph::Result result;
        ret = 0;
        // --stats or CTLAME_PROFILE: stage and task times on stderr after encoding
        std::optional<EncodeProfile> profile;
        if (options_.stats != 0 || ProfileRequested()) {
            profile.emplace();
            encoder->SetProfile(&profile->stages);
            (void)wcscpy_s(profile->simd.data(), profile->simd.size(), simdText.data());
        }

        const wchar_t* failure = L"";
        const bool     started = RunEncodeGraph(*encoder, config, options_, wavFile, options_.inPath.data(), options_.outPath.data(), result,
                                                profile ? &*profile : nullptr, stats_, failure);
        // get_audio: a read error ends the input like its end; LAME reports it, still flushes the
        // encoder and writes the tag, and exits with 0
        if (started && result.readError != ERROR_SUCCESS && options_.silent < 10)
            Console::Print(Stream::Error, L"Error reading input file\n");
        if (!started) {
            Console::Print(Stream::Error, L"%ls", failure);
            ret = 1;
        } else if (result.encodeError == -2) {
            Console::Print(Stream::Error, L"INTERNAL ERROR IN VBR NEW CODE, please send bug report\n");
            ret = -1;
        } else if (result.encodeError < 0) {
            if (result.encodeError == -1)
                Console::Print(Stream::Error, L"mp3 buffer is not big enough... \n");
            else
                Console::Print(Stream::Error, L"mp3 internal error:  error code=%i\n", result.encodeError);
            ret = 1;
        } else if (result.writeError != ERROR_SUCCESS) {
            Console::Print(Stream::Error, L"Error writing mp3 output \n");
            ret = 1;
        } else if (encoder->GetLameTagFrame(nullptr, 0) != 0 && options_.silent <= 0) {
            // write_xing_frame: the writer task has put the tag frame at the start of the file
            Console::Print(Stream::Error, L"Writing LAME Tag...");
            Console::Print(Stream::Error, L"done\n");
        }
        if (profile)
            PrintProfile(*profile);
        return ret;
    }

    std::int32_t Application::ParseError()
    {
        return result_ == Shared::ParseResult::Exit ? 0 : 1;
    }

    // init_outfile: like lame_fopen(outPath, "w+b").
    HANDLE Application::InitOutfile(const wchar_t* outPath)
    {
        return CreateFileW(outPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    // encoder_progress_begin (timestatus.c): what is printed before encoding starts; the progress
    // report and the VBR histogram were removed.
    void Application::EncoderProgressBegin(const SessionConfig& config, const Options& frontend, const wchar_t* inPath, const wchar_t* outPath)
    {
        if (frontend.silent < 9) {
            const auto pw = static_cast<std::size_t>(ConsoleWidth - 14);

            PrintConfig(config); // print useful information about options being used
            Console::Print(Stream::Error, L"Encoding %ls%ls to %ls\n", inPath, std::wcslen(inPath) + std::wcslen(outPath) < pw ? L"" : L"\n     ", outPath);
            Console::Print(Stream::Error, L"Encoding as %g kHz ", 1.e-3 * config.samplerate_out);
            {
                static const std::array<std::array<const wchar_t*, 4>, 2> modeNames
                    = {{{L"stereo", L"j-stereo", L"dual-ch", L"single-ch"}, {L"stereo", L"force-ms", L"dual-ch", L"single-ch"}}};
                Console::Print(Stream::Error, L"%ls MPEG-1 Layer III VBR(q=%g)\n", modeNames[config.options.forceMs][config.options.mode],
                               static_cast<double>(config.options.GetVbrQuality()));
            }
            if (frontend.silent <= -10)
                PrintInternals(config);
        }
    }

    void Application::PrintProfile(const EncodeProfile& profile) const
    {
        const Options&     frontend = options_;
        const double       perMs    = static_cast<double>(ProfileTicksPerSecond()) / 1000.0;
        const std::int64_t frames   = profile.stages.frames.load();
        std::int64_t       total    = 0;
        for (const auto& ticks : profile.stages.ticks)
            total += ticks.load();

        stats_.Print(frontend.sequential != 0 ? 1u : static_cast<std::uint32_t>(profile.threads), frontend.sequential != 0);
        Console::Print(Stream::Error, L"  %lld frames, features: %ls\n", frames, profile.simd.data());
        Console::Print(Stream::Error, L"  stage           ms   share   us/frame\n");
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(ProfileStage::Count); ++i) {
            const std::int64_t ticks = profile.stages.ticks[i].load();
            Console::Print(Stream::Error, L"  %-10ls %8.1f %6.1f %% %9.2f\n", ProfileStageName(static_cast<ProfileStage>(i)), ticks / perMs,
                           total > 0 ? 100.0 * ticks / total : 0.0, frames > 0 ? 1000.0 * ticks / perMs / frames : 0.0);
        }
        Console::Print(Stream::Error, L"  node          busy ms  idle ms\n");
        struct TaskLine {
            const wchar_t*     name;
            const TaskProfile* task;
        };
        const auto&                   graph = profile.graph;
        const std::array<TaskLine, 6> tasks = {{{L"read", &graph.read},
                                                {L"dispatch", &graph.dispatch},
                                                {L"worker", &graph.worker},
                                                {L"resolve", &graph.resolve},
                                                {frontend.sequential ? L"sequential" : L"encode", &graph.encode},
                                                {L"write", &graph.write}}};
        for (const auto& t : tasks) {
            const std::int32_t count = t.task->tasks.load();
            if (count == 0)
                continue; // not in the graph (dispatch, workers and resolve with --sequential)
            const std::int64_t life = t.task->life.load();
            const std::int64_t idle = t.task->idle.load();
            Console::Print(Stream::Error, L"  %-10ls %10.1f %8.1f%ls", t.name, (life - idle) / perMs, idle / perMs, count > 1 ? L"" : L"\n");
            if (count > 1)
                Console::Print(Stream::Error, L"  (%d tasks)\n", count);
        }
    }

    // Samples per channel the input can have at most: the bytes the reader may read.
    std::uint64_t Application::MaxInputSamples(const WavFile& wavFile, const wchar_t* inPath, std::int32_t channels, std::uint64_t byteLimit)
    {
        std::uint64_t fileSize = 0;
        if (!Shared::File::Size(inPath, fileSize))
            return 0; // unknown
        std::uint64_t dataBytes = fileSize > wavFile.DataOffset() ? fileSize - wavFile.DataOffset() : 0;
        if (dataBytes > byteLimit)
            dataBytes = byteLimit;
        const std::int32_t bytesPerSample = (wavFile.PcmBitWidth() + 7) / 8;
        return dataBytes / static_cast<std::uint64_t>(channels * (bytesPerSample > 0 ? bytesPerSample : 1)) + 1;
    }

    // lame_encoder_loop: the task graph of the encoder (EncodeGraph) on an I/O and a compute
    // scheduler; the calling thread waits for it. False if a part of the setup failed, with the
    // reason in failure.
    bool Application::RunEncodeGraph(Encoder& encoder, const SessionConfig& config, const Options& frontend, const WavFile& wavFile, const wchar_t* inPath,
                                     const wchar_t* outPath, EncodeGraph::Result& result, EncodeProfile* profile, Shared::RunStats& stats,
                                     const wchar_t*& failure)
    {
        // with a trusted header length LAME reads exactly num_samples, else up to the end
        const std::int32_t  bytesPerSample = (wavFile.PcmBitWidth() + 7) / 8;
        const std::uint64_t byteLimit
            = wavFile.CountSamplesCarefully() ? static_cast<std::uint64_t>(config.options.numSamples) * config.channels_in * bytesPerSample : ~0ull;

        EncodeGraph::Settings settings;
        settings.inPath          = inPath;
        settings.outPath         = outPath;
        settings.dataOffset      = wavFile.DataOffset();
        settings.byteLimit       = byteLimit;
        settings.maxInputSamples = MaxInputSamples(wavFile, inPath, config.channels_in, byteLimit);
        settings.format          = {config.channels_in, wavFile.PcmBitWidth(), wavFile.IsIeeeFloat(), frontend.swapChannel != 0};
        settings.threads         = frontend.threads > 0 ? static_cast<std::uint32_t>(frontend.threads) : Shared::TaskRuntime::LogicalProcessors();
        settings.sequential      = frontend.sequential != 0;
        if (profile != nullptr)
            profile->threads = static_cast<std::int32_t>(settings.threads);

        // The setup of ThinkMeta.ConcurrentTasks (Shared::TaskRuntime) with one I/O thread for Read and Write, and a
        // compute scheduler with one thread per worker. In reverse order on return: the compute scheduler stops, the
        // graph goes (and closes its files), then the I/O scheduler, async I/O and the task.
        Shared::TaskRuntime runtime(1);
        EncodeGraph         graph(encoder, config, settings, profile != nullptr ? &profile->graph : nullptr);
        if (!graph.Initialize()) {
            failure = L"ctLame: out of memory\n";
            return false;
        }
        Shared::RunningScheduler compute(SchedulerKind::Default, settings.threads);
        if (!runtime || !compute) {
            failure = L"ctLame: cannot start the schedulers\n";
            return false;
        }
        stats.WorkBegins();
        result = graph.Run(*runtime.Io(), *compute);
        stats.WorkEnds();
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
