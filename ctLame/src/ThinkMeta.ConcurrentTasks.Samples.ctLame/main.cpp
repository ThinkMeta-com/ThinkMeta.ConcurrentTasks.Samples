// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <crtdbg.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <optional>

#include "CommandLine.h"
#include "ConfigPrint.h"
#include "Console.h"
#include "Encoder.h"
#include "EncoderOptions.h"
#include "Input.h"
#include "Profiling.h"
#include "SampleStream.h"
#include "SchedulerHandle.h"
#include "SessionConfig.h"
#include "WavFile.h"

// headers with imports last
#include "EncodeGraph.h"

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;

// Port of frontend/main.c and frontend/lame_main.c. The encoder runs as a task graph (EncodeGraph)
// on the compute scheduler between the WAV reader and the MP3 writer on the I/O scheduler.

namespace ctLame = ThinkMeta::ConcurrentTasks::Samples::ctLame;
namespace Ct     = ThinkMeta::ConcurrentTasks;

namespace {

    // init_outfile: like lame_fopen(outPath, "w+b").
    HANDLE InitOutfile(const wchar_t* outPath)
    {
        return CreateFileW(outPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    // encoder_progress_begin (timestatus.c): what is printed before encoding starts; the progress
    // report and the VBR histogram were removed.
    void EncoderProgressBegin(const ctLame::SessionConfig& config, const ctLame::FrontendOptions& frontend, const wchar_t* inPath, const wchar_t* outPath)
    {
        if (frontend.silent < 9) {
            const auto pw = static_cast<std::size_t>(ctLame::ConsoleGetWidth() - 14);

            ctLame::PrintConfig(config); // print useful information about options being used
            ctLame::ConsolePrintf(L"Encoding %ls%ls to %ls\n", inPath, std::wcslen(inPath) + std::wcslen(outPath) < pw ? L"" : L"\n     ", outPath);
            ctLame::ConsolePrintf(L"Encoding as %g kHz ", 1.e-3 * config.samplerate_out);
            {
                static const std::array<std::array<const wchar_t*, 4>, 2> modeNames
                    = {{{L"stereo", L"j-stereo", L"dual-ch", L"single-ch"}, {L"stereo", L"force-ms", L"dual-ch", L"single-ch"}}};
                ctLame::ConsolePrintf(L"%ls MPEG-1 Layer III VBR(q=%g)\n", modeNames[config.options.forceMs][config.options.mode],
                                      static_cast<double>(config.options.GetVbrQuality()));
            }
            if (frontend.silent <= -10)
                ctLame::PrintInternals(config);
        }
    }

    // CTLAME_PROFILE: what the encoding run measures
    struct EncodeProfile {
        ctLame::StageProfile         stages; // per stage of the encoder
        ctLame::EncodeGraph::Profile graph;  // per node of the task graph
        std::int32_t                 threads = 0;
        std::array<wchar_t, 128>     simd{}; // the feature line
    };

    void PrintProfile(const EncodeProfile& profile, const ctLame::FrontendOptions& frontend)
    {
        const double       perMs  = static_cast<double>(ctLame::ProfileTicksPerSecond()) / 1000.0;
        const std::int64_t frames = profile.stages.frames.load();
        std::int64_t       total  = 0;
        for (const auto& ticks : profile.stages.ticks)
            total += ticks.load();

        ctLame::ErrorPrintf(L"ctLame profile: %lld frames in %.1f ms, %d compute threads%ls\n", frames, (profile.graph.end - profile.graph.start) / perMs,
                            profile.threads, frontend.sequential ? L", sequential" : L"");
        ctLame::ErrorPrintf(L"  features: %ls\n", profile.simd.data());
        ctLame::ErrorPrintf(L"  stage           ms   share   us/frame\n");
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(ctLame::ProfileStage::Count); ++i) {
            const std::int64_t ticks = profile.stages.ticks[i].load();
            ctLame::ErrorPrintf(L"  %-10ls %8.1f %6.1f %% %9.2f\n", ctLame::ProfileStageName(static_cast<ctLame::ProfileStage>(i)), ticks / perMs,
                                total > 0 ? 100.0 * ticks / total : 0.0, frames > 0 ? 1000.0 * ticks / perMs / frames : 0.0);
        }
        ctLame::ErrorPrintf(L"  node          busy ms  idle ms\n");
        struct TaskLine {
            const wchar_t*             name;
            const ctLame::TaskProfile* task;
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
            ctLame::ErrorPrintf(L"  %-10ls %10.1f %8.1f%ls", t.name, (life - idle) / perMs, idle / perMs, count > 1 ? L"" : L"\n");
            if (count > 1)
                ctLame::ErrorPrintf(L"  (%d tasks)\n", count);
        }
    }

    // Samples per channel the input can have at most: the bytes the reader may read.
    std::uint64_t MaxInputSamples(const ctLame::WavFile& wavFile, const wchar_t* inPath, std::int32_t channels, std::uint64_t byteLimit)
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(inPath, GetFileExInfoStandard, &attributes))
            return 0; // unknown
        const std::uint64_t fileSize  = static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32 | attributes.nFileSizeLow;
        std::uint64_t       dataBytes = fileSize > wavFile.DataOffset() ? fileSize - wavFile.DataOffset() : 0;
        if (dataBytes > byteLimit)
            dataBytes = byteLimit;
        const std::int32_t bytesPerSample = (wavFile.PcmBitWidth() + 7) / 8;
        return dataBytes / static_cast<std::uint64_t>(channels * (bytesPerSample > 0 ? bytesPerSample : 1)) + 1;
    }

    // lame_encoder_loop: the task graph of the encoder (EncodeGraph) on an I/O and a compute
    // scheduler; the calling thread waits for it.
    bool Encode(ctLame::Encoder& encoder, const ctLame::SessionConfig& config, const ctLame::FrontendOptions& frontend, const ctLame::WavFile& wavFile,
                const wchar_t* inPath, const wchar_t* outPath, ctLame::EncodeGraph::Result& result, EncodeProfile* profile)
    {
        // with a trusted header length LAME reads exactly num_samples, else up to the end
        const std::int32_t  bytesPerSample = (wavFile.PcmBitWidth() + 7) / 8;
        const std::uint64_t byteLimit
            = wavFile.CountSamplesCarefully() ? static_cast<std::uint64_t>(config.options.numSamples) * config.channels_in * bytesPerSample : ~0ull;

        ctLame::EncodeGraph::Settings settings;
        settings.inPath          = inPath;
        settings.outPath         = outPath;
        settings.dataOffset      = wavFile.DataOffset();
        settings.byteLimit       = byteLimit;
        settings.maxInputSamples = MaxInputSamples(wavFile, inPath, config.channels_in, byteLimit);
        settings.format          = {config.channels_in, wavFile.PcmBitWidth(), wavFile.IsIeeeFloat(), frontend.swapChannel != 0};
        settings.threads         = frontend.threads > 0 ? static_cast<std::uint32_t>(frontend.threads) : GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        settings.sequential      = frontend.sequential != 0;
        // CTLAME_IO=readKB,count[,firstKB]: the read size, reads in flight and a growing first read (measurements)
        std::array<wchar_t, 64> io{};
        if (GetEnvironmentVariableW(L"CTLAME_IO", io.data(), static_cast<DWORD>(io.size())) > 0) {
            unsigned int readKb = 64, count = 4, firstKb = 0;
            if (swscanf_s(io.data(), L"%u,%u,%u", &readKb, &count, &firstKb) >= 2 && readKb >= 4 && count >= 1 && count <= 16) {
                settings.readSize      = readKb * 1024;
                settings.readsInFlight = count;
                settings.firstReadSize = firstKb * 1024;
            }
        }
        if (profile != nullptr)
            profile->threads = static_cast<std::int32_t>(settings.threads);

        // the edges' events belong to the calling task, so the graph lives between the
        // conversion and the conversion back
        Ct::ConvertThreadToTask();
        std::optional<ctLame::EncodeGraph> graph(std::in_place, encoder, config, settings, profile != nullptr ? &profile->graph : nullptr);
        if (!graph->Initialize()) {
            graph.reset();
            Ct::ConvertTaskToThread();
            return false;
        }

        const bool asyncIo          = Ct::AsyncIo::EnableAsyncIo();
        auto       ioScheduler      = ctLame::SchedulerHandle::Create(Ct::SchedulerKind::Default, 1);
        auto       computeScheduler = ctLame::SchedulerHandle::Create(Ct::SchedulerKind::Default, settings.threads);
        const bool ok               = asyncIo && ioScheduler && computeScheduler;
        if (ok) {
            ioScheduler->Start();
            computeScheduler->Start();
            result = graph->Run(ioScheduler, computeScheduler);
            computeScheduler->Stop();
            ioScheduler->Stop();
        }
        graph.reset();
        if (asyncIo)
            Ct::AsyncIo::DisableAsyncIo();
        Ct::ConvertTaskToThread();
        return ok;
    }

    std::int32_t LameMain(std::int32_t argc, wchar_t** argv)
    {
        ctLame::EncoderOptions                   options;
        ctLame::FrontendOptions                  frontend;
        std::array<wchar_t, ctLame::PathMax + 1> inPath  = {};
        std::array<wchar_t, ctLame::PathMax + 1> outPath = {};

        options.report.message = &ctLame::FrontendMessage;
        options.report.error   = &ctLame::FrontendError;
        options.report.debug   = &ctLame::FrontendDebug;

        if (argc <= 1) {
            ctLame::Usage(ctLame::Stream::Error, argv[0]); // no command-line args, print usage, exit
            return 1;
        }

        std::int32_t ret = ctLame::ParseArgs(options, frontend, argc, argv, inPath.data(), outPath.data());
        if (ret < 0)
            return ret == -2 ? 0 : 1;

        // init_files
        if (std::wcscmp(inPath.data(), outPath.data()) == 0) {
            ctLame::ErrorPrintf(L"Input file and Output file are the same. Abort.\n");
            return -1;
        }
        ctLame::WavFile wavFile;
        if (!wavFile.Open(inPath.data(), options, frontend.ignoreWavLength != 0, frontend.silent, &ctLame::FrontendErrorW)) {
            ctLame::ErrorPrintf(L"Can't init infile '%ls'\n", inPath.data());
            return -1;
        }
        HANDLE const outFile = InitOutfile(outPath.data());
        if (outFile == INVALID_HANDLE_VALUE) {
            ctLame::ErrorPrintf(L"Can't init outfile '%ls'\n", outPath.data());
            return -1;
        }

        // Now that all the options are set, analyze them and set the internal options
        ctLame::SessionConfig config;
        ret = ctLame::InitParams(options, config);
        if (ret < 0) {
            if (ret == -1)
                ctLame::DisplayBitrates(ctLame::Stream::Error);
            ctLame::ErrorPrintf(L"fatal error during initialization\n");
            CloseHandle(outFile);
            return ret;
        }

        // the rest of lame_init_params: bit stream, quantizer and psychoacoustic model (the encoder
        // with its tables is too large for the stack)
        const auto encoder = std::make_unique<ctLame::Encoder>();
        // the SIMD level: the requested one (--simd, default the highest), at most what the CPU has
        const ctLame::CpuFeatures cpu  = ctLame::DetectCpuFeatures();
        const ctLame::SimdLevel   simd = ctLame::ChooseSimdLevel(frontend.simd, cpu);
        std::array<wchar_t, 128>  simdText;
        (void)ctLame::FormatSimdFeatures(cpu, simd, simdText.data(), static_cast<std::int32_t>(simdText.size()));
        if (frontend.features != 0)
            ctLame::ErrorPrintf(L"ctLame features: %ls\n", simdText.data());
        encoder->Initialize(config, simd);

        EncoderProgressBegin(config, frontend, inPath.data(), outPath.data());

        // read_samples_pcm checks the sample width on the first read, after the configuration
        // has been printed; LAME then encodes no samples at all.
        const std::int32_t bitWidth = wavFile.PcmBitWidth();
        if (bitWidth != 8 && bitWidth != 16 && bitWidth != 24 && bitWidth != 32 && frontend.silent < 10)
            ctLame::ErrorPrintf(L"Only 8, 16, 24 and 32 bit input files supported \n");

        // the writer task opens the file again for overlapped I/O
        CloseHandle(outFile);
        wavFile.Close();

        ctLame::EncodeGraph::Result result;
        ret = 0;
        // CTLAME_PROFILE: stage and task times on stderr after encoding
        std::optional<EncodeProfile> profile;
        if (ctLame::ProfileRequested()) {
            profile.emplace();
            encoder->SetProfile(&profile->stages);
            (void)wcscpy_s(profile->simd.data(), profile->simd.size(), simdText.data());
        }

        if (!Encode(*encoder, config, frontend, wavFile, inPath.data(), outPath.data(), result, profile ? &*profile : nullptr)) {
            ctLame::ErrorPrintf(L"ctLame: cannot start the schedulers\n");
            ret = 1;
        } else if (result.encodeError == -2) {
            ctLame::ErrorPrintf(L"INTERNAL ERROR IN VBR NEW CODE, please send bug report\n");
            ret = -1;
        } else if (result.encodeError < 0) {
            if (result.encodeError == -1)
                ctLame::ErrorPrintf(L"mp3 buffer is not big enough... \n");
            else
                ctLame::ErrorPrintf(L"mp3 internal error:  error code=%i\n", result.encodeError);
            ret = 1;
        } else if (result.writeError != ERROR_SUCCESS) {
            ctLame::ErrorPrintf(L"Error writing mp3 output \n");
            ret = 1;
        } else if (encoder->GetLameTagFrame(nullptr, 0) != 0 && frontend.silent <= 0) {
            // write_xing_frame: the writer task has put the tag frame at the start of the file
            ctLame::ConsolePrintf(L"Writing LAME Tag...");
            ctLame::ConsolePrintf(L"done\n");
        }
        if (profile)
            PrintProfile(*profile, frontend);
        return ret;
    }

} // namespace

int wmain(std::int32_t argc, wchar_t* argv[])
{
#ifdef _DEBUG
    // Debug builds run unattended in the test matrices: CRT assertions and errors (for instance a
    // std::array index out of range) go to stderr instead of a dialog, and the process ends with
    // an error instead of waiting for a button
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    // the program name in the help texts without ".exe" (ctLame.exe --help: "usage: ctLame ...")
    if (argc > 0) {
        const std::size_t length = std::wcslen(argv[0]);
        if (length > 4 && _wcsicmp(argv[0] + length - 4, L".exe") == 0)
            argv[0][length - 4] = L'\0';
    }

    ctLame::OpenConsole();
    const std::int32_t ret = LameMain(argc, argv);
    ctLame::CloseConsole();
    return ret;
}
