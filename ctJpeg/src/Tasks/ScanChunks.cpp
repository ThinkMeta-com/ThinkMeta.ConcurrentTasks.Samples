// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "ScanChunks.h"

#include <cstdint>
#include <new>

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    bool ScanChunks::Plan(const EncoderSetup& s, const TokenBands& bands) noexcept
    {
        std::int32_t count = 0;
        std::int32_t total = 0;
        for (std::int32_t i = 0; i < s.scanCount; ++i) {
            const std::int32_t n                    = bands.scanBands[static_cast<std::size_t>(i)];
            chunkBase[static_cast<std::size_t>(i)]  = total;
            chunkCount[static_cast<std::size_t>(i)] = n;
            total += n;
            count += n > 0 ? n : 1;
        }
        totalChunks = total;
        jobs.reset(new (std::nothrow) Job[static_cast<std::size_t>(count)]);
        const std::size_t size = static_cast<std::size_t>(total) + 1;
        bits.reset(new (std::nothrow) ChunkBits[size]);
        segments.reset(new (std::nothrow) ByteBuffer[size]);
        cursors.reset(new (std::nothrow) ChunkCursor[size]);
        refBitCounts.reset(new (std::nothrow) std::int64_t[size]());
        runCounts.reset(new (std::nothrow) std::int64_t[size]());
        scanOf.reset(new (std::nothrow) std::int32_t[size]);
        partBegin.reset(new (std::nothrow) std::int32_t[size]);
        partEnd.reset(new (std::nothrow) std::int32_t[size]);
        countChunks.reset(new (std::nothrow) std::int32_t[size]);
        if (!jobs || !bits || !segments || !cursors || !refBitCounts || !runCounts || !scanOf || !partBegin || !partEnd || !countChunks)
            return false;
        for (std::int32_t i = 0; i < s.scanCount; ++i) {
            const ScanInfo&    si = s.scans[static_cast<std::size_t>(i)];
            const std::int32_t n  = bands.scanBands[static_cast<std::size_t>(i)];
            if (n == 0) {
                jobs[static_cast<std::size_t>(jobCount++)] = {i, -1, 0, 0, ScanWork(s, i)};
                continue;
            }
            const bool dc = si.ss == 0;
            for (std::int32_t b = 0; b < n; ++b) {
                const std::int32_t chunk = chunkBase[static_cast<std::size_t>(i)] + b;
                const auto         ci    = static_cast<std::size_t>(chunk);
                scanOf[ci]               = i;
                partBegin[ci]            = dc ? b : 2 * b;
                partEnd[ci]              = dc ? b + 1 : (b + 1 < n ? 2 * b + 2 : 2 * n + 1);
                if (si.ah > 0)
                    countChunks[static_cast<std::size_t>(countChunkCount++)] = chunk;
                jobs[static_cast<std::size_t>(jobCount++)] = {i, chunk, partBegin[ci], partEnd[ci], ScanWork(s, i) / n};
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

    void ScanChunks::PlaceCursors(const EncoderSetup& s, const ScanTokens* scanTokens) noexcept
    {
        for (std::int32_t i = 0; i < s.scanCount; ++i) {
            if (s.scans[static_cast<std::size_t>(i)].ah == 0)
                continue;
            std::int64_t refBits = 0, runs = 0;
            for (std::int32_t k = 0; k < chunkCount[static_cast<std::size_t>(i)]; ++k) {
                const auto ci = static_cast<std::size_t>(chunkBase[static_cast<std::size_t>(i)] + k);
                cursors[ci]   = LocateRefinementStreams(scanTokens[i], refBits, runs);
                refBits += refBitCounts[ci];
                runs += runCounts[ci];
            }
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
