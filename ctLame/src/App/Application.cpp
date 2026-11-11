// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <fcntl.h>
#    include <unistd.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
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
        StageProfile                  stages; // per stage of the encoder
        EncodeGraph::Profile          graph;  // per node of the task graph
        std::int32_t                  threads = 0;
        std::array<Shared::Char, 128> simd{}; // the feature line
    };

    Application::Application()
    {
        Shared::DebugReports::ToStderr();
    }

    bool Application::ParseArgs(std::int32_t argc, Shared::Char** argv)
    {
        stats_.Start();
        result_ = CommandLine::Parse(argc, argv, options_);
        return result_ == Shared::ParseResult::Run;
    }

    std::int32_t Application::Execute()
    {
        // init_files
        if (Shared::Text::Equal(options_.inPath.data(), options_.outPath.data()) || Shared::File::Same(options_.inPath.data(), options_.outPath.data())) {
            Console::Print(Stream::Error, CT_TEXT("Input file and Output file are the same. Abort.\n"));
            return -1;
        }
        WavFile wavFile;
        if (!wavFile.Open(options_.inPath.data(), options_.encoder, options_.ignoreWavLength != 0, options_.silent)) {
            Console::Print(Stream::Error, CT_TEXT("Can't init infile '%s'\n"), options_.inPath.data());
            return -1;
        }
        if (!InitOutfile(options_.outPath.data())) {
            Console::Print(Stream::Error, CT_TEXT("Can't init outfile '%s'\n"), options_.outPath.data());
            return -1;
        }

        // Now that all the options are set, analyze them and set the internal options
        SessionConfig config;
        std::int32_t  ret = InitParams(options_.encoder, config);
        if (ret < 0) {
            if (ret == -1)
                CommandLine::DisplayBitrates(Stream::Error);
            Console::Print(Stream::Error, CT_TEXT("fatal error during initialization\n"));
            return ret;
        }

        // the rest of lame_init_params: bit stream, quantizer and psychoacoustic model (the encoder
        // with its tables is too large for the stack)
        const auto encoder = std::make_unique<Encoder>();
        // the SIMD level: the requested one (--simd, default the highest), at most what the CPU has
        const CpuFeatures             cpu  = CpuFeatures::Detect();
        const SimdLevel               simd = ChooseSimdLevel(options_.simd, cpu);
        std::array<Shared::Char, 128> simdText;
        (void)cpu.FormatLine(SimdLevelName(simd), simdText.data(), static_cast<std::int32_t>(simdText.size()));
        if (options_.features != 0)
            Console::Print(Stream::Error, CT_TEXT("ctLame features: %s\n"), simdText.data());
        encoder->Initialize(config, simd);

        EncoderProgressBegin(config, options_, options_.inPath.data(), options_.outPath.data());

        // lame_encoder_loop: the ID3v2 tag goes in front of the MP3 stream (if its memory is not available, LAME
        // leaves it out), the ID3v1 tag behind it (write_id3v1_tag); the encode node writes both
        Id3Bytes                     id3;
        std::unique_ptr<std::byte[]> id3v2;
        std::array<std::byte, 128>   id3v1{};
        const std::size_t            id3v2Size = options_.id3.GetV2Tag(nullptr, 0, config.options.numSamples, config.samplerate_in);
        if (id3v2Size > 0) {
            id3v2.reset(new (std::nothrow) std::byte[id3v2Size]);
            if (id3v2 != nullptr) {
                id3.v2     = id3v2.get();
                id3.v2Size = static_cast<std::uint32_t>(options_.id3.GetV2Tag(id3v2.get(), id3v2Size, config.options.numSamples, config.samplerate_in));
            }
        }
        id3.v1     = id3v1.data();
        id3.v1Size = static_cast<std::uint32_t>(options_.id3.GetV1Tag(id3v1.data(), id3v1.size()));

        // read_samples_pcm checks the sample width on the first read, after the configuration
        // has been printed; LAME then encodes no samples at all.
        const std::int32_t bitWidth = wavFile.PcmBitWidth();
        if (bitWidth != 8 && bitWidth != 16 && bitWidth != 24 && bitWidth != 32 && options_.silent < 10)
            Console::Print(Stream::Error, CT_TEXT("Only 8, 16, 24 and 32 bit input files supported \n"));

        // the writer task opens the file again for asynchronous I/O
        wavFile.Close();

        EncodeGraph::Result result;
        ret = 0;
        // --stats or CTLAME_PROFILE: stage and task times on stderr after encoding
        std::optional<EncodeProfile> profile;
        if (options_.stats != 0 || ProfileRequested()) {
            profile.emplace();
            encoder->SetProfile(&profile->stages);
            (void)Shared::Text::Copy(profile->simd.data(), profile->simd.size(), simdText.data());
        }

        const Shared::Char* failure = CT_TEXT("");
        const bool          started = RunEncodeGraph(*encoder, config, options_, id3, wavFile, options_.inPath.data(), options_.outPath.data(), result,
                                                     profile ? &*profile : nullptr, stats_, failure);
        // get_audio: a read error ends the input like its end; LAME reports it, still flushes the
        // encoder and writes the tag, and exits with 0
        if (started && result.readError != 0 && options_.silent < 10)
            Console::Print(Stream::Error, CT_TEXT("Error reading input file\n"));
        if (!started) {
            Console::Print(Stream::Error, CT_TEXT("%s"), failure);
            ret = 1;
        } else if (result.encodeError == -2) {
            Console::Print(Stream::Error, CT_TEXT("INTERNAL ERROR IN VBR NEW CODE, please send bug report\n"));
            ret = -1;
        } else if (result.encodeError < 0) {
            if (result.encodeError == -1)
                Console::Print(Stream::Error, CT_TEXT("mp3 buffer is not big enough... \n"));
            else
                Console::Print(Stream::Error, CT_TEXT("mp3 internal error:  error code=%i\n"), result.encodeError);
            ret = 1;
        } else if (result.writeError != 0) {
            Console::Print(Stream::Error, CT_TEXT("Error writing mp3 output \n"));
            ret = 1;
        } else if (encoder->GetLameTagFrame(nullptr, 0) != 0 && options_.silent <= 0) {
            // write_xing_frame: the writer task has put the tag frame at the start of the file
            Console::Print(Stream::Error, CT_TEXT("Writing LAME Tag..."));
            Console::Print(Stream::Error, CT_TEXT("done\n"));
        }
        // print_trailing_info
        if (started && ret == 0 && options_.silent <= 0 && config.options.findReplayGain != 0) {
            const std::int32_t radioGain = encoder->Result().radio_gain;
            Console::Print(Stream::Error, CT_TEXT("ReplayGain: %s%.1fdB\n"), radioGain > 0 ? CT_TEXT("+") : CT_TEXT(""), static_cast<float>(radioGain) / 10.0);
            if (radioGain > 0x1FE || radioGain < -0x1FE)
                Console::Print(Stream::Error,
                               CT_TEXT("WARNING: ReplayGain exceeds the -51dB to +51dB range. Such a result is too\n"
                                       "         high to be stored in the header.\n"));
        }
        if (profile)
            PrintProfile(*profile);
        return ret;
    }

    std::int32_t Application::ParseError()
    {
        return result_ == Shared::ParseResult::Exit ? 0 : 1;
    }

    // init_outfile: like lame_fopen(outPath, "w+b"); the file is created (emptied) and closed again, the writer task
    // opens it for asynchronous I/O.
    bool Application::InitOutfile(const Shared::Char* outPath)
    {
#if defined(_WIN32)
        const HANDLE file
            = CreateFileW(outPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        CloseHandle(file);
        return true;
#else
        const int file = ::open(outPath, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        return file >= 0 && ::close(file) == 0;
#endif
    }

    // encoder_progress_begin (timestatus.c): what is printed before encoding starts; the progress
    // report and the VBR histogram were removed.
    void Application::EncoderProgressBegin(const SessionConfig& config, const Options& frontend, const Shared::Char* inPath, const Shared::Char* outPath)
    {
        if (frontend.silent < 9) {
            const auto pw = static_cast<std::size_t>(ConsoleWidth - 14);

            PrintConfig(config); // print useful information about options being used
            Console::Print(Stream::Error, CT_TEXT("Encoding %s%s to %s\n"), inPath,
                           Shared::Text::Length(inPath) + Shared::Text::Length(outPath) < pw ? CT_TEXT("") : CT_TEXT("\n     "), outPath);
            Console::Print(Stream::Error, CT_TEXT("Encoding as %g kHz "), 1.e-3 * config.samplerate_out);
            {
                static const std::array<std::array<const Shared::Char*, 4>, 2> modeNames
                    = {{{CT_TEXT("stereo"), CT_TEXT("j-stereo"), CT_TEXT("dual-ch"), CT_TEXT("single-ch")},
                        {CT_TEXT("stereo"), CT_TEXT("force-ms"), CT_TEXT("dual-ch"), CT_TEXT("single-ch")}}};
                Console::Print(Stream::Error, CT_TEXT("%s MPEG-1 Layer III VBR(q=%g)\n"), modeNames[config.options.forceMs][config.options.mode],
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
        Console::Print(Stream::Error, CT_TEXT("  %lld frames, features: %s\n"), frames, profile.simd.data());
        Console::Print(Stream::Error, CT_TEXT("  stage           ms   share   us/frame\n"));
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(ProfileStage::Count); ++i) {
            const std::int64_t ticks = profile.stages.ticks[i].load();
            Console::Print(Stream::Error, CT_TEXT("  %-10s %8.1f %6.1f %% %9.2f\n"), ProfileStageName(static_cast<ProfileStage>(i)), ticks / perMs,
                           total > 0 ? 100.0 * ticks / total : 0.0, frames > 0 ? 1000.0 * ticks / perMs / frames : 0.0);
        }
        Console::Print(Stream::Error, CT_TEXT("  node          busy ms  idle ms\n"));
        struct TaskLine {
            const Shared::Char* name;
            const TaskProfile*  task;
        };
        const auto&                   graph = profile.graph;
        const std::array<TaskLine, 7> tasks = {{{CT_TEXT("read"), &graph.read},
                                                {CT_TEXT("dispatch"), &graph.dispatch},
                                                {CT_TEXT("worker"), &graph.worker},
                                                {CT_TEXT("resolve"), &graph.resolve},
                                                {frontend.sequential ? CT_TEXT("sequential") : CT_TEXT("encode"), &graph.encode},
                                                {CT_TEXT("gain"), &graph.gain},
                                                {CT_TEXT("write"), &graph.write}}};
        for (const auto& t : tasks) {
            const std::int32_t count = t.task->tasks.load();
            if (count == 0)
                continue; // not in the graph (dispatch, workers and resolve with --sequential)
            const std::int64_t life = t.task->life.load();
            const std::int64_t idle = t.task->idle.load();
            Console::Print(Stream::Error, CT_TEXT("  %-10s %10.1f %8.1f%s"), t.name, (life - idle) / perMs, idle / perMs,
                           count > 1 ? CT_TEXT("") : CT_TEXT("\n"));
            if (count > 1)
                Console::Print(Stream::Error, CT_TEXT("  (%d tasks)\n"), count);
        }
    }

    // Samples per channel the input can have at most: the bytes the reader may read.
    std::uint64_t Application::MaxInputSamples(const WavFile& wavFile, const Shared::Char* inPath, std::int32_t channels, std::uint64_t byteLimit)
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
    bool Application::RunEncodeGraph(Encoder& encoder, const SessionConfig& config, const Options& frontend, const Id3Bytes& id3, const WavFile& wavFile,
                                     const Shared::Char* inPath, const Shared::Char* outPath, EncodeGraph::Result& result, EncodeProfile* profile,
                                     Shared::RunStats& stats, const Shared::Char*& failure)
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
        settings.id3             = id3;
        if (profile != nullptr)
            profile->threads = static_cast<std::int32_t>(settings.threads);

        // The setup of ThinkMeta.ConcurrentTasks (Shared::TaskRuntime) with one I/O thread for Read and Write, and a
        // compute scheduler with one thread per worker. In reverse order on return: the compute scheduler stops, the
        // graph goes (and closes its files), then the I/O scheduler, async I/O and the task.
        Shared::TaskRuntime runtime(1);
        EncodeGraph         graph(encoder, config, settings, profile != nullptr ? &profile->graph : nullptr);
        if (!graph.Initialize()) {
            failure = CT_TEXT("ctLame: out of memory\n");
            return false;
        }
        Shared::RunningScheduler compute(SchedulerKind::Default, settings.threads);
        if (!runtime || !compute) {
            failure = CT_TEXT("ctLame: cannot start the schedulers\n");
            return false;
        }
        stats.WorkBegins();
        result = graph.Run(*runtime.Io(), *compute);
        stats.WorkEnds();
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
