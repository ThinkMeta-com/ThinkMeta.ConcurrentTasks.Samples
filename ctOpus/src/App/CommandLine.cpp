// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "CommandLine.h"

#include <Windows.h>

#include <process.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "Console.h"
#include "Formatter.h"
#include "OggOpusEncoder.h"
#include "OpusDefines.h"
#include "OpusHeaders.h"
#include "OpusPacket.h"
#include "VersionInfo.h"
#include "ctOpusVersion.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    using Shared::Console;
    using Shared::Formatter;
    using Shared::Stream;

    // opusenc's long_options, then the ctOpus options
    const CommandLine::LongOption CommandLine::LongOptions[] = {{"quiet", false},
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
                                                                {"sequential", false},
                                                                {"stats", false}};

    // getopt_long of opus-tools (GNU getopt, permuting): one parsed option
    struct CommandLine::ParsedOption {
        std::int32_t index    = -1; // into LongOptions, -1 for short options
        char         shortOpt = 0;
        const char*  argument = nullptr;
    };

    class CommandLine::OptionParser
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
                Console::Print(Stream::Error, L"%hs: option `%hs' is ambiguous\n", argv_[0], arg);
                ++index_;
                return -1;
            }
            if (found < 0) {
                Console::Print(Stream::Error, L"%hs: unrecognized option `--%hs'\n", argv_[0], name);
                ++index_;
                return -1;
            }
            ++index_;
            parsed.index = found;
            if (*nameEnd != 0) {
                if (!LongOptions[found].hasArgument) {
                    Console::Print(Stream::Error, L"%hs: option `--%hs' doesn't allow an argument\n", argv_[0], LongOptions[found].name);
                    return -1;
                }
                parsed.argument = nameEnd + 1;
            } else if (LongOptions[found].hasArgument) {
                if (index_ < argc_) {
                    parsed.argument = argv_[index_++];
                } else {
                    Console::Print(Stream::Error, L"%hs: option `%hs' requires an argument\n", argv_[0], arg);
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
                Console::Print(Stream::Error, L"%hs: invalid option -- %c\n", argv_[0], c);
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

    Shared::ParseResult CommandLine::Parse(int argc, wchar_t** argv, Options& options)
    {
        // the arguments as UTF-8, like opusenc's init_commandline_arguments_utf8 (WIN_UNICODE)
        std::size_t total = 0;
        for (int i = 0; i < argc; ++i)
            total += static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr));
        if (!options.argvUtf8.Allocate(static_cast<std::size_t>(argc) + 1) || !options.argvStorage.Allocate(total + 1)) {
            return Shared::ParseResult::Error;
        }
        std::size_t pos = 0;
        for (int i = 0; i < argc; ++i) {
            options.argvUtf8[i] = options.argvStorage.Data() + pos;
            pos += static_cast<std::size_t>(
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, options.argvStorage.Data() + pos, static_cast<int>(total - pos), nullptr, nullptr));
        }
        options.argvWide = argv;

        options.startTime = std::time(nullptr);
        std::srand(((static_cast<unsigned>(_getpid()) & 65535) << 15) ^ static_cast<unsigned>(options.startTime));
        options.serialNo = std::rand();

        options.opusVersion = LibopusVersion;
        char vendor[1024];
        Formatter::Format(vendor, static_cast<std::int32_t>(sizeof(vendor)), "%s, %s", options.opusVersion, LibopusencVersion);
        if (!options.comments.Initialize(vendor))
            Console::Fatal(L"Error: failed to allocate memory for comments\n");
        // the vendor string names the library; the ENCODER comment names the tool
        char encoderString[EncoderStringSize];
        Formatter::Format(encoderString, EncoderStringSize, "opusenc from %s %s", PackageName, PackageVersion);
        if (!options.comments.Add("ENCODER", encoderString))
            Console::Fatal(L"Error: failed to add ENCODER comment: %hs\n", OpeErrorString(OpeAllocFail));

        OptionParser parser(argc, options.argvUtf8.Data());
        if (std::getenv("POSIXLY_CORRECT") != nullptr)
            parser.SetRequireOrder(true);
        std::int32_t cline = 0;
        for (;;) {
            ParsedOption       opt;
            const std::int32_t r = parser.Next(opt);
            if (r == 0)
                break;
            if (r < 0) {
                PrintUsage();
                return Shared::ParseResult::Error;
            }
            if (opt.shortOpt == 'h') {
                PrintUsage();
                return Shared::ParseResult::Exit;
            }
            if (opt.shortOpt == 'V') {
                PrintVersion(options.opusVersion);
                return Shared::ParseResult::Exit;
            }
            const char* optname = LongOptions[opt.index].name;
            const char* optarg  = opt.argument;
            bool        saveCmd = true;
            if (std::strcmp(optname, "quiet") == 0) {
                options.quiet = true;
                saveCmd       = false;
            } else if (std::strcmp(optname, "bitrate") == 0) {
                options.bitrate = static_cast<std::int32_t>(std::atof(optarg) * 1000.);
            } else if (std::strcmp(optname, "hard-cbr") == 0) {
                options.withHardCbr = true;
                options.withCvbr    = false;
            } else if (std::strcmp(optname, "cvbr") == 0) {
                options.withCvbr    = true;
                options.withHardCbr = false;
            } else if (std::strcmp(optname, "vbr") == 0) {
                options.withCvbr    = false;
                options.withHardCbr = false;
            } else if (std::strcmp(optname, "help") == 0) {
                PrintUsage();
                return Shared::ParseResult::Exit;
            } else if (std::strcmp(optname, "help-picture") == 0) {
                Console::Print(Stream::Output, L"  Pictures (--picture) are not supported by ctOpus.\n");
                return Shared::ParseResult::Exit;
            } else if (std::strcmp(optname, "version") == 0 || std::strcmp(optname, "version-short") == 0) {
                PrintVersion(options.opusVersion);
                return Shared::ParseResult::Exit;
            } else if (std::strcmp(optname, "ignorelength") == 0) {
                options.ignoreLength = true;
                saveCmd              = false;
            } else if (std::strcmp(optname, "raw") == 0 || std::strcmp(optname, "raw-bits") == 0 || std::strcmp(optname, "raw-rate") == 0
                       || std::strcmp(optname, "raw-chan") == 0 || std::strcmp(optname, "raw-endianness") == 0) {
                if (std::strcmp(optname, "raw-bits") == 0) {
                    const std::int32_t bits = std::atoi(optarg);
                    if (bits != 8 && bits != 16 && bits != 24)
                        Console::Fatal(L"Invalid bit-depth: %hs\n"
                                       L"--raw-bits must be one of 8, 16, or 24\n",
                                       optarg);
                }
                options.rawMode = true;
                saveCmd         = false;
            } else if (std::strcmp(optname, "downmix-mono") == 0) {
                options.downmix = 1;
            } else if (std::strcmp(optname, "downmix-stereo") == 0) {
                options.downmix = 2;
            } else if (std::strcmp(optname, "no-downmix") == 0) {
                options.downmix = -1;
            } else if (std::strcmp(optname, "no-phase-inv") == 0) {
                options.noPhaseInv = true;
            } else if (std::strcmp(optname, "music") == 0) {
                options.signalType = OPUS_SIGNAL_MUSIC;
            } else if (std::strcmp(optname, "speech") == 0) {
                options.signalType = OPUS_SIGNAL_VOICE;
            } else if (std::strcmp(optname, "expect-loss") == 0) {
                options.expectLoss = std::atoi(optarg);
                if (options.expectLoss > 100 || options.expectLoss < 0)
                    Console::Fatal(L"Invalid expect-loss: %hs\n"
                                   L"Expected loss is a percentage in the range 0 to 100.\n",
                                   optarg);
            } else if (std::strcmp(optname, "comp") == 0 || std::strcmp(optname, "complexity") == 0) {
                options.complexity = std::atoi(optarg);
                if (options.complexity > 10 || options.complexity < 0)
                    Console::Fatal(L"Invalid complexity: %hs\n"
                                   L"Complexity must be in the range 0 to 10.\n",
                                   optarg);
            } else if (std::strcmp(optname, "framesize") == 0) {
                if (std::strcmp(optarg, "2.5") == 0)
                    options.opusFrameParam = OPUS_FRAMESIZE_2_5_MS;
                else if (std::strcmp(optarg, "5") == 0)
                    options.opusFrameParam = OPUS_FRAMESIZE_5_MS;
                else if (std::strcmp(optarg, "10") == 0)
                    options.opusFrameParam = OPUS_FRAMESIZE_10_MS;
                else if (std::strcmp(optarg, "20") == 0)
                    options.opusFrameParam = OPUS_FRAMESIZE_20_MS;
                else if (std::strcmp(optarg, "40") == 0)
                    options.opusFrameParam = OPUS_FRAMESIZE_40_MS;
                else if (std::strcmp(optarg, "60") == 0)
                    options.opusFrameParam = OPUS_FRAMESIZE_60_MS;
                else
                    Console::Fatal(L"Invalid framesize: %hs\n"
                                   L"Value is in milliseconds and must be 2.5, 5, 10, 20, 40, or 60.\n",
                                   optarg);
                options.frameSize = options.opusFrameParam <= OPUS_FRAMESIZE_40_MS ? 120 << (options.opusFrameParam - OPUS_FRAMESIZE_2_5_MS)
                                                                                   : (options.opusFrameParam - OPUS_FRAMESIZE_20_MS + 1) * 960;
            } else if (std::strcmp(optname, "max-delay") == 0) {
                const double val = std::atof(optarg);
                if (val < 0. || val > 1000.)
                    Console::Fatal(L"Invalid max-delay: %hs\n"
                                   L"Value is in milliseconds and must be in the range 0 to 1000.\n",
                                   optarg);
                options.maxOggDelay = static_cast<std::int32_t>(std::floor(val * 48.));
            } else if (std::strcmp(optname, "serial") == 0) {
                options.serialNo = std::atoi(optarg);
            } else if (std::strcmp(optname, "set-ctl-int") == 0) {
                const std::size_t len  = std::strlen(optarg);
                const char*       spos = std::strchr(optarg, '=');
                if (len < 3 || spos == nullptr || (spos - optarg) < 1 || static_cast<std::size_t>(spos - optarg) >= len)
                    Console::Fatal(L"Invalid set-ctl-int: %hs\n"
                                   L"Syntax is --set-ctl-int intX=intY\n"
                                   L"       or --set-ctl-int intS:intX=intY\n",
                                   optarg);
                const char*  tpos   = std::strchr(optarg, ':');
                std::int32_t target = -1;
                if (tpos == nullptr)
                    tpos = optarg - 1;
                else
                    target = std::atoi(optarg);
                const std::int32_t request = std::atoi(tpos + 1);
                if (!IsValidCtl(request))
                    Console::Fatal(L"Invalid set-ctl-int: %hs\n", optarg);
                if (options.optCtls >= 64)
                    Console::Fatal(L"Error: failed to allocate memory for ctls\n");
                options.optCtlValues[options.optCtls * 3]     = target;
                options.optCtlValues[options.optCtls * 3 + 1] = request;
                options.optCtlValues[options.optCtls * 3 + 2] = std::atoi(spos + 1);
                ++options.optCtls;
            } else if (std::strcmp(optname, "save-range") == 0) {
                Console::Fatal(L"Error: --save-range is not supported by ctOpus\n");
            } else if (std::strcmp(optname, "comment") == 0) {
                saveCmd = false;
                if (std::strchr(optarg, '=') == nullptr)
                    Console::Fatal(L"Invalid comment: %hs\n"
                                   L"Comments must be of the form name=value\n",
                                   optarg);
                if (!options.comments.AddString(optarg))
                    Console::Fatal(L"Error: failed to add comment: %hs\n", OpeErrorString(OpeAllocFail));
            } else if (std::strcmp(optname, "artist") == 0 || std::strcmp(optname, "title") == 0 || std::strcmp(optname, "album") == 0
                       || std::strcmp(optname, "tracknumber") == 0 || std::strcmp(optname, "date") == 0 || std::strcmp(optname, "genre") == 0) {
                saveCmd = false;
                if (!options.comments.Add(optname, optarg))
                    Console::Fatal(L"Error: failed to add %hs comment: %hs\n", optname, OpeErrorString(OpeAllocFail));
            } else if (std::strcmp(optname, "picture") == 0) {
                Console::Fatal(L"Error: --picture is not supported by ctOpus\n");
            } else if (std::strcmp(optname, "padding") == 0) {
                options.commentPadding = std::atoi(optarg);
            } else if (std::strcmp(optname, "discard-comments") == 0 || std::strcmp(optname, "discard-pictures") == 0) {
                // only for FLAC/Ogg input, which ctOpus does not read
            } else if (std::strcmp(optname, "threads") == 0) {
                options.threads = std::atoi(optarg);
                if (options.threads < 1 || options.threads > 1024)
                    Console::Fatal(L"Invalid threads: %hs\n"
                                   L"The number of worker threads must be in the range 1 to 1024.\n",
                                   optarg);
                saveCmd = false; // ctOpus options: no effect on the output, not recorded
            } else if (std::strcmp(optname, "sequential") == 0) {
                options.sequential = true;
                saveCmd            = false;
            } else if (std::strcmp(optname, "stats") == 0) {
                options.stats = true;
                saveCmd       = false;
            }
            // options leaking file paths, metadata or console behaviour are not saved in ENCODER_OPTIONS
            if (saveCmd && cline < EncoderStringSize) {
                std::int32_t ret = Formatter::Format(encoderString + cline, EncoderStringSize - cline, "%s--%s", cline == 0 ? "" : " ", optname);
                if (ret < 0 || ret >= EncoderStringSize - cline) {
                    cline = EncoderStringSize;
                } else {
                    cline += ret;
                    if (optarg != nullptr) {
                        ret = Formatter::Format(encoderString + cline, EncoderStringSize - cline, " %s", optarg);
                        if (ret < 0 || ret >= EncoderStringSize - cline)
                            cline = EncoderStringSize;
                        else
                            cline += ret;
                    }
                }
            }
        }
        if (parser.PositionalCount() != 2) {
            PrintUsage();
            return Shared::ParseResult::Error;
        }
        options.inIndex  = parser.Positional(0);
        options.outIndex = parser.Positional(1);

        if (cline > 0 && !options.comments.Add("ENCODER_OPTIONS", encoderString))
            Console::Fatal(L"Error: failed to add ENCODER_OPTIONS comment: %hs\n", OpeErrorString(OpeAllocFail));
        return Shared::ParseResult::Run;
    }

    void CommandLine::PrintUsage()
    {
        const auto p = [](const wchar_t* text) { Console::Print(Stream::Output, L"%ls", text); };
        p(L"Usage: opusenc [options] input_file output_file.opus\n");
        p(L"\n");
        p(L"Encode audio using Opus.\n");
        p(L"The input format can be Wave.\n");
        p(L"\ninput_file can be:\n");
        p(L"  filename.wav      file\n");
        p(L"  -                 stdin\n");
        p(L"\noutput_file can be:\n");
        p(L"  filename.opus     compressed file\n");
        p(L"  -                 stdout\n");
        p(L"\nGeneral options:\n");
        p(L" -h, --help         Show this help\n");
        p(L" -V, --version      Show version information\n");
        p(L" --help-picture     Show help on attaching album art\n");
        p(L" --quiet            Enable quiet mode\n");
        p(L"\nEncoding options:\n");
        p(L" --bitrate n.nnn    Set target bitrate in kbit/s (6-256/channel)\n");
        p(L" --vbr              Use variable bitrate encoding (default)\n");
        p(L" --cvbr             Use constrained variable bitrate encoding\n");
        p(L" --hard-cbr         Use hard constant bitrate encoding\n");
        p(L" --music            Tune low bitrates for music (override automatic detection)\n");
        p(L" --speech           Tune low bitrates for speech (override automatic detection)\n");
        p(L" --comp n           Set encoding complexity (0-10, default: 10 (slowest))\n");
        p(L" --framesize n      Set maximum frame size in milliseconds\n");
        p(L"                      (2.5, 5, 10, 20, 40, 60, default: 20)\n");
        p(L" --expect-loss n    Set expected packet loss in percent (default: 0)\n");
        p(L" --downmix-mono     Downmix to mono\n");
        p(L" --downmix-stereo   Downmix to stereo (if >2 channels)\n");
        p(L" --no-phase-inv     Disable use of phase inversion for intensity stereo\n");
        p(L" --max-delay n      Set maximum container delay in milliseconds\n");
        p(L"                      (0-1000, default: 1000)\n");
        p(L"\nMetadata options:\n");
        p(L" --title title      Set track title\n");
        p(L" --artist artist    Set artist or author, may be used multiple times\n");
        p(L" --album album      Set album or collection\n");
        p(L" --tracknumber n    Set track number\n");
        p(L" --genre genre      Set genre, may be used multiple times\n");
        p(L" --date YYYY-MM-DD  Set date of track (YYYY, YYYY-MM, or YYYY-MM-DD)\n");
        p(L" --comment tag=val  Add the given string as an extra comment\n");
        p(L"                      This may be used multiple times\n");
        p(L" --padding n        Reserve n extra bytes for metadata (default: 512)\n");
        p(L" --discard-comments Don't keep metadata when transcoding\n");
        p(L" --discard-pictures Don't keep pictures when transcoding\n");
        p(L"\nInput options:\n");
        p(L" --ignorelength     Ignore the data length in Wave headers\n");
        p(L"\nDiagnostic options:\n");
        p(L" --serial n         Force use of a specific stream serial number\n");
        p(L" --set-ctl-int x=y  Pass the encoder control x with value y (advanced)\n");
        p(L"                      Preface with s: to direct the ctl to multistream s\n");
        p(L"                      This may be used multiple times\n");
        p(L"\nctOpus options (not stored in ENCODER_OPTIONS):\n");
        p(L" --threads n        Number of worker threads (default: all logical processors)\n");
        p(L" --sequential       Encode on one thread (the reference path)\n");
        p(L" --stats            Print the times, the thread count and the peak memory\n");
    }

    void CommandLine::PrintVersion(const char* opusVersion)
    {
        Console::Print(Stream::Output, L"ctOpus %hs, compatible with opusenc %hs %hs (using %hs)\n", CTOPUS_VERSION_STRING, PackageName, PackageVersion,
                       opusVersion);
        Console::Print(Stream::Output, L"%hs\n", CT_SAMPLES_COPYRIGHT);
        Console::Print(Stream::Output, L"opusenc: Copyright (C) 2008-2018 Xiph.Org Foundation\n");
    }

    // is_valid_ctl
    bool CommandLine::IsValidCtl(std::int32_t request) noexcept
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

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
