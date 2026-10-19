//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "CommandLine.h"

#include <cstdint>
#include <cwchar>

#include "Console.h"
#include "VersionInfo.h"
#include "ctDeflateVersion.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    using Shared::Console;
    using Shared::Stream;

    Shared::ParseResult CommandLine::Parse(std::int32_t argc, wchar_t** argv, Options& options)
    {
        const auto usageError = [] {
            PrintUsage(Stream::Error);
            return Shared::ParseResult::Error;
        };
        for (std::int32_t i = 1; i < argc; ++i) {
            const wchar_t* arg = argv[i];
            if (std::wcscmp(arg, L"-h") == 0 || std::wcscmp(arg, L"--help") == 0) {
                PrintUsage(Stream::Output);
                return Shared::ParseResult::Exit;
            }
            if (std::wcscmp(arg, L"--version") == 0) {
                PrintVersion();
                return Shared::ParseResult::Exit;
            }
            if (arg[0] == L'-' && arg[1] >= L'0' && arg[1] <= L'9' && arg[2] == 0)
                options.level = arg[1] - L'0';
            else if (std::wcscmp(arg, L"-o") == 0 && i + 1 < argc)
                options.output = argv[++i];
            else if (std::wcscmp(arg, L"--threads") == 0 && i + 1 < argc)
                options.threads = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
            else if (std::wcscmp(arg, L"--chunk") == 0 && i + 1 < argc)
                options.chunks.chunkSize = static_cast<std::size_t>(std::wcstoull(argv[++i], nullptr, 10));
            else if (std::wcscmp(arg, L"--ahead") == 0 && i + 1 < argc)
                options.chunks.ahead = static_cast<std::size_t>(std::wcstoull(argv[++i], nullptr, 10));
            else if (std::wcscmp(arg, L"--sequential") == 0)
                options.sequential = true;
            else if (std::wcscmp(arg, L"--in-memory") == 0)
                options.inMemory = true;
            else if (std::wcscmp(arg, L"--stats") == 0 || std::wcscmp(arg, L"-v") == 0)
                options.stats = true;
            else if (arg[0] == L'-' || options.input != nullptr)
                return usageError();
            else
                options.input = arg;
        }
        return options.input != nullptr && options.chunks.chunkSize >= 4096 ? Shared::ParseResult::Run : usageError();
    }

    void CommandLine::PrintUsage(Stream stream)
    {
        Console::Print(stream,
                       L"Usage: ctDeflate [-0..-9] [-o OUTPUT] [--threads N] [--sequential] [--in-memory] [--chunk BYTES] [--ahead N] [--stats] FILE\n"
                       L"Compresses FILE to FILE.gz (or OUTPUT), bit-identical to libdeflate 1.26 gzip.\n"
                       L"  -LEVEL        compression level 0..9 (default 6)\n"
                       L"  -o            output file\n"
                       L"  --threads N   worker threads (default: all logical processors)\n"
                       L"  --sequential  the sequential graph: one compressor task, streaming\n"
                       L"  --in-memory   read the whole file first and write the output at the end\n"
                       L"  --chunk BYTES speculative parse chunk size (default 1 MiB, at least 4096)\n"
                       L"  --ahead N     chunks parsed ahead of the resolver (default threads + 4)\n"
                       L"  --stats       print size, time (including file I/O), peak memory and pipeline counters\n"
                       L"  --help        print this text\n"
                       L"  --version     print the version\n");
    }

    void CommandLine::PrintVersion()
    {
        Console::Print(Stream::Output, L"ctDeflate %hs, based on libdeflate 1.26 (https://github.com/ebiggers/libdeflate)\n%hs\n", CTDEFLATE_VERSION_STRING,
                       CT_SAMPLES_COPYRIGHT);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
