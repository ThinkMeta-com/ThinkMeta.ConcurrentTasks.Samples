// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

#include "Console.h"
#include "Diagnostics.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// The first line of --stats, the same in every sample:
    /// "stats: 0.585 s (start 0.009, setup 0.002, work 0.574, rest 0.000), 16 threads, peak memory 52 MB".
    /// start: from the creation of the process to ParseArgs (Start); setup: to the start of the task graph (WorkBegins);
    /// work: the task graph with its file I/O (to WorkEnds); rest: to Print. The details of the sample follow, indented.
    /// <para>Header only, like Diagnostics (CT_SAMPLES_NO_TIMING). Preliminary.</para>
    /// </summary>
    class RunStats
    {
    public:
        /// <summary>At the start of ParseArgs.</summary>
        void Start() noexcept
        {
            startup_ = Diagnostics::SinceProcessStart();
            t0_      = Diagnostics::Now();
        }

        /// <summary>Right before the task graph runs.</summary>
        void WorkBegins() noexcept { work0_ = Diagnostics::Now(); }

        /// <summary>Right after the task graph has ended.</summary>
        void WorkEnds() noexcept { work1_ = Diagnostics::Now(); }

        /// <summary>Seconds from the creation of the process to Start.</summary>
        double Startup() const noexcept { return startup_; }

        /// <summary>Seconds from Start to WorkBegins.</summary>
        double Setup() const noexcept { return work0_ - t0_; }

        /// <summary>Seconds from WorkBegins to WorkEnds.</summary>
        double Work() const noexcept { return work1_ - work0_; }

        /// <summary>The line, to stderr; threads: of the compute scheduler.</summary>
        void Print(std::uint32_t threads, bool sequential) const noexcept
        {
            const double end = Diagnostics::Now();
            Console::Print(Stream::Error, L"stats: %.3f s (start %.3f, setup %.3f, work %.3f, rest %.3f), %u thread%ls%ls, peak memory %.0f MB\n",
                           startup_ + (end - t0_), startup_, Setup(), Work(), end - work1_, threads, threads != 1 ? L"s" : L"",
                           sequential ? L" (sequential)" : L"", Diagnostics::PeakMemoryMB());
        }

    private:
        double startup_ = 0.0;
        double t0_      = 0.0;
        double work0_   = 0.0;
        double work1_   = 0.0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
