//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#pragma once

#include <cstdint>

#include "ChunkedDeflate.h"
#include "Console.h"
#include "ParseResult.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    /// <summary>The settings of the command line.</summary>
    struct Options {
        std::int32_t   level      = 6;
        const wchar_t* input      = nullptr;
        const wchar_t* output     = nullptr; // FILE.gz unless -o
        std::uint32_t  threads    = 0;       // --threads N: worker threads, 0 = one per logical processor
        bool           sequential = false;
        bool           inMemory   = false;
        bool           stats      = false; // --stats (or -v): size, time, peak memory and pipeline counters
        ChunkOptions   chunks;
    };

    /// <summary>
    /// The command line of ctDeflate: [-0..-9] [-o OUTPUT] [--threads N] [--sequential] [--in-memory] [--chunk BYTES]
    /// [--ahead N] [--stats] FILE, or --help, --version. The same interface in every sample.
    /// </summary>
    class CommandLine
    {
    public:
        /// <summary>Reads the command line into options; prints the usage text on an unknown option or a missing file name.</summary>
        static Shared::ParseResult Parse(std::int32_t argc, wchar_t** argv, Options& options);

        /// <summary>The usage text: to stdout for --help, to stderr after an error.</summary>
        static void PrintUsage(Shared::Stream stream);

        /// <summary>The version and the copyright, to stdout.</summary>
        static void PrintVersion();
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
