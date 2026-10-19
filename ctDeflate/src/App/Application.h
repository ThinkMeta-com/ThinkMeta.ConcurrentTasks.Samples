//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "CommandLine.h"
#include "RunStats.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    struct ParallelStats;

    /// <summary>
    /// The program ctDeflate: the command line (ParseArgs), then either the compression (Execute) or the usage text
    /// (ParseError). The same three steps in every sample.
    /// </summary>
    class Application
    {
    public:
        /// <summary>Debug builds: CRT reports to stderr (DebugReports).</summary>
        Application();

        /// <summary>Reads the command line (CommandLine). False on an error.</summary>
        bool ParseArgs(std::int32_t argc, wchar_t** argv);

        /// <summary>Compresses the file; returns the exit code.</summary>
        std::int32_t Execute();

        /// <summary>After a failed ParseArgs, which has printed the usage text: returns the exit code.</summary>
        std::int32_t ParseError();

    private:
        static void PrintStats(const ParallelStats& stats);

        Options             options_;
        Shared::ParseResult result_ = Shared::ParseResult::Error;
        Shared::RunStats    stats_; // --stats
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
