// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>

#include "CommandLine.h"
#include "Console.h"
#include "DebugReports.h"
#include "Formatter.h"
#include "OggOpusEncoder.h"
#include "OpusDefines.h"
#include "OpusHeaders.h"
#include "OpusPacket.h"
#include "WavInput.h"

// headers with imports last
#include "EncodeGraph.h"
#include "FileIo.h"
#include "TaskRuntime.h"

// after the headers above: Application.h imports modules, and none of their textual includes may follow it
#include "Application.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    using Shared::Console;
    using Shared::Formatter;
    using Shared::Stream;

    // EncData of opusenc.c and the output file
    struct Application::EncData {
        OggOpusEncoder* enc          = nullptr;
        HANDLE          out          = INVALID_HANDLE_VALUE;
        bool            outIsStdout  = false;
        FileIo*         io           = nullptr; // the output through the node FileWrite, else out
        std::int64_t    totalBytes   = 0;
        std::int64_t    bytesWritten = 0;
        std::int64_t    nbEncoded    = 0;
        std::int64_t    pagesOut     = 0;
        std::int64_t    packetsOut   = 0;
        std::int32_t    peakBytes    = 0;
        std::int32_t    minBytes     = 256 * 1275 * 6;
        std::int32_t    lastLength   = 0;
        std::int32_t    nbStreams    = 1;
        std::int32_t    nbCoupled    = 0;
    };

    // the spinner of opusenc's main loop
    struct Application::Progress {
        const EncData* data                   = nullptr;
        std::time_t    startTime              = 0;
        std::int64_t   totalSamplesPerChannel = 0;
        std::int32_t   lookahead              = 0;
        std::int32_t   frameSize              = 960;
        std::int32_t   bitrate                = 0;
        bool           withHardCbr            = false;
        std::time_t    lastSpin               = 0;
        std::int32_t   lastSpinLen            = 0;
    };

    Application::Application()
    {
        Shared::DebugReports::ToStderr();
    }

    bool Application::ParseArgs(int argc, wchar_t** argv)
    {
        stats_.Start();
        result_ = CommandLine::Parse(argc, argv, options_);
        return result_ == Shared::ParseResult::Run;
    }

    int Application::Execute()
    {
        const char* inFile  = options_.argvUtf8[options_.inIndex];
        const char* outFile = options_.argvUtf8[options_.outIndex];

        // The setup of ThinkMeta.ConcurrentTasks (Shared::TaskRuntime) with one I/O thread for FileRead and FileWrite;
        // this task waits for the graph and for the pipes of FileIo. The compute scheduler follows in RunEncodeGraph,
        // once the graph knows its thread count.
        Shared::TaskRuntime runtime(1);
        if (!runtime)
            Console::Fatal(L"Error: failed to start the asynchronous I/O and its scheduler\n");
        FileIo    fileIo;
        InputFile fin;
        if (!fin.Open(options_.argvWide[options_.inIndex])) {
            PrintOpenError(inFile, GetLastError());
            std::exit(1);
        }
        WavInput wav;
        if (options_.rawMode || !wav.Open(fin, options_.ignoreLength, ReportToStderr))
            Console::Fatal(L"Error: unsupported input file: %hs\n", inFile);

        WavInput::Info info = wav.GetInfo();
        if (info.rate < 100 || info.rate > 768000)
            Console::Fatal(L"Error: unsupported sample rate in input file: %ld Hz\n", static_cast<long>(info.rate));
        if (info.channels > 255 || info.channels < 1)
            Console::Fatal(L"Error: unsupported channel count in input file: %d\n"
                           L"Channel count must be in the range 1 to 255.\n",
                           info.channels);

        if (options_.downmix == 0 && info.channels > 2 && options_.bitrate > 0 && options_.bitrate < (16000 * info.channels)) {
            if (!options_.quiet)
                Console::Print(Stream::Error, L"Notice: Surround bitrate less than 16 kbit/s per channel, downmixing.\n");
            options_.downmix = info.channels > 8 ? 1 : 2;
        }
        if (options_.downmix > 0 && options_.downmix < info.channels)
            options_.downmix = wav.SetupDownmix(options_.downmix, ReportToStderr);
        else
            options_.downmix = 0;
        info = wav.GetInfo();

        const std::int32_t rate                   = info.rate;
        const std::int32_t chan                   = info.channels;
        std::int64_t       totalSamplesPerChannel = info.totalSamplesPerChannel;
        if (totalSamplesPerChannel != 0 && rate != 48000)
            totalSamplesPerChannel = static_cast<std::int64_t>(static_cast<double>(totalSamplesPerChannel) * (48000. / static_cast<double>(rate)));

        // the Opus encoder
        EncData                   data;
        OggOpusEncoder            encoder;
        OggOpusEncoder::Callbacks callbacks;
        callbacks.user   = &data;
        callbacks.write  = WriteCallback;
        callbacks.close  = CloseCallback;
        callbacks.packet = PacketCallback;
        std::int32_t ret = encoder.Create(callbacks, options_.comments, rate, chan, chan > 8 ? 255 : chan > 2 ? 1 : 0);
        if (ret != OpeOk)
            Console::Fatal(L"Error: failed to create encoder: %hs\n", OpeErrorString(ret));
        data.enc = &encoder;

        ret = encoder.SetExpertFrameDuration(options_.opusFrameParam);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPUS_SET_EXPERT_FRAME_DURATION failed: %hs\n", OpeErrorString(ret));
        ret = encoder.Ctl(OpeSetMuxingDelayRequest, options_.maxOggDelay);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPE_SET_MUXING_DELAY failed: %hs\n", OpeErrorString(ret));
        ret = encoder.Ctl(OpeSetSerialNoRequest, options_.serialNo);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPE_SET_SERIALNO failed: %hs\n", OpeErrorString(ret));
        ret = encoder.Ctl(OpeSetHeaderGainRequest, 0);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPE_SET_HEADER_GAIN failed: %hs\n", OpeErrorString(ret));
        ret = encoder.Ctl(OpeSetCommentPaddingRequest, options_.commentPadding);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPE_SET_COMMENT_PADDING failed: %hs\n", OpeErrorString(ret));

        data.nbStreams = encoder.NbStreams();
        data.nbCoupled = encoder.NbCoupledStreams();

        if (options_.bitrate < 0) {
            // lower default rate for sampling rates [8000-44100) by a factor of (rate+16k)/(64k)
            const std::int32_t r = rate < 44100 ? rate : 48000;
            std::int32_t       k = (r + 1000) / 1000;
            k                    = k < 8 ? 8 : k;
            k                    = k > 48 ? 48 : k;
            options_.bitrate     = ((64000 * data.nbStreams + 32000 * data.nbCoupled) * (k + 16) + 32) >> 6;
        }
        if (options_.bitrate > (1024000 * chan) || options_.bitrate < 500)
            Console::Fatal(L"Error: bitrate %d bits/sec is insane\n%hs"
                           L"--bitrate values from 6 to 256 kbit/s per channel are meaningful.\n",
                           options_.bitrate, options_.bitrate >= 1000000 ? "Did you mistake bits for kilobits?\n" : "");
        options_.bitrate = chan * 256000 < options_.bitrate ? chan * 256000 : options_.bitrate;

        ret = encoder.Ctl(OPUS_SET_BITRATE_REQUEST, options_.bitrate);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPUS_SET_BITRATE %d failed: %hs\n", options_.bitrate, OpeErrorString(ret));
        ret = encoder.Ctl(OPUS_SET_VBR_REQUEST, !options_.withHardCbr);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPUS_SET_VBR %d failed: %hs\n", !options_.withHardCbr, OpeErrorString(ret));
        if (!options_.withHardCbr) {
            ret = encoder.Ctl(OPUS_SET_VBR_CONSTRAINT_REQUEST, options_.withCvbr);
            if (ret != OpeOk)
                Console::Fatal(L"Error: OPUS_SET_VBR_CONSTRAINT %d failed: %hs\n", options_.withCvbr, OpeErrorString(ret));
        }
        ret = encoder.Ctl(OPUS_SET_SIGNAL_REQUEST, options_.signalType);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPUS_SET_SIGNAL failed: %hs\n", OpeErrorString(ret));
        ret = encoder.Ctl(OPUS_SET_COMPLEXITY_REQUEST, options_.complexity);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPUS_SET_COMPLEXITY %d failed: %hs\n", options_.complexity, OpeErrorString(ret));
        ret = encoder.Ctl(OPUS_SET_PACKET_LOSS_PERC_REQUEST, options_.expectLoss);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPUS_SET_PACKET_LOSS_PERC %d failed: %hs\n", options_.expectLoss, OpeErrorString(ret));
        {
            std::int32_t lsb = info.sampleBits < 24 ? info.sampleBits : 24;
            lsb              = lsb > 8 ? lsb : 8;
            ret              = encoder.Ctl(OPUS_SET_LSB_DEPTH_REQUEST, lsb);
            if (ret != OpeOk)
                Console::Print(Stream::Error, L"Warning: OPUS_SET_LSB_DEPTH failed: %hs\n", OpeErrorString(ret));
        }
        if (options_.noPhaseInv) {
            ret = encoder.Ctl(OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST, 1);
            if (ret != OpeOk)
                Console::Print(Stream::Error, L"Warning: OPUS_SET_PHASE_INVERSION_DISABLED failed: %hs\n", OpeErrorString(ret));
        }

        // the last SET ctls, so they can override the defaults
        for (std::int32_t i = 0; i < options_.optCtls; ++i) {
            const std::int32_t target = options_.optCtlValues[i * 3];
            if (target == -1) {
                ret = encoder.Ctl(options_.optCtlValues[i * 3 + 1], options_.optCtlValues[i * 3 + 2]);
                if (ret != OpeOk)
                    Console::Fatal(L"Error: failed to set encoder ctl %d=%d: %hs\n", options_.optCtlValues[i * 3 + 1], options_.optCtlValues[i * 3 + 2],
                                   OpeErrorString(ret));
            } else if (target < data.nbStreams) {
                ret = encoder.StreamCtl(target, options_.optCtlValues[i * 3 + 1], options_.optCtlValues[i * 3 + 2]);
                if (ret != OPUS_OK)
                    Console::Fatal(L"Error: failed to set stream %d encoder ctl %d=%d: %hs\n", target, options_.optCtlValues[i * 3 + 1],
                                   options_.optCtlValues[i * 3 + 2], OpusStrError(ret));
            } else {
                Console::Fatal(L"Error: --set-ctl-int stream %d is higher than the highest stream number %d\n", target, data.nbStreams - 1);
            }
        }

        // the lookahead late, so user ctls can change it
        std::int32_t lookahead = 0;
        ret                    = encoder.Get(OPUS_GET_LOOKAHEAD_REQUEST, lookahead);
        if (ret != OpeOk)
            Console::Fatal(L"Error: OPUS_GET_LOOKAHEAD failed: %hs\n", OpeErrorString(ret));

        if (!options_.quiet) {
            std::int32_t opusApp = 0;
            Console::Print(Stream::Error, L"Encoding using %hs", options_.opusVersion);
            ret = encoder.Get(OPUS_GET_APPLICATION_REQUEST, opusApp);
            if (ret != OpeOk)
                Console::Print(Stream::Error, L"\n");
            else if (opusApp == OPUS_APPLICATION_VOIP)
                Console::Print(Stream::Error, L" (VoIP)\n");
            else if (opusApp == OPUS_APPLICATION_AUDIO)
                Console::Print(Stream::Error, L" (audio)\n");
            else if (opusApp == OPUS_APPLICATION_RESTRICTED_LOWDELAY)
                Console::Print(Stream::Error, L" (low-delay)\n");
            else
                Console::Print(Stream::Error, L" (unknown application)\n");
            Console::Print(Stream::Error, L"-----------------------------------------------------\n");
            Console::Print(Stream::Error, L"   Input: %0.6g kHz, %d channel%hs\n", rate / 1000., chan, chan < 2 ? "" : "s");
            Console::Print(Stream::Error, L"  Output: %d channel%hs (", chan, chan < 2 ? "" : "s");
            if (data.nbCoupled > 0)
                Console::Print(Stream::Error, L"%d coupled", data.nbCoupled * 2);
            if (data.nbStreams - data.nbCoupled > 0)
                Console::Print(Stream::Error, L"%hs%d uncoupled", data.nbCoupled > 0 ? ", " : "", data.nbStreams - data.nbCoupled);
            Console::Print(Stream::Error, L")\n          %0.2gms packets, %0.6g kbit/s%hs\n", options_.frameSize / (48000 / 1000.), options_.bitrate / 1000.,
                           options_.withHardCbr    ? " CBR"
                               : options_.withCvbr ? " CVBR"
                                                   : " VBR");
            Console::Print(Stream::Error, L" Preskip: %d\n", lookahead);
            Console::Print(Stream::Error, L"\n");
        }

        if (std::strcmp(outFile, "-") == 0) {
            data.out         = GetStdHandle(STD_OUTPUT_HANDLE);
            data.outIsStdout = true;
        } else {
            const std::uint32_t error = fileIo.CreateOutput(options_.argvWide[options_.outIndex]);
            if (error != ERROR_SUCCESS) {
                PrintOpenError(outFile, error);
                std::exit(1);
            }
            data.io = &fileIo;
        }

        HeapArray<float> input;
        if (!input.Allocate(static_cast<std::size_t>(options_.frameSize) * chan))
            Console::Fatal(L"Error: failed to allocate sample buffer\n");

        Progress progress;
        progress.data                   = &data;
        progress.startTime              = options_.startTime;
        progress.totalSamplesPerChannel = totalSamplesPerChannel;
        progress.lookahead              = lookahead;
        progress.frameSize              = options_.frameSize;
        progress.bitrate                = options_.bitrate;
        progress.withHardCbr            = options_.withHardCbr;

        // the task graph of the encoder (EncodeGraph): resampling (at 48 kHz a copy) on worker
        // tasks, the encoder in order (the same output); the sequential graph for --sequential.
        // The graph owns its edges and their events; Run waits for its TaskGroup, so the calling
        // thread waits as a task (ConvertThreadToTask above).
        EncodeProgress encodeProgress;
        if (!options_.quiet) {
            encodeProgress.callback = ShowProgressCallback;
            encodeProgress.user     = &progress;
        }
        EncodeGraph::Settings settings;
        settings.threads    = static_cast<std::uint32_t>(options_.threads);
        settings.lookahead  = lookahead;
        settings.complexity = options_.complexity;
        settings.progress   = encodeProgress;
        // the rest of the input file behind the WAV header through FileRead (not for stdin)
        settings.asyncInput = fileIo.AttachInput(fin, options_.argvWide[options_.inIndex]);
        fileIo.Start(*runtime.Io());
        stats_.WorkBegins();
        ret                = RunEncodeGraph(wav, encoder, settings, options_.sequential, input.Data(), options_.frameSize, *runtime.Io());
        const bool written = fileIo.Finish();
        stats_.WorkEnds();
        runtime.End();
        if (ret == OpeOk && !written)
            ret = OpeCloseFail;
        if (progress.lastSpinLen != 0) {
            Console::Print(Stream::Error, L"\r");
            for (std::int32_t i = 0; i < progress.lastSpinLen; ++i)
                Console::Print(Stream::Error, L" ");
            Console::Print(Stream::Error, L"\r");
        }

        if (ret != OpeOk)
            Console::Fatal(L"Encoding aborted: %hs\n", OpeErrorString(ret));
        const std::time_t stopTime = std::time(nullptr);

        if (!options_.quiet) {
            const double codedSeconds = data.nbEncoded / 48000.;
            const double wallTime     = static_cast<double>(stopTime - options_.startTime);
            Console::Print(Stream::Error, L"Encoding complete\n");
            Console::Print(Stream::Error, L"-----------------------------------------------------\n");
            Console::Print(Stream::Error, L"       Encoded:");
            PrintTime(codedSeconds);
            Console::Print(Stream::Error, L"\n       Runtime:");
            PrintTime(wallTime);
            Console::Print(Stream::Error, L"\n");
            if (wallTime > 0)
                Console::Print(Stream::Error, L"                (%0.4gx realtime)\n", codedSeconds / wallTime);
            Console::Print(Stream::Error, L"         Wrote: %lld bytes, %lld packets, %lld pages\n", data.bytesWritten, data.packetsOut, data.pagesOut);
            if (data.nbEncoded > 0) {
                Console::Print(Stream::Error, L"       Bitrate: %0.6g kbit/s (without overhead)\n", data.totalBytes * 8.0 / (codedSeconds) / 1000.0);
                Console::Print(Stream::Error,
                               L" Instant rates: %0.6g to %0.6g kbit/s\n"
                               L"                (%d to %d bytes per packet)\n",
                               data.minBytes * (8 * 48000. / options_.frameSize / 1000.), data.peakBytes * (8 * 48000. / options_.frameSize / 1000.),
                               data.minBytes, data.peakBytes);
            }
            if (data.bytesWritten > 0)
                Console::Print(Stream::Error, L"      Overhead: %0.3g%% (container+metadata)\n",
                               (data.bytesWritten - data.totalBytes) / static_cast<double>(data.bytesWritten) * 100.);
            Console::Print(Stream::Error, L"\n");
        }
        if (options_.stats) {
            const std::uint32_t threads = options_.sequential ? 1u
                : options_.threads > 0                        ? static_cast<std::uint32_t>(options_.threads)
                                                              : Shared::TaskRuntime::LogicalProcessors();
            stats_.Print(threads, options_.sequential);
        }
        return 0;
    }

    int Application::ParseError()
    {
        return result_ == Shared::ParseResult::Exit ? 0 : 1;
    }

    void Application::ReportToStderr(const wchar_t* format, std::va_list args)
    {
        Console::PrintV(Stream::Error, format, args);
    }

    // print_time
    void Application::PrintTime(double seconds)
    {
        const auto hours = static_cast<std::int64_t>(seconds / 3600);
        seconds -= hours * 3600.;
        const auto minutes = static_cast<std::int64_t>(seconds / 60);
        seconds -= minutes * 60.;
        if (hours != 0)
            Console::Print(Stream::Error, L" %lld hour%hs%hs", hours, hours != 1 ? "s" : "",
                           minutes != 0 && seconds > 0       ? ","
                               : minutes != 0 || seconds > 0 ? " and"
                                                             : "");
        if (minutes != 0)
            Console::Print(Stream::Error, L" %lld minute%hs%hs", minutes, minutes != 1 ? "s" : "", seconds > 0 ? (hours != 0 ? ", and" : " and") : "");
        if (seconds > 0 || (hours == 0 && minutes == 0))
            Console::Print(Stream::Error, L" %0.4g second%hs", seconds, seconds != 1 ? "s" : "");
    }

    // perror() for a file that cannot be opened
    void Application::PrintOpenError(const char* name, unsigned long error)
    {
        const char* text = "Invalid argument";
        switch (error) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_INVALID_NAME:
            case ERROR_INVALID_DRIVE:
            case ERROR_BAD_NETPATH: text = "No such file or directory"; break;
            case ERROR_ACCESS_DENIED:
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:
            case ERROR_WRITE_PROTECT: text = "Permission denied"; break;
            case ERROR_TOO_MANY_OPEN_FILES: text = "Too many open files"; break;
            case ERROR_DISK_FULL: text = "No space left on device"; break;
            default: break;
        }
        Console::Print(Stream::Error, L"%hs: %hs\n", name, text);
    }

    bool Application::WriteCallback(void* user, const std::uint8_t* data, std::int32_t length)
    {
        auto* d = static_cast<EncData*>(user);
        d->bytesWritten += length;
        ++d->pagesOut;
        if (d->io != nullptr)
            return d->io->Write(data, length);
        unsigned long written = 0; // DWORD of WriteFile
        return WriteFile(d->out, data, static_cast<unsigned long>(length), &written, nullptr) && written == static_cast<unsigned long>(length);
    }

    bool Application::CloseCallback(void* user)
    {
        auto* d = static_cast<EncData*>(user);
        if (d->io != nullptr) {
            d->io->EndOutput(); // FileIo::Finish waits for the last write and closes the file
            return true;
        }
        const bool ok = CloseHandle(d->out) != 0;
        d->out        = INVALID_HANDLE_VALUE;
        return ok;
    }

    void Application::PacketCallback(void* user, const std::uint8_t* packet, std::int32_t length)
    {
        auto*     d         = static_cast<EncData*>(user);
        const int nbSamples = PacketGetNbSamples(packet, length, 48000);
        if (nbSamples <= 0)
            return; // header packets
        d->totalBytes += length;
        d->peakBytes = length > d->peakBytes ? length : d->peakBytes;
        d->minBytes  = length < d->minBytes ? length : d->minBytes;
        d->nbEncoded += nbSamples;
        ++d->packetsOut;
        d->lastLength = length;
    }

    void Application::ShowProgress(Progress& p)
    {
        const std::time_t stopTime = std::time(nullptr);
        if (stopTime <= p.lastSpin)
            return;
        const EncData&           data         = *p.data;
        double                   estBitrate   = 0;
        const double             codedSeconds = data.nbEncoded / 48000.;
        const double             wallTime     = static_cast<double>(stopTime - p.startTime);
        wchar_t                  sbuf[55];
        static constexpr wchar_t Spinner[] = L"|/-\\";
        if (p.withHardCbr) {
            estBitrate = data.lastLength * (8 * 48000. / p.frameSize);
        } else if (data.nbEncoded <= 0) {
            estBitrate = 0;
        } else {
            const double tweight = 1. / (1 + std::exp(-((codedSeconds / 10.) - 3.)));
            estBitrate           = (data.totalBytes * 8.0 / codedSeconds) * tweight + p.bitrate * (1. - tweight);
        }
        Console::Print(Stream::Error, L"\r");
        for (std::int32_t i = 0; i < p.lastSpinLen; ++i)
            Console::Print(Stream::Error, L" ");
        if (p.totalSamplesPerChannel > 0 && data.nbEncoded < p.totalSamplesPerChannel + p.lookahead)
            Formatter::Format(sbuf, 54, L"\r[%c] %2d%% ", Spinner[p.lastSpin & 3],
                              static_cast<int>(std::floor(data.nbEncoded / static_cast<double>(p.totalSamplesPerChannel + p.lookahead) * 100.)));
        else
            Formatter::Format(sbuf, 54, L"\r[%c] ", Spinner[p.lastSpin & 3]);
        p.lastSpinLen = static_cast<std::int32_t>(std::wcslen(sbuf));
        Formatter::Format(sbuf + p.lastSpinLen, 54 - p.lastSpinLen, L"%02d:%02d:%02d.%02d %4.3gx realtime, %5.4g kbit/s", static_cast<int>(codedSeconds / 3600),
                          static_cast<int>(codedSeconds / 60) % 60, static_cast<int>(codedSeconds) % 60, static_cast<int>(codedSeconds * 100) % 100,
                          codedSeconds / (wallTime > 0 ? wallTime : 1e-6), estBitrate / 1000.);
        Console::Print(Stream::Error, L"%ls", sbuf);
        p.lastSpinLen = static_cast<std::int32_t>(std::wcslen(sbuf));
        p.lastSpin    = stopTime;
    }

    void Application::ShowProgressCallback(void* user)
    {
        ShowProgress(*static_cast<Progress*>(user));
    }

    // The encoder's task graph on a compute scheduler of its own (the I/O scheduler already runs FileRead and
    // FileWrite). The parallel graph (EncodeGraph) allocates its rings and stores in Initialize; if that fails, and
    // for --sequential, the sequential graph encodes on one thread. Both write the same output.
    std::int32_t Application::RunEncodeGraph(WavInput& wav, OggOpusEncoder& encoder, const EncodeGraph::Settings& settings, bool sequential, float* buffer,
                                             std::int32_t frameSize, ITaskScheduler& io)
    {
        if (!sequential) {
            EncodeGraph graph(wav, encoder, settings);
            if (graph.Initialize()) {
                Shared::RunningScheduler compute(SchedulerKind::Default, graph.Threads());
                if (!compute)
                    return OpeAllocFail;
                return graph.Run(io, *compute);
            }
        }
        SequentialGraph          graph(wav, encoder, buffer, frameSize, settings.progress);
        Shared::RunningScheduler compute(SchedulerKind::Default, 1);
        if (!compute)
            return OpeAllocFail;
        return graph.Run(*compute);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
