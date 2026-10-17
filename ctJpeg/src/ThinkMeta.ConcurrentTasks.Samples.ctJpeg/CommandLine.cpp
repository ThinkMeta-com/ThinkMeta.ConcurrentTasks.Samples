// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "CommandLine.h"

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    namespace {

        // Splits "--name=value" into name and value; value is nullptr without '='.
        void SplitOption(const wchar_t* arg, wchar_t* name, std::size_t nameSize, const wchar_t*& value)
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

        bool ParseInt(const wchar_t* text, std::int32_t& value)
        {
            wchar_t*   end = nullptr;
            const long v   = std::wcstol(text, &end, 10);
            if (end == text || *end != L'\0')
                return false;
            value = static_cast<std::int32_t>(v);
            return true;
        }

        bool ParseFloat(const wchar_t* text, float& value)
        {
            wchar_t*     end = nullptr;
            const double v   = std::wcstod(text, &end);
            if (end == text || *end != L'\0')
                return false;
            value = static_cast<float>(v);
            return true;
        }

    } // namespace

    void PrintUsage(const wchar_t* programName)
    {
        std::fwprintf(stdout,
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
                      L" --quiet            no messages\n",
                      programName);
    }

    std::int32_t ParseCommandLine(std::int32_t argc, wchar_t** argv, EncoderOptions& options, FrontendOptions& frontend)
    {
        const wchar_t* programName = argc > 0 ? argv[0] : L"ctJpeg";
        bool           haveQuality = false, haveDistance = false;
        for (std::int32_t i = 1; i < argc; ++i) {
            const wchar_t* arg = argv[i];
            if (arg[0] != L'-' || arg[1] == L'\0') {
                if (frontend.inPath == nullptr)
                    frontend.inPath = arg;
                else if (frontend.outPath == nullptr)
                    frontend.outPath = arg;
                else {
                    std::fwprintf(stderr, L"%ls: unexpected argument '%ls'\n", programName, arg);
                    return 1;
                }
                continue;
            }
            wchar_t        name[64];
            const wchar_t* value = nullptr;
            SplitOption(arg, name, sizeof(name) / sizeof(name[0]), value);

            // options without a value
            if (std::wcscmp(name, L"-h") == 0 || std::wcscmp(name, L"--help") == 0) {
                PrintUsage(programName);
                return 2;
            }
            if (std::wcscmp(name, L"--sequential") == 0) {
                frontend.sequential = true;
                continue;
            }
            if (std::wcscmp(name, L"--quiet") == 0) {
                frontend.quiet = true;
                continue;
            }
            if (std::wcscmp(name, L"--times") == 0) {
                frontend.times = true;
                continue;
            }

            // options with a value: "--opt=v", "--opt v" or "-o v"
            const bool known = std::wcscmp(name, L"-q") == 0 || std::wcscmp(name, L"--quality") == 0 || std::wcscmp(name, L"-d") == 0
                || std::wcscmp(name, L"--distance") == 0 || std::wcscmp(name, L"--chroma_subsampling") == 0 || std::wcscmp(name, L"-p") == 0
                || std::wcscmp(name, L"--progressive_level") == 0 || std::wcscmp(name, L"--simd") == 0 || std::wcscmp(name, L"--threads") == 0
                || std::wcscmp(name, L"--dump-coefficients") == 0 || std::wcscmp(name, L"--band-rows") == 0 || std::wcscmp(name, L"--scan-bands") == 0
                || std::wcscmp(name, L"--strip-width") == 0;
            if (!known) {
                std::fwprintf(stderr, L"%ls: option '%ls' is not supported\n", programName, name);
                return 1;
            }
            if (value == nullptr) {
                if (i + 1 >= argc) {
                    std::fwprintf(stderr, L"%ls: option '%ls' needs a value\n", programName, name);
                    return 1;
                }
                value = argv[++i];
            }

            if (std::wcscmp(name, L"--dump-coefficients") == 0) {
                frontend.dumpPath = value;
            } else if (std::wcscmp(name, L"--scan-bands") == 0) {
                if (!ParseInt(value, frontend.scanBands) || frontend.scanBands < 1) {
                    std::fwprintf(stderr, L"%ls: invalid scan band count '%ls'\n", programName, value);
                    return 1;
                }
            } else if (std::wcscmp(name, L"--strip-width") == 0) {
                if (!ParseInt(value, frontend.stripWidth) || frontend.stripWidth < 1) {
                    std::fwprintf(stderr, L"%ls: invalid strip width '%ls'\n", programName, value);
                    return 1;
                }
            } else if (std::wcscmp(name, L"--band-rows") == 0) {
                if (!ParseInt(value, frontend.bandRows) || frontend.bandRows < 1) {
                    std::fwprintf(stderr, L"%ls: invalid band size '%ls'\n", programName, value);
                    return 1;
                }
            } else if (std::wcscmp(name, L"-q") == 0 || std::wcscmp(name, L"--quality") == 0) {
                if (!ParseInt(value, options.quality) || options.quality < 1 || options.quality > 100) {
                    std::fwprintf(stderr, L"%ls: invalid quality '%ls'\n", programName, value);
                    return 1;
                }
                options.useQuality = true;
                haveQuality        = true;
            } else if (std::wcscmp(name, L"-d") == 0 || std::wcscmp(name, L"--distance") == 0) {
                if (!ParseFloat(value, options.distance) || options.distance < 0.0f || options.distance > 25.0f) {
                    std::fwprintf(stderr, L"%ls: invalid distance '%ls'\n", programName, value);
                    return 1;
                }
                haveDistance = true;
            } else if (std::wcscmp(name, L"--chroma_subsampling") == 0) {
                if (std::wcscmp(value, L"444") == 0)
                    options.subsampling = ChromaSubsampling::Yuv444;
                else if (std::wcscmp(value, L"420") == 0)
                    options.subsampling = ChromaSubsampling::Yuv420;
                else {
                    std::fwprintf(stderr, L"%ls: chroma subsampling '%ls' is not supported (444 or 420)\n", programName, value);
                    return 1;
                }
            } else if (std::wcscmp(name, L"-p") == 0 || std::wcscmp(name, L"--progressive_level") == 0) {
                std::int32_t level = 0;
                if (!ParseInt(value, level) || level != 2) {
                    std::fwprintf(stderr, L"%ls: only progressive level 2 is supported\n", programName);
                    return 1;
                }
            } else if (std::wcscmp(name, L"--simd") == 0) {
                if (!ParseSimdLevel(value, frontend.simd)) {
                    std::fwprintf(stderr, L"%ls: --simd must be sse2 or avx2, not '%ls'\n", programName, value);
                    return 1;
                }
                frontend.simdGiven = true;
            } else { // --threads
                if (!ParseInt(value, frontend.threads) || frontend.threads < 0 || frontend.threads > 1024) {
                    std::fwprintf(stderr, L"%ls: invalid thread count '%ls'\n", programName, value);
                    return 1;
                }
            }
        }
        if (haveQuality && haveDistance) {
            std::fwprintf(stderr, L"%ls: --quality and --distance are mutually exclusive\n", programName);
            return 1;
        }
        if (frontend.inPath == nullptr || frontend.outPath == nullptr) {
            PrintUsage(programName);
            return 1;
        }
        return 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
