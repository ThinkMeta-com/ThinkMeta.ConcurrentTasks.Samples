// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "CommandLine.h"

#include <cstdint>
#include <cwctype>

#include "Console.h"
#include "Text.h"
#include "VersionInfo.h"
#include "ctJpegVersion.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    using Shared::Console;
    using Shared::Stream;
    using Shared::Text;

    Shared::ParseResult CommandLine::Parse(std::int32_t argc, Shared::Char** argv, Options& options)
    {
        const Shared::Char* programName = argc > 0 ? argv[0] : CT_TEXT("ctJpeg");
        bool                haveQuality = false, haveDistance = false;
        for (std::int32_t i = 1; i < argc; ++i) {
            const Shared::Char* arg = argv[i];
            if (arg[0] != '-' || arg[1] == '\0') {
                if (options.inPath == nullptr)
                    options.inPath = arg;
                else if (options.outPath == nullptr)
                    options.outPath = arg;
                else {
                    Console::Print(Stream::Error, CT_TEXT("%s: unexpected argument '%s'\n"), programName, arg);
                    return Shared::ParseResult::Error;
                }
                continue;
            }
            Shared::Char        name[64];
            const Shared::Char* value = nullptr;
            SplitOption(arg, name, sizeof(name) / sizeof(name[0]), value);

            // options without a value
            if (Text::Equal(name, CT_TEXT("-h")) || Text::Equal(name, CT_TEXT("--help"))) {
                PrintUsage(programName);
                return Shared::ParseResult::Exit;
            }
            if (Text::Equal(name, CT_TEXT("--version"))) {
                PrintVersion();
                return Shared::ParseResult::Exit;
            }
            if (Text::Equal(name, CT_TEXT("--sequential"))) {
                options.sequential = true;
                continue;
            }
            if (Text::Equal(name, CT_TEXT("--quiet"))) {
                options.quiet = true;
                continue;
            }
            if (Text::Equal(name, CT_TEXT("--stats")) || Text::Equal(name, CT_TEXT("--times"))) {
                options.stats = true;
                continue;
            }
            if (Text::Equal(name, CT_TEXT("--features"))) {
                options.features = true;
                continue;
            }

            // options with a value: "--opt=v", "--opt v" or "-o v"
            const bool known = Text::Equal(name, CT_TEXT("-q")) || Text::Equal(name, CT_TEXT("--quality")) || Text::Equal(name, CT_TEXT("-d"))
                || Text::Equal(name, CT_TEXT("--distance")) || Text::Equal(name, CT_TEXT("--chroma_subsampling")) || Text::Equal(name, CT_TEXT("-p"))
                || Text::Equal(name, CT_TEXT("--progressive_level")) || Text::Equal(name, CT_TEXT("--simd")) || Text::Equal(name, CT_TEXT("--threads"))
                || Text::Equal(name, CT_TEXT("--dump-coefficients")) || Text::Equal(name, CT_TEXT("--band-rows")) || Text::Equal(name, CT_TEXT("--scan-bands"))
                || Text::Equal(name, CT_TEXT("--strip-width"));
            if (!known) {
                Console::Print(Stream::Error, CT_TEXT("%s: option '%s' is not supported\n"), programName, name);
                return Shared::ParseResult::Error;
            }
            if (value == nullptr) {
                if (i + 1 >= argc) {
                    Console::Print(Stream::Error, CT_TEXT("%s: option '%s' needs a value\n"), programName, name);
                    return Shared::ParseResult::Error;
                }
                value = argv[++i];
            }

            if (Text::Equal(name, CT_TEXT("--dump-coefficients"))) {
                options.dumpPath = value;
            } else if (Text::Equal(name, CT_TEXT("--scan-bands"))) {
                if (!ParseInt(value, options.scanBands) || options.scanBands < 1) {
                    Console::Print(Stream::Error, CT_TEXT("%s: invalid scan band count '%s'\n"), programName, value);
                    return Shared::ParseResult::Error;
                }
            } else if (Text::Equal(name, CT_TEXT("--strip-width"))) {
                if (!ParseInt(value, options.stripWidth) || options.stripWidth < 1) {
                    Console::Print(Stream::Error, CT_TEXT("%s: invalid strip width '%s'\n"), programName, value);
                    return Shared::ParseResult::Error;
                }
            } else if (Text::Equal(name, CT_TEXT("--band-rows"))) {
                if (!ParseInt(value, options.bandRows) || options.bandRows < 1) {
                    Console::Print(Stream::Error, CT_TEXT("%s: invalid band size '%s'\n"), programName, value);
                    return Shared::ParseResult::Error;
                }
            } else if (Text::Equal(name, CT_TEXT("-q")) || Text::Equal(name, CT_TEXT("--quality"))) {
                if (!ParseInt(value, options.encoder.quality) || options.encoder.quality < 1 || options.encoder.quality > 100) {
                    Console::Print(Stream::Error, CT_TEXT("%s: invalid quality '%s'\n"), programName, value);
                    return Shared::ParseResult::Error;
                }
                options.encoder.useQuality = true;
                haveQuality                = true;
            } else if (Text::Equal(name, CT_TEXT("-d")) || Text::Equal(name, CT_TEXT("--distance"))) {
                if (!ParseFloat(value, options.encoder.distance) || options.encoder.distance < 0.0f || options.encoder.distance > 25.0f) {
                    Console::Print(Stream::Error, CT_TEXT("%s: invalid distance '%s'\n"), programName, value);
                    return Shared::ParseResult::Error;
                }
                haveDistance = true;
            } else if (Text::Equal(name, CT_TEXT("--chroma_subsampling"))) {
                if (Text::Equal(value, CT_TEXT("444")))
                    options.encoder.subsampling = ChromaSubsampling::Yuv444;
                else if (Text::Equal(value, CT_TEXT("420")))
                    options.encoder.subsampling = ChromaSubsampling::Yuv420;
                else {
                    Console::Print(Stream::Error, CT_TEXT("%s: chroma subsampling '%s' is not supported (444 or 420)\n"), programName, value);
                    return Shared::ParseResult::Error;
                }
            } else if (Text::Equal(name, CT_TEXT("-p")) || Text::Equal(name, CT_TEXT("--progressive_level"))) {
                std::int32_t level = 0;
                if (!ParseInt(value, level) || level != 2) {
                    Console::Print(Stream::Error, CT_TEXT("%s: only progressive level 2 is supported\n"), programName);
                    return Shared::ParseResult::Error;
                }
            } else if (Text::Equal(name, CT_TEXT("--simd"))) {
                if (!ParseSimdLevel(value, options.simd)) {
                    Console::Print(Stream::Error, CT_TEXT("%s: --simd must be sse2 or avx2, not '%s'\n"), programName, value);
                    return Shared::ParseResult::Error;
                }
                options.simdGiven = true;
            } else { // --threads
                if (!ParseInt(value, options.threads) || options.threads < 0 || options.threads > 1024) {
                    Console::Print(Stream::Error, CT_TEXT("%s: invalid thread count '%s'\n"), programName, value);
                    return Shared::ParseResult::Error;
                }
            }
        }
        if (haveQuality && haveDistance) {
            Console::Print(Stream::Error, CT_TEXT("%s: --quality and --distance are mutually exclusive\n"), programName);
            return Shared::ParseResult::Error;
        }
        if (options.inPath == nullptr || options.outPath == nullptr) {
            PrintUsage(programName);
            return Shared::ParseResult::Error;
        }
        return Shared::ParseResult::Run;
    }

    void CommandLine::PrintUsage(const Shared::Char* programName)
    {
        PrintVersion();
        Console::Print(Stream::Output, CT_TEXT("\n"));
        Console::Print(Stream::Output,
                       CT_TEXT("Usage: %s INPUT OUTPUT [OPTIONS...]\n") CT_TEXT(" INPUT   binary PPM (P6) or PGM (P5), 8 bit\n")
                           CT_TEXT(" OUTPUT  the JPEG file, bit-identical to cjpegli (jpegli 031a007) per feature set\n")
                               CT_TEXT(" -q QUALITY, --quality=QUALITY   1..100, mapped to a distance as in cjpegli\n")
                                   CT_TEXT(" -d DISTANCE, --distance=DISTANCE  Butteraugli distance 0..25 (default 1.0)\n")
                                       CT_TEXT(" --chroma_subsampling=444|420    default: no subsampling\n")
                                           CT_TEXT(" -p 2, --progressive_level=2     the only level ported\n")
                                               CT_TEXT(" --simd sse2|avx2   feature set, each bit-identical to jpegli on that target\n")
                                                   CT_TEXT("                    (default: the best the CPU has; an unavailable one is an error)\n")
                                                       CT_TEXT(" --threads N        threads of the scheduler (default: logical processors)\n")
                                                           CT_TEXT(" --sequential       the reference path in one task\n")
                                                               CT_TEXT(" --features         print the CPU features and the feature set in use\n")
                                                                   CT_TEXT(" --stats            print the time of each phase and of the serial parts\n")
                                                                       CT_TEXT(" --quiet            no messages\n")
                                                                           CT_TEXT(" --version          print the version\n"),
                       programName);
    }

    void CommandLine::PrintVersion()
    {
        Console::Print(Stream::Output, CT_TEXT("ctJpeg %hs, based on jpegli 031a007 (https://github.com/google/jpegli)\n%hs\n"), CTJPEG_VERSION_STRING,
                       CT_SAMPLES_COPYRIGHT);
    }

    void CommandLine::SplitOption(const Shared::Char* arg, Shared::Char* name, std::size_t nameSize, const Shared::Char*& value)
    {
        const Shared::Char* eq = Text::Find(arg, '=');
        value                  = nullptr;
        std::size_t len        = eq != nullptr ? static_cast<std::size_t>(eq - arg) : Text::Length(arg);
        if (len >= nameSize)
            len = nameSize - 1;
        Text::Copy(name, len + 1, arg);
        if (eq != nullptr && arg[0] == '-' && arg[1] == '-')
            value = eq + 1;
    }

    bool CommandLine::ParseInt(const Shared::Char* text, std::int32_t& value)
    {
        const Shared::Char* end = nullptr;
        const std::int32_t  v   = Text::ToInt32(text, &end);
        if (end == text || *end != '\0')
            return false;
        value = static_cast<std::int32_t>(v);
        return true;
    }

    bool CommandLine::ParseFloat(const Shared::Char* text, float& value)
    {
        const Shared::Char* end = nullptr;
        const double        v   = Text::ToDouble(text, &end);
        if (end == text || *end != '\0')
            return false;
        value = static_cast<float>(v);
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
