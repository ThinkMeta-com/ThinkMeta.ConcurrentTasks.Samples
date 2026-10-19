// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "EncoderSetup.h"
#include "Entropy.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>
    /// Rough work of a scan, for planning only: coefficient positions it visits; refinement scans
    /// also write their correction bits, so they count twice. A DC scan makes a token for every
    /// block from the dense DC plane, which costs about as much as 4 positions of an AC scan.
    /// </summary>
    std::int64_t ScanWork(const EncoderSetup& setup, std::int32_t scan) noexcept;

    /// <summary>
    /// The edge between the nodes Tokenize and Stitch (EncodeGraph.h): how the scans are cut into
    /// tokenization jobs, and what each band reports to the stitch. The tokens themselves go to the
    /// parts of the scans' ScanTokens.
    /// <list type="bullet">
    /// <item>The AC scans of a component whose work exceeds the target piece are cut into common
    /// bands of block rows; one job tokenizes a band through all AC scans of its component, so its
    /// coefficients come from the cache after the first scan. Its scan gets 2 * bands + 1 parts
    /// (prefix, band, ..., final).</item>
    /// <item>A DC scan is cut into bands of MCU rows, at least one per worker; its scan gets one
    /// part per band and a histogram per band.</item>
    /// <item>All other scans are tokenized whole, one job each.</item>
    /// </list>
    /// </summary>
    class TokenBands
    {
    public:
        /// <summary>A tokenization job: a whole scan (band -1), a band of one component through all its AC scans (scan -1), or a band of a DC scan.</summary>
        struct Job {
            std::int32_t scan, component, band, rowBegin, rowEnd;
            std::int64_t work;
        };

        /// <summary>
        /// Plans the jobs, largest first, and allocates the parts of the banded scans in
        /// scanTokens; scanBands > 0 forces that many bands per scan (check tool). False if out
        /// of memory.
        /// </summary>
        bool Plan(const EncoderSetup& setup, ScanTokens* scanTokens, std::uint32_t workers, std::int32_t scanBands) noexcept;

        std::unique_ptr<Job[]> jobs;
        std::int32_t           jobCount = 0;

        std::array<std::array<std::int32_t, 4>, MaxComponents> componentScans{}; // banded AC scans per component
        std::array<std::int32_t, MaxComponents>                componentScanCount{};
        std::array<std::int32_t, MaxScans>                     scanBands{};   // per scan: 0 = tokenized whole
        std::array<std::unique_ptr<BandResult[]>, MaxScans>    bandResults;   // per band of an AC scan
        std::array<std::unique_ptr<Histograms[]>, MaxScans>    dcHistograms;  // per band of a DC scan
        std::array<std::int32_t, MaxScans>                     stitchScans{}; // the banded scans, one Stitch job each
        std::int32_t                                           stitchCount = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
