// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "TokenBands.h"

#include <array>
#include <cstdint>
#include <new>

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    std::int64_t ScanWork(const EncoderSetup& s, std::int32_t scan) noexcept
    {
        const ScanInfo& si     = s.scans[static_cast<std::size_t>(scan)];
        std::int64_t    blocks = 0;
        for (std::int32_t i = 0; i < si.componentCount; ++i) {
            const auto c = static_cast<std::size_t>(si.components[static_cast<std::size_t>(i)]);
            blocks += static_cast<std::int64_t>(s.widthInBlocks[c]) * s.heightInBlocks[c];
        }
        return si.ss == 0 ? blocks * 4 : blocks * (si.se - si.ss + 1) * (si.ah > 0 ? 2 : 1);
    }

    bool TokenBands::Plan(const EncoderSetup& s, ScanTokens* scanTokens, std::uint32_t workers, std::int32_t forcedBands) noexcept
    {
        std::int64_t total = 0;
        for (std::int32_t i = 0; i < s.scanCount; ++i)
            total += ScanWork(s, i);
        // small pieces (about 16 per worker): the work of a band is only roughly known, and the
        // bands that finish last decide the stage time; the stitch per band is cheap
        const std::int64_t target = total / (16 * static_cast<std::int64_t>(workers)) + 1;

        std::array<std::int32_t, MaxComponents> componentBands{};
        std::array<std::int64_t, MaxComponents> componentWork{};
        for (std::int32_t i = 0; i < s.scanCount; ++i)
            if (CanSplitScan(s, i))
                componentWork[static_cast<std::size_t>(s.scans[static_cast<std::size_t>(i)].components[0])] += ScanWork(s, i);
        std::int32_t count = 0;
        for (std::int32_t comp = 0; comp < s.frame.components; ++comp) {
            const auto         ci    = static_cast<std::size_t>(comp);
            const std::int32_t rows  = s.heightInBlocks[ci];
            std::int32_t       bands = forcedBands > 0 ? forcedBands : static_cast<std::int32_t>((componentWork[ci] + target - 1) / target);
            bands                    = bands > rows ? rows : bands;
            componentBands[ci]       = componentWork[ci] > 0 && bands >= 2 ? bands : 0;
            count += componentBands[ci];
        }
        for (std::int32_t i = 0; i < s.scanCount; ++i) {
            const ScanInfo& si = s.scans[static_cast<std::size_t>(i)];
            if (si.ss == 0) {
                const std::int32_t rows = DcScanMcuRows(s, i);
                // at least one band per worker: the scan is cheap per block, but as a whole it
                // would be the last job to finish
                std::int32_t bands = forcedBands > 0 ? forcedBands : static_cast<std::int32_t>((ScanWork(s, i) + target - 1) / target);
                if (forcedBands <= 0 && bands < static_cast<std::int32_t>(workers))
                    bands = static_cast<std::int32_t>(workers);
                bands = bands > rows ? rows : bands;
                if (bands < 2) {
                    ++count;
                    continue;
                }
                scanBands[static_cast<std::size_t>(i)] = bands;
                if (!scanTokens[i].AllocateParts(bands))
                    return false;
                dcHistograms[static_cast<std::size_t>(i)].reset(new (std::nothrow) Histograms[static_cast<std::size_t>(bands)]());
                if (!dcHistograms[static_cast<std::size_t>(i)])
                    return false;
                stitchScans[static_cast<std::size_t>(stitchCount++)] = i;
                count += bands;
                continue;
            }
            const bool banded = CanSplitScan(s, i) && componentBands[static_cast<std::size_t>(si.components[0])] > 0;
            if (!banded) {
                ++count;
                continue;
            }
            const auto         comp                                                    = static_cast<std::size_t>(si.components[0]);
            const std::int32_t bands                                                   = componentBands[comp];
            scanBands[static_cast<std::size_t>(i)]                                     = bands;
            componentScans[comp][static_cast<std::size_t>(componentScanCount[comp]++)] = i;
            if (!scanTokens[i].AllocateParts(2 * bands + 1))
                return false;
            bandResults[static_cast<std::size_t>(i)].reset(new (std::nothrow) BandResult[static_cast<std::size_t>(bands)]);
            if (!bandResults[static_cast<std::size_t>(i)])
                return false;
            stitchScans[static_cast<std::size_t>(stitchCount++)] = i;
        }
        jobs.reset(new (std::nothrow) Job[static_cast<std::size_t>(count)]);
        if (!jobs)
            return false;
        for (std::int32_t i = 0; i < s.scanCount; ++i) {
            const std::int32_t bands = scanBands[static_cast<std::size_t>(i)];
            if (bands == 0)
                jobs[static_cast<std::size_t>(jobCount++)] = {i, -1, -1, 0, 0, ScanWork(s, i)};
            else if (dcHistograms[static_cast<std::size_t>(i)]) {
                const std::int32_t rows = DcScanMcuRows(s, i);
                for (std::int32_t b = 0; b < bands; ++b) {
                    const std::int32_t r0                      = static_cast<std::int32_t>(static_cast<std::int64_t>(rows) * b / bands);
                    const std::int32_t r1                      = static_cast<std::int32_t>(static_cast<std::int64_t>(rows) * (b + 1) / bands);
                    jobs[static_cast<std::size_t>(jobCount++)] = {i, -1, b, r0, r1, ScanWork(s, i) * (r1 - r0) / rows};
                }
            }
        }
        for (std::int32_t comp = 0; comp < s.frame.components; ++comp) {
            const auto         ci    = static_cast<std::size_t>(comp);
            const std::int32_t bands = componentBands[ci];
            const std::int32_t rows  = s.heightInBlocks[ci];
            for (std::int32_t b = 0; b < bands; ++b) {
                const std::int32_t r0                      = static_cast<std::int32_t>(static_cast<std::int64_t>(rows) * b / bands);
                const std::int32_t r1                      = static_cast<std::int32_t>(static_cast<std::int64_t>(rows) * (b + 1) / bands);
                jobs[static_cast<std::size_t>(jobCount++)] = {-1, comp, b, r0, r1, componentWork[ci] * (r1 - r0) / rows};
            }
        }
        for (std::int32_t a = 1; a < jobCount; ++a) { // largest first
            const Job    v = jobs[static_cast<std::size_t>(a)];
            std::int32_t b = a;
            for (; b > 0 && jobs[static_cast<std::size_t>(b - 1)].work < v.work; --b)
                jobs[static_cast<std::size_t>(b)] = jobs[static_cast<std::size_t>(b - 1)];
            jobs[static_cast<std::size_t>(b)] = v;
        }
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
