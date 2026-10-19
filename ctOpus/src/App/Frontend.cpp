// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "Frontend.h"

#include <Windows.h>

#include <process.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "Console.h"
#include "OggOpusEncoder.h"
#include "OpusDefines.h"
#include "OpusHeaders.h"
#include "OpusPacket.h"
#include "WavInput.h"
#include "ctOpusVersion.h"

// headers with imports last
#include "EncodeGraph.h"
#include "FileIo.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

        // The strings opusenc and libopusenc write into the stream (OpusTags); kept for bit identity.
        constexpr const char* PackageName       = "opus-tools";
        constexpr const char* PackageVersion    = "0.2";
        constexpr const char* LibopusencVersion = "libopusenc 0.2.1";

        constexpr std::int32_t EncoderStringSize = 1024;

        struct LongOption {
            const char* name;
            bool        hasArgument;
        };

        // opusenc's long_options, then the ctOpus options
        constexpr LongOption LongOptions[] = {{"quiet", false},
                                              {"bitrate", true},
                                              {"hard-cbr", false},
                                              {"vbr", false},
                                              {"cvbr", false},
                                              {"music", false},
                                              {"speech", false},
                                              {"comp", true},
                                              {"complexity", true},
                                              {"framesize", true},
                                              {"expect-loss", true},
                                              {"downmix-mono", false},
                                              {"downmix-stereo", false},
                                              {"no-downmix", false},
                                              {"no-phase-inv", false},
                                              {"max-delay", true},
                                              {"serial", true},
                                              {"save-range", true},
                                              {"set-ctl-int", true},
                                              {"help", false},
                                              {"help-picture", false},
                                              {"raw", false},
                                              {"raw-bits", true},
                                              {"raw-rate", true},
                                              {"raw-chan", true},
                                              {"raw-endianness", true},
                                              {"ignorelength", false},
                                              {"version", false},
                                              {"version-short", false},
                                              {"comment", true},
                                              {"artist", true},
                                              {"title", true},
                                              {"album", true},
                                              {"tracknumber", true},
                                              {"date", true},
                                              {"genre", true},
                                              {"picture", true},
                                              {"padding", true},
                                              {"discard-comments", false},
                                              {"discard-pictures", false},
                                              {"threads", true},
                                              {"sequential", false}};

        void Version(const char* opusVersion)
        {
            Print(Stream::Output, "ctOpus %s, compatible with opusenc %s %s (using %s)\n", CTOPUS_VERSION_STRING, PackageName, PackageVersion, opusVersion);
            Print(Stream::Output, "%s\n", CTOPUS_COPYRIGHT);
            Print(Stream::Output, "opusenc: Copyright (C) 2008-2018 Xiph.Org Foundation\n");
        }

        void Usage()
        {
            const auto p = [](const char* text) { Print(Stream::Output, "%s", text); };
            p("Usage: opusenc [options] input_file output_file.opus\n");
            p("\n");
            p("Encode audio using Opus.\n");
            p("The input format can be Wave.\n");
            p("\ninput_file can be:\n");
            p("  filename.wav      file\n");
            p("  -                 stdin\n");
            p("\noutput_file can be:\n");
            p("  filename.opus     compressed file\n");
            p("  -                 stdout\n");
            p("\nGeneral options:\n");
            p(" -h, --help         Show this help\n");
            p(" -V, --version      Show version information\n");
            p(" --help-picture     Show help on attaching album art\n");
            p(" --quiet            Enable quiet mode\n");
            p("\nEncoding options:\n");
            p(" --bitrate n.nnn    Set target bitrate in kbit/s (6-256/channel)\n");
            p(" --vbr              Use variable bitrate encoding (default)\n");
            p(" --cvbr             Use constrained variable bitrate encoding\n");
            p(" --hard-cbr         Use hard constant bitrate encoding\n");
            p(" --music            Tune low bitrates for music (override automatic detection)\n");
            p(" --speech           Tune low bitrates for speech (override automatic detection)\n");
            p(" --comp n           Set encoding complexity (0-10, default: 10 (slowest))\n");
            p(" --framesize n      Set maximum frame size in milliseconds\n");
            p("                      (2.5, 5, 10, 20, 40, 60, default: 20)\n");
            p(" --expect-loss n    Set expected packet loss in percent (default: 0)\n");
            p(" --downmix-mono     Downmix to mono\n");
            p(" --downmix-stereo   Downmix to stereo (if >2 channels)\n");
            p(" --no-phase-inv     Disable use of phase inversion for intensity stereo\n");
            p(" --max-delay n      Set maximum container delay in milliseconds\n");
            p("                      (0-1000, default: 1000)\n");
            p("\nMetadata options:\n");
            p(" --title title      Set track title\n");
            p(" --artist artist    Set artist or author, may be used multiple times\n");
            p(" --album album      Set album or collection\n");
            p(" --tracknumber n    Set track number\n");
            p(" --genre genre      Set genre, may be used multiple times\n");
            p(" --date YYYY-MM-DD  Set date of track (YYYY, YYYY-MM, or YYYY-MM-DD)\n");
            p(" --comment tag=val  Add the given string as an extra comment\n");
            p("                      This may be used multiple times\n");
            p(" --padding n        Reserve n extra bytes for metadata (default: 512)\n");
            p(" --discard-comments Don't keep metadata when transcoding\n");
            p(" --discard-pictures Don't keep pictures when transcoding\n");
            p("\nInput options:\n");
            p(" --ignorelength     Ignore the data length in Wave headers\n");
            p("\nDiagnostic options:\n");
            p(" --serial n         Force use of a specific stream serial number\n");
            p(" --set-ctl-int x=y  Pass the encoder control x with value y (advanced)\n");
            p("                      Preface with s: to direct the ctl to multistream s\n");
            p("                      This may be used multiple times\n");
            p("\nctOpus options (not stored in ENCODER_OPTIONS):\n");
            p(" --threads n        Number of worker threads (default: all logical processors)\n");
            p(" --sequential       Encode on one thread (the reference path)\n");
        }

        // print_time
        void PrintTime(double seconds)
        {
            const auto hours = static_cast<std::int64_t>(seconds / 3600);
            seconds -= hours * 3600.;
            const auto minutes = static_cast<std::int64_t>(seconds / 60);
            seconds -= minutes * 60.;
            if (hours != 0)
                ErrorPrintf(" %lld hour%s%s", hours, hours != 1 ? "s" : "", minutes != 0 && seconds > 0 ? "," : minutes != 0 || seconds > 0 ? " and" : "");
            if (minutes != 0)
                ErrorPrintf(" %lld minute%s%s", minutes, minutes != 1 ? "s" : "", seconds > 0 ? (hours != 0 ? ", and" : " and") : "");
            if (seconds > 0 || (hours == 0 && minutes == 0))
                ErrorPrintf(" %0.4g second%s", seconds, seconds != 1 ? "s" : "");
        }

        // perror() for a file that cannot be opened
        void PrintOpenError(const char* name, unsigned long error)
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
            ErrorPrintf("%s: %s\n", name, text);
        }

        // is_valid_ctl
        bool IsValidCtl(std::int32_t request) noexcept
        {
            switch (request) {
                case OPUS_SET_APPLICATION_REQUEST:
                case OPUS_SET_BITRATE_REQUEST:
                case OPUS_SET_MAX_BANDWIDTH_REQUEST:
                case OPUS_SET_VBR_REQUEST:
                case OPUS_SET_BANDWIDTH_REQUEST:
                case OPUS_SET_COMPLEXITY_REQUEST:
                case OPUS_SET_INBAND_FEC_REQUEST:
                case OPUS_SET_PACKET_LOSS_PERC_REQUEST:
                case OPUS_SET_DTX_REQUEST:
                case OPUS_SET_VBR_CONSTRAINT_REQUEST:
                case OPUS_SET_FORCE_CHANNELS_REQUEST:
                case OPUS_SET_SIGNAL_REQUEST:
                case OPUS_SET_LSB_DEPTH_REQUEST:
                case OPUS_SET_PREDICTION_DISABLED_REQUEST:
                case OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST:
                case OpeSetDecisionDelayRequest:
                case OpeSetMuxingDelayRequest:
                case OpeSetCommentPaddingRequest:
                case OpeSetSerialNoRequest:
                case OpeSetHeaderGainRequest: return true;
                default: return false;
            }
        }

        // EncData of opusenc.c and the output file
        struct EncData {
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

        bool WriteCallback(void* user, const std::uint8_t* data, std::int32_t length)
        {
            auto* d = static_cast<EncData*>(user);
            d->bytesWritten += length;
            ++d->pagesOut;
            if (d->io != nullptr)
                return d->io->Write(data, length);
            unsigned long written = 0; // DWORD of WriteFile
            return WriteFile(d->out, data, static_cast<unsigned long>(length), &written, nullptr) && written == static_cast<unsigned long>(length);
        }

        bool CloseCallback(void* user)
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

        void PacketCallback(void* user, const std::uint8_t* packet, std::int32_t length)
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

        // getopt_long of opus-tools (GNU getopt, permuting): one parsed option
        struct ParsedOption {
            std::int32_t index    = -1; // into LongOptions, -1 for short options
            char         shortOpt = 0;
            const char*  argument = nullptr;
        };

        class OptionParser
        {
        public:
            OptionParser(int argc, char** argv) noexcept : argc_(argc), argv_(argv) {}

            /// <summary>Next option: 1 = an option in parsed, 0 = done, -1 = error (message printed).</summary>
            std::int32_t Next(ParsedOption& parsed) noexcept
            {
                parsed = ParsedOption{};
                if (shortPos_ != nullptr && *shortPos_ != 0)
                    return ShortOption(parsed);
                shortPos_ = nullptr;
                while (index_ < argc_) {
                    char* arg = argv_[index_];
                    if (!optionsEnded_ && std::strcmp(arg, "--") == 0) {
                        optionsEnded_ = true;
                        ++index_;
                        continue;
                    }
                    if (optionsEnded_ || arg[0] != '-' || arg[1] == 0) {
                        if (positionalCount_ < 64)
                            positional_[positionalCount_] = index_;
                        ++positionalCount_;
                        ++index_;
                        if (requireOrder_)
                            optionsEnded_ = true;
                        continue;
                    }
                    if (arg[1] == '-')
                        return LongOptionAt(parsed);
                    shortPos_ = arg + 1;
                    return ShortOption(parsed);
                }
                return 0;
            }

            std::int32_t PositionalCount() const noexcept { return positionalCount_; }
            std::int32_t Positional(std::int32_t i) const noexcept { return positional_[i]; }
            void         SetRequireOrder(bool value) noexcept { requireOrder_ = value; }

        private:
            std::int32_t LongOptionAt(ParsedOption& parsed) noexcept
            {
                char*       arg     = argv_[index_];
                const char* name    = arg + 2;
                const char* nameEnd = name;
                while (*nameEnd != 0 && *nameEnd != '=')
                    ++nameEnd;
                const auto   length = static_cast<std::size_t>(nameEnd - name);
                std::int32_t found  = -1;
                bool         exact  = false;
                bool         ambig  = false;
                for (std::int32_t i = 0; i < static_cast<std::int32_t>(sizeof(LongOptions) / sizeof(LongOptions[0])); ++i) {
                    if (std::strncmp(LongOptions[i].name, name, length) == 0) {
                        if (length == std::strlen(LongOptions[i].name)) {
                            found = i;
                            exact = true;
                            break;
                        }
                        if (found < 0)
                            found = i;
                        else
                            ambig = true;
                    }
                }
                if (ambig && !exact) {
                    ErrorPrintf("%s: option `%s' is ambiguous\n", argv_[0], arg);
                    ++index_;
                    return -1;
                }
                if (found < 0) {
                    ErrorPrintf("%s: unrecognized option `--%s'\n", argv_[0], name);
                    ++index_;
                    return -1;
                }
                ++index_;
                parsed.index = found;
                if (*nameEnd != 0) {
                    if (!LongOptions[found].hasArgument) {
                        ErrorPrintf("%s: option `--%s' doesn't allow an argument\n", argv_[0], LongOptions[found].name);
                        return -1;
                    }
                    parsed.argument = nameEnd + 1;
                } else if (LongOptions[found].hasArgument) {
                    if (index_ < argc_) {
                        parsed.argument = argv_[index_++];
                    } else {
                        ErrorPrintf("%s: option `%s' requires an argument\n", argv_[0], arg);
                        return -1;
                    }
                }
                return 1;
            }

            std::int32_t ShortOption(ParsedOption& parsed) noexcept
            {
                const char c = *shortPos_++;
                if (*shortPos_ == 0) {
                    ++index_;
                    shortPos_ = nullptr;
                }
                if (c != 'h' && c != 'V') {
                    ErrorPrintf("%s: invalid option -- %c\n", argv_[0], c);
                    return -1;
                }
                parsed.shortOpt = c;
                return 1;
            }

            int          argc_;
            char**       argv_;
            std::int32_t index_           = 1;
            const char*  shortPos_        = nullptr;
            bool         optionsEnded_    = false;
            bool         requireOrder_    = false;
            std::int32_t positional_[64]  = {};
            std::int32_t positionalCount_ = 0;
        };

        // the spinner of opusenc's main loop
        struct Progress {
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

        void ShowProgress(Progress& p)
        {
            const std::time_t stopTime = std::time(nullptr);
            if (stopTime <= p.lastSpin)
                return;
            const EncData&        data         = *p.data;
            double                estBitrate   = 0;
            const double          codedSeconds = data.nbEncoded / 48000.;
            const double          wallTime     = static_cast<double>(stopTime - p.startTime);
            char                  sbuf[55];
            static constexpr char Spinner[] = "|/-\\";
            if (p.withHardCbr) {
                estBitrate = data.lastLength * (8 * 48000. / p.frameSize);
            } else if (data.nbEncoded <= 0) {
                estBitrate = 0;
            } else {
                const double tweight = 1. / (1 + std::exp(-((codedSeconds / 10.) - 3.)));
                estBitrate           = (data.totalBytes * 8.0 / codedSeconds) * tweight + p.bitrate * (1. - tweight);
            }
            ErrorPrintf("\r");
            for (std::int32_t i = 0; i < p.lastSpinLen; ++i)
                ErrorPrintf(" ");
            if (p.totalSamplesPerChannel > 0 && data.nbEncoded < p.totalSamplesPerChannel + p.lookahead)
                std::snprintf(sbuf, 54, "\r[%c] %2d%% ", Spinner[p.lastSpin & 3],
                              static_cast<int>(std::floor(data.nbEncoded / static_cast<double>(p.totalSamplesPerChannel + p.lookahead) * 100.)));
            else
                std::snprintf(sbuf, 54, "\r[%c] ", Spinner[p.lastSpin & 3]);
            p.lastSpinLen = static_cast<std::int32_t>(std::strlen(sbuf));
            std::snprintf(sbuf + p.lastSpinLen, static_cast<std::size_t>(54 - p.lastSpinLen), "%02d:%02d:%02d.%02d %4.3gx realtime, %5.4g kbit/s",
                          static_cast<int>(codedSeconds / 3600), static_cast<int>(codedSeconds / 60) % 60, static_cast<int>(codedSeconds) % 60,
                          static_cast<int>(codedSeconds * 100) % 100, codedSeconds / (wallTime > 0 ? wallTime : 1e-6), estBitrate / 1000.);
            ErrorPrintf("%s", sbuf);
            p.lastSpinLen = static_cast<std::int32_t>(std::strlen(sbuf));
            p.lastSpin    = stopTime;
        }

        void ShowProgressCallback(void* user)
        {
            ShowProgress(*static_cast<Progress*>(user));
        }

        // The encoder's task graph on a compute scheduler of its own (the I/O scheduler already runs FileRead and
        // FileWrite). The parallel graph (EncodeGraph) allocates its rings and stores in Initialize; if that fails, and
        // for --sequential, the sequential graph encodes on one thread. Both write the same output.
        std::int32_t RunEncodeGraph(WavInput& wav, OggOpusEncoder& encoder, const EncodeGraph::Settings& settings, bool sequential, float* buffer,
                                    std::int32_t frameSize, SchedulerHandle& io)
        {
            if (!sequential) {
                EncodeGraph graph(wav, encoder, settings);
                if (graph.Initialize()) {
                    SchedulerHandle compute = SchedulerHandle::Create(SchedulerKind::Default, graph.Threads());
                    if (!compute)
                        return OpeAllocFail;
                    compute->Start();
                    const std::int32_t ret = graph.Run(io, compute);
                    compute->Stop();
                    return ret;
                }
            }
            SequentialGraph graph(wav, encoder, buffer, frameSize, settings.progress);
            SchedulerHandle compute = SchedulerHandle::Create(SchedulerKind::Default, 1);
            if (!compute)
                return OpeAllocFail;
            compute->Start();
            const std::int32_t ret = graph.Run(compute);
            compute->Stop();
            return ret;
        }

    } // namespace

    int RunFrontend(int argc, char** argvUtf8, wchar_t** argvWide)
    {
        // settings (opusenc.c main)
        bool         quiet          = false;
        std::int32_t bitrate        = -1;
        std::int32_t rate           = 48000;
        std::int32_t frameSize      = 960;
        std::int32_t opusFrameParam = OPUS_FRAMESIZE_20_MS;
        std::int32_t chan           = 2;
        bool         withHardCbr    = false;
        bool         withCvbr       = false;
        std::int32_t signalType     = OPUS_AUTO;
        std::int32_t expectLoss     = 0;
        std::int32_t complexity     = 10;
        std::int32_t downmix        = 0;
        bool         noPhaseInv     = false;
        std::int32_t maxOggDelay    = 48000;
        std::int32_t commentPadding = 512;
        bool         ignoreLength   = false;
        bool         rawMode        = false;
        std::int32_t threads        = 0;
        bool         sequential     = false;
        std::int32_t optCtls        = 0;
        std::int32_t optCtlValues[3 * 64];
        std::int32_t serialNo;

        const std::time_t startTime = std::time(nullptr);
        std::srand(((static_cast<unsigned>(_getpid()) & 65535) << 15) ^ static_cast<unsigned>(startTime));
        serialNo = std::rand();

        const char* opusVersion = LibopusVersion;
        char        vendor[1024];
        std::snprintf(vendor, sizeof(vendor), "%s, %s", opusVersion, LibopusencVersion);
        OpusTags comments;
        if (!comments.Initialize(vendor))
            Fatal("Error: failed to allocate memory for comments\n");
        // the vendor string names the library; the ENCODER comment names the tool
        char encoderString[EncoderStringSize];
        std::snprintf(encoderString, sizeof(encoderString), "opusenc from %s %s", PackageName, PackageVersion);
        if (!comments.Add("ENCODER", encoderString))
            Fatal("Error: failed to add ENCODER comment: %s\n", OpeErrorString(OpeAllocFail));

        OptionParser parser(argc, argvUtf8);
        if (std::getenv("POSIXLY_CORRECT") != nullptr)
            parser.SetRequireOrder(true);
        std::int32_t cline = 0;
        for (;;) {
            ParsedOption       opt;
            const std::int32_t r = parser.Next(opt);
            if (r == 0)
                break;
            if (r < 0) {
                Usage();
                std::exit(1);
            }
            if (opt.shortOpt == 'h') {
                Usage();
                std::exit(0);
            }
            if (opt.shortOpt == 'V') {
                Version(opusVersion);
                std::exit(0);
            }
            const char* optname = LongOptions[opt.index].name;
            const char* optarg  = opt.argument;
            bool        saveCmd = true;
            if (std::strcmp(optname, "quiet") == 0) {
                quiet   = true;
                saveCmd = false;
            } else if (std::strcmp(optname, "bitrate") == 0) {
                bitrate = static_cast<std::int32_t>(std::atof(optarg) * 1000.);
            } else if (std::strcmp(optname, "hard-cbr") == 0) {
                withHardCbr = true;
                withCvbr    = false;
            } else if (std::strcmp(optname, "cvbr") == 0) {
                withCvbr    = true;
                withHardCbr = false;
            } else if (std::strcmp(optname, "vbr") == 0) {
                withCvbr    = false;
                withHardCbr = false;
            } else if (std::strcmp(optname, "help") == 0) {
                Usage();
                std::exit(0);
            } else if (std::strcmp(optname, "help-picture") == 0) {
                Print(Stream::Output, "  Pictures (--picture) are not supported by ctOpus.\n");
                std::exit(0);
            } else if (std::strcmp(optname, "version") == 0 || std::strcmp(optname, "version-short") == 0) {
                Version(opusVersion);
                std::exit(0);
            } else if (std::strcmp(optname, "ignorelength") == 0) {
                ignoreLength = true;
                saveCmd      = false;
            } else if (std::strcmp(optname, "raw") == 0 || std::strcmp(optname, "raw-bits") == 0 || std::strcmp(optname, "raw-rate") == 0
                       || std::strcmp(optname, "raw-chan") == 0 || std::strcmp(optname, "raw-endianness") == 0) {
                if (std::strcmp(optname, "raw-bits") == 0) {
                    const std::int32_t bits = std::atoi(optarg);
                    if (bits != 8 && bits != 16 && bits != 24)
                        Fatal("Invalid bit-depth: %s\n"
                              "--raw-bits must be one of 8, 16, or 24\n",
                              optarg);
                }
                rawMode = true;
                saveCmd = false;
            } else if (std::strcmp(optname, "downmix-mono") == 0) {
                downmix = 1;
            } else if (std::strcmp(optname, "downmix-stereo") == 0) {
                downmix = 2;
            } else if (std::strcmp(optname, "no-downmix") == 0) {
                downmix = -1;
            } else if (std::strcmp(optname, "no-phase-inv") == 0) {
                noPhaseInv = true;
            } else if (std::strcmp(optname, "music") == 0) {
                signalType = OPUS_SIGNAL_MUSIC;
            } else if (std::strcmp(optname, "speech") == 0) {
                signalType = OPUS_SIGNAL_VOICE;
            } else if (std::strcmp(optname, "expect-loss") == 0) {
                expectLoss = std::atoi(optarg);
                if (expectLoss > 100 || expectLoss < 0)
                    Fatal("Invalid expect-loss: %s\n"
                          "Expected loss is a percentage in the range 0 to 100.\n",
                          optarg);
            } else if (std::strcmp(optname, "comp") == 0 || std::strcmp(optname, "complexity") == 0) {
                complexity = std::atoi(optarg);
                if (complexity > 10 || complexity < 0)
                    Fatal("Invalid complexity: %s\n"
                          "Complexity must be in the range 0 to 10.\n",
                          optarg);
            } else if (std::strcmp(optname, "framesize") == 0) {
                if (std::strcmp(optarg, "2.5") == 0)
                    opusFrameParam = OPUS_FRAMESIZE_2_5_MS;
                else if (std::strcmp(optarg, "5") == 0)
                    opusFrameParam = OPUS_FRAMESIZE_5_MS;
                else if (std::strcmp(optarg, "10") == 0)
                    opusFrameParam = OPUS_FRAMESIZE_10_MS;
                else if (std::strcmp(optarg, "20") == 0)
                    opusFrameParam = OPUS_FRAMESIZE_20_MS;
                else if (std::strcmp(optarg, "40") == 0)
                    opusFrameParam = OPUS_FRAMESIZE_40_MS;
                else if (std::strcmp(optarg, "60") == 0)
                    opusFrameParam = OPUS_FRAMESIZE_60_MS;
                else
                    Fatal("Invalid framesize: %s\n"
                          "Value is in milliseconds and must be 2.5, 5, 10, 20, 40, or 60.\n",
                          optarg);
                frameSize = opusFrameParam <= OPUS_FRAMESIZE_40_MS ? 120 << (opusFrameParam - OPUS_FRAMESIZE_2_5_MS)
                                                                   : (opusFrameParam - OPUS_FRAMESIZE_20_MS + 1) * 960;
            } else if (std::strcmp(optname, "max-delay") == 0) {
                const double val = std::atof(optarg);
                if (val < 0. || val > 1000.)
                    Fatal("Invalid max-delay: %s\n"
                          "Value is in milliseconds and must be in the range 0 to 1000.\n",
                          optarg);
                maxOggDelay = static_cast<std::int32_t>(std::floor(val * 48.));
            } else if (std::strcmp(optname, "serial") == 0) {
                serialNo = std::atoi(optarg);
            } else if (std::strcmp(optname, "set-ctl-int") == 0) {
                const std::size_t len  = std::strlen(optarg);
                const char*       spos = std::strchr(optarg, '=');
                if (len < 3 || spos == nullptr || (spos - optarg) < 1 || static_cast<std::size_t>(spos - optarg) >= len)
                    Fatal("Invalid set-ctl-int: %s\n"
                          "Syntax is --set-ctl-int intX=intY\n"
                          "       or --set-ctl-int intS:intX=intY\n",
                          optarg);
                const char*  tpos   = std::strchr(optarg, ':');
                std::int32_t target = -1;
                if (tpos == nullptr)
                    tpos = optarg - 1;
                else
                    target = std::atoi(optarg);
                const std::int32_t request = std::atoi(tpos + 1);
                if (!IsValidCtl(request))
                    Fatal("Invalid set-ctl-int: %s\n", optarg);
                if (optCtls >= 64)
                    Fatal("Error: failed to allocate memory for ctls\n");
                optCtlValues[optCtls * 3]     = target;
                optCtlValues[optCtls * 3 + 1] = request;
                optCtlValues[optCtls * 3 + 2] = std::atoi(spos + 1);
                ++optCtls;
            } else if (std::strcmp(optname, "save-range") == 0) {
                Fatal("Error: --save-range is not supported by ctOpus\n");
            } else if (std::strcmp(optname, "comment") == 0) {
                saveCmd = false;
                if (std::strchr(optarg, '=') == nullptr)
                    Fatal("Invalid comment: %s\n"
                          "Comments must be of the form name=value\n",
                          optarg);
                if (!comments.AddString(optarg))
                    Fatal("Error: failed to add comment: %s\n", OpeErrorString(OpeAllocFail));
            } else if (std::strcmp(optname, "artist") == 0 || std::strcmp(optname, "title") == 0 || std::strcmp(optname, "album") == 0
                       || std::strcmp(optname, "tracknumber") == 0 || std::strcmp(optname, "date") == 0 || std::strcmp(optname, "genre") == 0) {
                saveCmd = false;
                if (!comments.Add(optname, optarg))
                    Fatal("Error: failed to add %s comment: %s\n", optname, OpeErrorString(OpeAllocFail));
            } else if (std::strcmp(optname, "picture") == 0) {
                Fatal("Error: --picture is not supported by ctOpus\n");
            } else if (std::strcmp(optname, "padding") == 0) {
                commentPadding = std::atoi(optarg);
            } else if (std::strcmp(optname, "discard-comments") == 0 || std::strcmp(optname, "discard-pictures") == 0) {
                // only for FLAC/Ogg input, which ctOpus does not read
            } else if (std::strcmp(optname, "threads") == 0) {
                threads = std::atoi(optarg);
                if (threads < 1 || threads > 1024)
                    Fatal("Invalid threads: %s\n"
                          "The number of worker threads must be in the range 1 to 1024.\n",
                          optarg);
                saveCmd = false; // ctOpus options: no effect on the output, not recorded
            } else if (std::strcmp(optname, "sequential") == 0) {
                sequential = true;
                saveCmd    = false;
            }
            // options leaking file paths, metadata or console behaviour are not saved in ENCODER_OPTIONS
            if (saveCmd && cline < EncoderStringSize) {
                std::int32_t ret
                    = std::snprintf(encoderString + cline, static_cast<std::size_t>(EncoderStringSize - cline), "%s--%s", cline == 0 ? "" : " ", optname);
                if (ret < 0 || ret >= EncoderStringSize - cline) {
                    cline = EncoderStringSize;
                } else {
                    cline += ret;
                    if (optarg != nullptr) {
                        ret = std::snprintf(encoderString + cline, static_cast<std::size_t>(EncoderStringSize - cline), " %s", optarg);
                        if (ret < 0 || ret >= EncoderStringSize - cline)
                            cline = EncoderStringSize;
                        else
                            cline += ret;
                    }
                }
            }
        }
        if (parser.PositionalCount() != 2) {
            Usage();
            std::exit(1);
        }
        const std::int32_t inIndex  = parser.Positional(0);
        const std::int32_t outIndex = parser.Positional(1);
        const char*        inFile   = argvUtf8[inIndex];
        const char*        outFile  = argvUtf8[outIndex];

        if (cline > 0 && !comments.Add("ENCODER_OPTIONS", encoderString))
            Fatal("Error: failed to add ENCODER_OPTIONS comment: %s\n", OpeErrorString(OpeAllocFail));

        // The setup of ThinkMeta.ConcurrentTasks, the same in every sample:
        //   1. ConvertThreadToTask: this thread becomes a task, so it can wait for the graph (TaskGroup::Wait)
        //      and for the pipes of FileIo.
        //   2. EnableAsyncIo: the core's completion port, for the overlapped I/O of FileRead and FileWrite.
        //   3. An I/O scheduler (one thread) for FileRead and FileWrite here; the compute scheduler follows in
        //      RunEncodeGraph, once the graph knows its thread count.
        //   4. Start, run the graph, stop once every task has exited; then undo 2 and 1 (below).
        ConvertThreadToTask();
        if (!AsyncIo::EnableAsyncIo())
            Fatal("Error: failed to start asynchronous I/O\n");
        SchedulerHandle ioScheduler = SchedulerHandle::Create(SchedulerKind::Default, 1);
        if (!ioScheduler)
            Fatal("Error: failed to create the I/O scheduler\n");
        ioScheduler->Start();
        FileIo    fileIo;
        InputFile fin;
        if (!fin.Open(argvWide[inIndex])) {
            PrintOpenError(inFile, GetLastError());
            std::exit(1);
        }
        WavInput wav;
        if (rawMode || !wav.Open(fin, ignoreLength, ReportToStderr))
            Fatal("Error: unsupported input file: %s\n", inFile);

        WavInput::Info info = wav.GetInfo();
        if (info.rate < 100 || info.rate > 768000)
            Fatal("Error: unsupported sample rate in input file: %ld Hz\n", static_cast<long>(info.rate));
        if (info.channels > 255 || info.channels < 1)
            Fatal("Error: unsupported channel count in input file: %d\n"
                  "Channel count must be in the range 1 to 255.\n",
                  info.channels);

        if (downmix == 0 && info.channels > 2 && bitrate > 0 && bitrate < (16000 * info.channels)) {
            if (!quiet)
                ErrorPrintf("Notice: Surround bitrate less than 16 kbit/s per channel, downmixing.\n");
            downmix = info.channels > 8 ? 1 : 2;
        }
        if (downmix > 0 && downmix < info.channels)
            downmix = wav.SetupDownmix(downmix, ReportToStderr);
        else
            downmix = 0;
        info = wav.GetInfo();

        rate                                = info.rate;
        chan                                = info.channels;
        std::int64_t totalSamplesPerChannel = info.totalSamplesPerChannel;
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
        std::int32_t ret = encoder.Create(callbacks, comments, rate, chan, chan > 8 ? 255 : chan > 2 ? 1 : 0);
        if (ret != OpeOk)
            Fatal("Error: failed to create encoder: %s\n", OpeErrorString(ret));
        data.enc = &encoder;

        ret = encoder.SetExpertFrameDuration(opusFrameParam);
        if (ret != OpeOk)
            Fatal("Error: OPUS_SET_EXPERT_FRAME_DURATION failed: %s\n", OpeErrorString(ret));
        ret = encoder.Ctl(OpeSetMuxingDelayRequest, maxOggDelay);
        if (ret != OpeOk)
            Fatal("Error: OPE_SET_MUXING_DELAY failed: %s\n", OpeErrorString(ret));
        ret = encoder.Ctl(OpeSetSerialNoRequest, serialNo);
        if (ret != OpeOk)
            Fatal("Error: OPE_SET_SERIALNO failed: %s\n", OpeErrorString(ret));
        ret = encoder.Ctl(OpeSetHeaderGainRequest, 0);
        if (ret != OpeOk)
            Fatal("Error: OPE_SET_HEADER_GAIN failed: %s\n", OpeErrorString(ret));
        ret = encoder.Ctl(OpeSetCommentPaddingRequest, commentPadding);
        if (ret != OpeOk)
            Fatal("Error: OPE_SET_COMMENT_PADDING failed: %s\n", OpeErrorString(ret));

        data.nbStreams = encoder.NbStreams();
        data.nbCoupled = encoder.NbCoupledStreams();

        if (bitrate < 0) {
            // lower default rate for sampling rates [8000-44100) by a factor of (rate+16k)/(64k)
            const std::int32_t r = rate < 44100 ? rate : 48000;
            std::int32_t       k = (r + 1000) / 1000;
            k                    = k < 8 ? 8 : k;
            k                    = k > 48 ? 48 : k;
            bitrate              = ((64000 * data.nbStreams + 32000 * data.nbCoupled) * (k + 16) + 32) >> 6;
        }
        if (bitrate > (1024000 * chan) || bitrate < 500)
            Fatal("Error: bitrate %d bits/sec is insane\n%s"
                  "--bitrate values from 6 to 256 kbit/s per channel are meaningful.\n",
                  bitrate, bitrate >= 1000000 ? "Did you mistake bits for kilobits?\n" : "");
        bitrate = chan * 256000 < bitrate ? chan * 256000 : bitrate;

        ret = encoder.Ctl(OPUS_SET_BITRATE_REQUEST, bitrate);
        if (ret != OpeOk)
            Fatal("Error: OPUS_SET_BITRATE %d failed: %s\n", bitrate, OpeErrorString(ret));
        ret = encoder.Ctl(OPUS_SET_VBR_REQUEST, !withHardCbr);
        if (ret != OpeOk)
            Fatal("Error: OPUS_SET_VBR %d failed: %s\n", !withHardCbr, OpeErrorString(ret));
        if (!withHardCbr) {
            ret = encoder.Ctl(OPUS_SET_VBR_CONSTRAINT_REQUEST, withCvbr);
            if (ret != OpeOk)
                Fatal("Error: OPUS_SET_VBR_CONSTRAINT %d failed: %s\n", withCvbr, OpeErrorString(ret));
        }
        ret = encoder.Ctl(OPUS_SET_SIGNAL_REQUEST, signalType);
        if (ret != OpeOk)
            Fatal("Error: OPUS_SET_SIGNAL failed: %s\n", OpeErrorString(ret));
        ret = encoder.Ctl(OPUS_SET_COMPLEXITY_REQUEST, complexity);
        if (ret != OpeOk)
            Fatal("Error: OPUS_SET_COMPLEXITY %d failed: %s\n", complexity, OpeErrorString(ret));
        ret = encoder.Ctl(OPUS_SET_PACKET_LOSS_PERC_REQUEST, expectLoss);
        if (ret != OpeOk)
            Fatal("Error: OPUS_SET_PACKET_LOSS_PERC %d failed: %s\n", expectLoss, OpeErrorString(ret));
        {
            std::int32_t lsb = info.sampleBits < 24 ? info.sampleBits : 24;
            lsb              = lsb > 8 ? lsb : 8;
            ret              = encoder.Ctl(OPUS_SET_LSB_DEPTH_REQUEST, lsb);
            if (ret != OpeOk)
                ErrorPrintf("Warning: OPUS_SET_LSB_DEPTH failed: %s\n", OpeErrorString(ret));
        }
        if (noPhaseInv) {
            ret = encoder.Ctl(OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST, 1);
            if (ret != OpeOk)
                ErrorPrintf("Warning: OPUS_SET_PHASE_INVERSION_DISABLED failed: %s\n", OpeErrorString(ret));
        }

        // the last SET ctls, so they can override the defaults
        for (std::int32_t i = 0; i < optCtls; ++i) {
            const std::int32_t target = optCtlValues[i * 3];
            if (target == -1) {
                ret = encoder.Ctl(optCtlValues[i * 3 + 1], optCtlValues[i * 3 + 2]);
                if (ret != OpeOk)
                    Fatal("Error: failed to set encoder ctl %d=%d: %s\n", optCtlValues[i * 3 + 1], optCtlValues[i * 3 + 2], OpeErrorString(ret));
            } else if (target < data.nbStreams) {
                ret = encoder.StreamCtl(target, optCtlValues[i * 3 + 1], optCtlValues[i * 3 + 2]);
                if (ret != OPUS_OK)
                    Fatal("Error: failed to set stream %d encoder ctl %d=%d: %s\n", target, optCtlValues[i * 3 + 1], optCtlValues[i * 3 + 2],
                          OpusStrError(ret));
            } else {
                Fatal("Error: --set-ctl-int stream %d is higher than the highest stream number %d\n", target, data.nbStreams - 1);
            }
        }

        // the lookahead late, so user ctls can change it
        std::int32_t lookahead = 0;
        ret                    = encoder.Get(OPUS_GET_LOOKAHEAD_REQUEST, lookahead);
        if (ret != OpeOk)
            Fatal("Error: OPUS_GET_LOOKAHEAD failed: %s\n", OpeErrorString(ret));

        if (!quiet) {
            std::int32_t opusApp = 0;
            ErrorPrintf("Encoding using %s", opusVersion);
            ret = encoder.Get(OPUS_GET_APPLICATION_REQUEST, opusApp);
            if (ret != OpeOk)
                ErrorPrintf("\n");
            else if (opusApp == OPUS_APPLICATION_VOIP)
                ErrorPrintf(" (VoIP)\n");
            else if (opusApp == OPUS_APPLICATION_AUDIO)
                ErrorPrintf(" (audio)\n");
            else if (opusApp == OPUS_APPLICATION_RESTRICTED_LOWDELAY)
                ErrorPrintf(" (low-delay)\n");
            else
                ErrorPrintf(" (unknown application)\n");
            ErrorPrintf("-----------------------------------------------------\n");
            ErrorPrintf("   Input: %0.6g kHz, %d channel%s\n", rate / 1000., chan, chan < 2 ? "" : "s");
            ErrorPrintf("  Output: %d channel%s (", chan, chan < 2 ? "" : "s");
            if (data.nbCoupled > 0)
                ErrorPrintf("%d coupled", data.nbCoupled * 2);
            if (data.nbStreams - data.nbCoupled > 0)
                ErrorPrintf("%s%d uncoupled", data.nbCoupled > 0 ? ", " : "", data.nbStreams - data.nbCoupled);
            ErrorPrintf(")\n          %0.2gms packets, %0.6g kbit/s%s\n", frameSize / (48000 / 1000.), bitrate / 1000.,
                        withHardCbr    ? " CBR"
                            : withCvbr ? " CVBR"
                                       : " VBR");
            ErrorPrintf(" Preskip: %d\n", lookahead);
            ErrorPrintf("\n");
        }

        if (std::strcmp(outFile, "-") == 0) {
            data.out         = GetStdHandle(STD_OUTPUT_HANDLE);
            data.outIsStdout = true;
        } else {
            const std::uint32_t error = fileIo.CreateOutput(argvWide[outIndex]);
            if (error != ERROR_SUCCESS) {
                PrintOpenError(outFile, error);
                std::exit(1);
            }
            data.io = &fileIo;
        }

        HeapArray<float> input;
        if (!input.Allocate(static_cast<std::size_t>(frameSize) * chan))
            Fatal("Error: failed to allocate sample buffer\n");

        Progress progress;
        progress.data                   = &data;
        progress.startTime              = startTime;
        progress.totalSamplesPerChannel = totalSamplesPerChannel;
        progress.lookahead              = lookahead;
        progress.frameSize              = frameSize;
        progress.bitrate                = bitrate;
        progress.withHardCbr            = withHardCbr;

        // the task graph of the encoder (EncodeGraph): resampling (at 48 kHz a copy) on worker
        // tasks, the encoder in order (the same output); the sequential graph for --sequential.
        // The graph owns its edges and their events; Run waits for its TaskGroup, so the calling
        // thread waits as a task (ConvertThreadToTask above).
        EncodeProgress encodeProgress;
        if (!quiet) {
            encodeProgress.callback = ShowProgressCallback;
            encodeProgress.user     = &progress;
        }
        EncodeGraph::Settings settings;
        settings.threads    = static_cast<std::uint32_t>(threads);
        settings.lookahead  = lookahead;
        settings.complexity = complexity;
        settings.progress   = encodeProgress;
        // the rest of the input file behind the WAV header through FileRead (not for stdin)
        settings.asyncInput = fileIo.AttachInput(fin, argvWide[inIndex]);
        fileIo.Start(ioScheduler);
        ret                = RunEncodeGraph(wav, encoder, settings, sequential, input.Data(), frameSize, ioScheduler);
        const bool written = fileIo.Finish();
        ioScheduler->Stop();
        AsyncIo::DisableAsyncIo();
        ConvertTaskToThread();
        if (ret == OpeOk && !written)
            ret = OpeCloseFail;
        if (progress.lastSpinLen != 0) {
            ErrorPrintf("\r");
            for (std::int32_t i = 0; i < progress.lastSpinLen; ++i)
                ErrorPrintf(" ");
            ErrorPrintf("\r");
        }

        if (ret != OpeOk)
            Fatal("Encoding aborted: %s\n", OpeErrorString(ret));
        const std::time_t stopTime = std::time(nullptr);

        if (!quiet) {
            const double codedSeconds = data.nbEncoded / 48000.;
            const double wallTime     = static_cast<double>(stopTime - startTime);
            ErrorPrintf("Encoding complete\n");
            ErrorPrintf("-----------------------------------------------------\n");
            ErrorPrintf("       Encoded:");
            PrintTime(codedSeconds);
            ErrorPrintf("\n       Runtime:");
            PrintTime(wallTime);
            ErrorPrintf("\n");
            if (wallTime > 0)
                ErrorPrintf("                (%0.4gx realtime)\n", codedSeconds / wallTime);
            ErrorPrintf("         Wrote: %lld bytes, %lld packets, %lld pages\n", data.bytesWritten, data.packetsOut, data.pagesOut);
            if (data.nbEncoded > 0) {
                ErrorPrintf("       Bitrate: %0.6g kbit/s (without overhead)\n", data.totalBytes * 8.0 / (codedSeconds) / 1000.0);
                ErrorPrintf(" Instant rates: %0.6g to %0.6g kbit/s\n"
                            "                (%d to %d bytes per packet)\n",
                            data.minBytes * (8 * 48000. / frameSize / 1000.), data.peakBytes * (8 * 48000. / frameSize / 1000.), data.minBytes, data.peakBytes);
            }
            if (data.bytesWritten > 0)
                ErrorPrintf("      Overhead: %0.3g%% (container+metadata)\n",
                            (data.bytesWritten - data.totalBytes) / static_cast<double>(data.bytesWritten) * 100.);
            ErrorPrintf("\n");
        }
        (void)downmix;
        return 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
