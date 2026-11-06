//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "CommandLine.h"

#include <cstdint>

#include "Console.h"
#include "Text.h"
#include "VersionInfo.h"
#include "ctDeflateVersion.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    using Shared::Console;
    using Shared::Stream;
    using Shared::Text;

    Shared::ParseResult CommandLine::Parse(std::int32_t argc, Shared::Char** argv, Options& options)
    {
        const auto usageError = [] {
            PrintUsage(Stream::Error);
            return Shared::ParseResult::Error;
        };
        for (std::int32_t i = 1; i < argc; ++i) {
            const Shared::Char* arg = argv[i];
            if (Text::Equal(arg, CT_TEXT("-h")) || Text::Equal(arg, CT_TEXT("--help"))) {
                PrintUsage(Stream::Output);
                return Shared::ParseResult::Exit;
            }
            if (Text::Equal(arg, CT_TEXT("--version"))) {
                PrintVersion();
                return Shared::ParseResult::Exit;
            }
            if (arg[0] == '-' && arg[1] >= '0' && arg[1] <= '9' && arg[2] == 0)
                options.level = arg[1] - '0';
            else if (Text::Equal(arg, CT_TEXT("-o")) && i + 1 < argc)
                options.output = argv[++i];
            else if (Text::Equal(arg, CT_TEXT("--threads")) && i + 1 < argc)
                options.threads = Text::ToUInt32(argv[++i]);
            else if (Text::Equal(arg, CT_TEXT("--chunk")) && i + 1 < argc)
                options.chunks.chunkSize = static_cast<std::size_t>(Text::ToUInt64(argv[++i]));
            else if (Text::Equal(arg, CT_TEXT("--ahead")) && i + 1 < argc)
                options.chunks.ahead = static_cast<std::size_t>(Text::ToUInt64(argv[++i]));
            else if (Text::Equal(arg, CT_TEXT("--sequential")))
                options.sequential = true;
            else if (Text::Equal(arg, CT_TEXT("--in-memory")))
                options.inMemory = true;
            else if (Text::Equal(arg, CT_TEXT("--stats")) || Text::Equal(arg, CT_TEXT("-v")))
                options.stats = true;
            else if (arg[0] == '-' || options.input != nullptr)
                return usageError();
            else
                options.input = arg;
        }
        return options.input != nullptr && options.chunks.chunkSize >= 4096 ? Shared::ParseResult::Run : usageError();
    }

    void CommandLine::PrintUsage(Stream stream)
    {
        Console::Print(stream,
                       CT_TEXT("Usage: ctDeflate [-0..-9] [-o OUTPUT] [--threads N] [--sequential] [--in-memory] [--chunk BYTES] [--ahead N] [--stats] FILE\n")
                           CT_TEXT("Compresses FILE to FILE.gz (or OUTPUT), bit-identical to libdeflate 1.26 gzip.\n")
                               CT_TEXT("  -LEVEL        compression level 0..9 (default 6)\n") CT_TEXT("  -o            output file\n")
                                   CT_TEXT("  --threads N   worker threads (default: all logical processors)\n")
                                       CT_TEXT("  --sequential  the sequential graph: one compressor task, streaming\n")
                                           CT_TEXT("  --in-memory   read the whole file first and write the output at the end\n")
                                               CT_TEXT("  --chunk BYTES speculative parse chunk size (default 1 MiB, at least 4096)\n")
                                                   CT_TEXT("  --ahead N     chunks parsed ahead of the resolver (default threads + 4)\n")
                                                       CT_TEXT("  --stats       print size, time (including file I/O), peak memory and pipeline counters\n")
                                                           CT_TEXT("  --help        print this text\n") CT_TEXT("  --version     print the version\n"));
    }

    void CommandLine::PrintVersion()
    {
        Console::Print(Stream::Output, CT_TEXT("ctDeflate %hs, based on libdeflate 1.26 (https://github.com/ebiggers/libdeflate)\n%hs\n"),
                       CTDEFLATE_VERSION_STRING, CT_SAMPLES_COPYRIGHT);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
