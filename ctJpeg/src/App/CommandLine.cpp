// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "CommandLine.h"

#include <cstdint>
#include <cwchar>
#include <cwctype>

#include "Console.h"
#include "VersionInfo.h"
#include "ctJpegVersion.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    using Shared::Console;
    using Shared::Stream;

    Shared::ParseResult CommandLine::Parse(std::int32_t argc, wchar_t** argv, Options& options)
    {
        const wchar_t* programName = argc > 0 ? argv[0] : L"ctJpeg";
        bool           haveQuality = false, haveDistance = false;
        for (std::int32_t i = 1; i < argc; ++i) {
            const wchar_t* arg = argv[i];
            if (arg[0] != L'-' || arg[1] == L'\0') {
                if (options.inPath == nullptr)
                    options.inPath = arg;
                else if (options.outPath == nullptr)
                    options.outPath = arg;
                else {
                    Console::Print(Stream::Error, L"%ls: unexpected argument '%ls'\n", programName, arg);
                    return Shared::ParseResult::Error;
                }
                continue;
            }
            wchar_t        name[64];
            const wchar_t* value = nullptr;
            SplitOption(arg, name, sizeof(name) / sizeof(name[0]), value);

            // options without a value
            if (std::wcscmp(name, L"-h") == 0 || std::wcscmp(name, L"--help") == 0) {
                PrintUsage(programName);
                return Shared::ParseResult::Exit;
            }
            if (std::wcscmp(name, L"--version") == 0) {
                PrintVersion();
                return Shared::ParseResult::Exit;
            }
            if (std::wcscmp(name, L"--sequential") == 0) {
                options.sequential = true;
                continue;
            }
            if (std::wcscmp(name, L"--quiet") == 0) {
                options.quiet = true;
                continue;
            }
            if (std::wcscmp(name, L"--stats") == 0 || std::wcscmp(name, L"--times") == 0) {
                options.stats = true;
                continue;
            }
            if (std::wcscmp(name, L"--features") == 0) {
                options.features = true;
                continue;
            }

            // options with a value: "--opt=v", "--opt v" or "-o v"
            const bool known = std::wcscmp(name, L"-q") == 0 || std::wcscmp(name, L"--quality") == 0 || std::wcscmp(name, L"-d") == 0
                || std::wcscmp(name, L"--distance") == 0 || std::wcscmp(name, L"--chroma_subsampling") == 0 || std::wcscmp(name, L"-p") == 0
                || std::wcscmp(name, L"--progressive_level") == 0 || std::wcscmp(name, L"--simd") == 0 || std::wcscmp(name, L"--threads") == 0
                || std::wcscmp(name, L"--dump-coefficients") == 0 || std::wcscmp(name, L"--band-rows") == 0 || std::wcscmp(name, L"--scan-bands") == 0
                || std::wcscmp(name, L"--strip-width") == 0;
            if (!known) {
                Console::Print(Stream::Error, L"%ls: option '%ls' is not supported\n", programName, name);
                return Shared::ParseResult::Error;
            }
            if (value == nullptr) {
                if (i + 1 >= argc) {
                    Console::Print(Stream::Error, L"%ls: option '%ls' needs a value\n", programName, name);
                    return Shared::ParseResult::Error;
                }
                value = argv[++i];
            }

            if (std::wcscmp(name, L"--dump-coefficients") == 0) {
                options.dumpPath = value;
            } else if (std::wcscmp(name, L"--scan-bands") == 0) {
                if (!ParseInt(value, options.scanBands) || options.scanBands < 1) {
                    Console::Print(Stream::Error, L"%ls: invalid scan band count '%ls'\n", programName, value);
                    return Shared::ParseResult::Error;
                }
            } else if (std::wcscmp(name, L"--strip-width") == 0) {
                if (!ParseInt(value, options.stripWidth) || options.stripWidth < 1) {
                    Console::Print(Stream::Error, L"%ls: invalid strip width '%ls'\n", programName, value);
                    return Shared::ParseResult::Error;
                }
            } else if (std::wcscmp(name, L"--band-rows") == 0) {
                if (!ParseInt(value, options.bandRows) || options.bandRows < 1) {
                    Console::Print(Stream::Error, L"%ls: invalid band size '%ls'\n", programName, value);
                    return Shared::ParseResult::Error;
                }
            } else if (std::wcscmp(name, L"-q") == 0 || std::wcscmp(name, L"--quality") == 0) {
                if (!ParseInt(value, options.encoder.quality) || options.encoder.quality < 1 || options.encoder.quality > 100) {
                    Console::Print(Stream::Error, L"%ls: invalid quality '%ls'\n", programName, value);
                    return Shared::ParseResult::Error;
                }
                options.encoder.useQuality = true;
                haveQuality                = true;
            } else if (std::wcscmp(name, L"-d") == 0 || std::wcscmp(name, L"--distance") == 0) {
                if (!ParseFloat(value, options.encoder.distance) || options.encoder.distance < 0.0f || options.encoder.distance > 25.0f) {
                    Console::Print(Stream::Error, L"%ls: invalid distance '%ls'\n", programName, value);
                    return Shared::ParseResult::Error;
                }
                haveDistance = true;
            } else if (std::wcscmp(name, L"--chroma_subsampling") == 0) {
                if (std::wcscmp(value, L"444") == 0)
                    options.encoder.subsampling = ChromaSubsampling::Yuv444;
                else if (std::wcscmp(value, L"420") == 0)
                    options.encoder.subsampling = ChromaSubsampling::Yuv420;
                else {
                    Console::Print(Stream::Error, L"%ls: chroma subsampling '%ls' is not supported (444 or 420)\n", programName, value);
                    return Shared::ParseResult::Error;
                }
            } else if (std::wcscmp(name, L"-p") == 0 || std::wcscmp(name, L"--progressive_level") == 0) {
                std::int32_t level = 0;
                if (!ParseInt(value, level) || level != 2) {
                    Console::Print(Stream::Error, L"%ls: only progressive level 2 is supported\n", programName);
                    return Shared::ParseResult::Error;
                }
            } else if (std::wcscmp(name, L"--simd") == 0) {
                if (!ParseSimdLevel(value, options.simd)) {
                    Console::Print(Stream::Error, L"%ls: --simd must be sse2 or avx2, not '%ls'\n", programName, value);
                    return Shared::ParseResult::Error;
                }
                options.simdGiven = true;
            } else { // --threads
                if (!ParseInt(value, options.threads) || options.threads < 0 || options.threads > 1024) {
                    Console::Print(Stream::Error, L"%ls: invalid thread count '%ls'\n", programName, value);
                    return Shared::ParseResult::Error;
                }
            }
        }
        if (haveQuality && haveDistance) {
            Console::Print(Stream::Error, L"%ls: --quality and --distance are mutually exclusive\n", programName);
            return Shared::ParseResult::Error;
        }
        if (options.inPath == nullptr || options.outPath == nullptr) {
            PrintUsage(programName);
            return Shared::ParseResult::Error;
        }
        return Shared::ParseResult::Run;
    }

    void CommandLine::PrintUsage(const wchar_t* programName)
    {
        Console::Print(Stream::Output,
                       L"Usage: %ls INPUT OUTPUT [OPTIONS...]\n"
                       L" INPUT   binary PPM (P6) or PGM (P5), 8 bit\n"
                       L" OUTPUT  the JPEG file, bit-identical to cjpegli (jpegli 031a007) per feature set\n"
                       L" -q QUALITY, --quality=QUALITY   1..100, mapped to a distance as in cjpegli\n"
                       L" -d DISTANCE, --distance=DISTANCE  Butteraugli distance 0..25 (default 1.0)\n"
                       L" --chroma_subsampling=444|420    default: no subsampling\n"
                       L" -p 2, --progressive_level=2     the only level ported\n"
                       L" --simd sse2|avx2   feature set, each bit-identical to jpegli on that target\n"
                       L"                    (default: the best the CPU has; an unavailable one is an error)\n"
                       L" --threads N        threads of the scheduler (default: logical processors)\n"
                       L" --sequential       the reference path in one task\n"
                       L" --features         print the CPU features and the feature set in use\n"
                       L" --stats            print the time of each phase and of the serial parts\n"
                       L" --quiet            no messages\n"
                       L" --version          print the version\n",
                       programName);
    }

    void CommandLine::PrintVersion()
    {
        Console::Print(Stream::Output, L"ctJpeg %hs, based on jpegli 031a007 (https://github.com/google/jpegli)\n%hs\n", CTJPEG_VERSION_STRING,
                       CT_SAMPLES_COPYRIGHT);
    }

    void CommandLine::SplitOption(const wchar_t* arg, wchar_t* name, std::size_t nameSize, const wchar_t*& value)
    {
        const wchar_t* eq = std::wcschr(arg, L'=');
        value             = nullptr;
        std::size_t len   = eq != nullptr ? static_cast<std::size_t>(eq - arg) : std::wcslen(arg);
        if (len >= nameSize)
            len = nameSize - 1;
        wmemcpy(name, arg, len);
        name[len] = L'\0';
        if (eq != nullptr && arg[0] == L'-' && arg[1] == L'-')
            value = eq + 1;
    }

    bool CommandLine::ParseInt(const wchar_t* text, std::int32_t& value)
    {
        wchar_t*   end = nullptr;
        const long v   = std::wcstol(text, &end, 10);
        if (end == text || *end != L'\0')
            return false;
        value = static_cast<std::int32_t>(v);
        return true;
    }

    bool CommandLine::ParseFloat(const wchar_t* text, float& value)
    {
        wchar_t*     end = nullptr;
        const double v   = std::wcstod(text, &end);
        if (end == text || *end != L'\0')
            return false;
        value = static_cast<float>(v);
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
