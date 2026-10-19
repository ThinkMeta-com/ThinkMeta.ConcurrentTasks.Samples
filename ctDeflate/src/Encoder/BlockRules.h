//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include "DeflateTables.h"

#include <bit>
#include <cstddef>
#include <cstdint>

// Block rules shared by the sequential compressor (Compressor) and the block tracker of the parallel parse
// (BlockTracker), from libdeflate 1.26 lib/deflate_compress.c.

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    /// <summary>The parsing algorithm of a level (libdeflate 1.26).</summary>
    enum class ParseMode : std::uint8_t { None, Fastest, Greedy, Lazy, Lazy2 };

    /// <summary>Search parameters of a level; libdeflate: libdeflate_alloc_compressor_ex() (levels 0..9).</summary>
    struct LevelParams {
        ParseMode     mode;
        std::uint32_t maxSearchDepth;
        std::uint32_t niceMatchLength;
        std::size_t   maxPassthroughSize; // inputs up to this size are stored uncompressed

        static constexpr std::int32_t MaxLevel = 9;

        /// <summary>level: 0..MaxLevel (the caller checks it).</summary>
        static LevelParams For(std::int32_t level) noexcept
        {
            static constexpr struct {
                ParseMode     mode;
                std::uint32_t depth;
                std::uint32_t nice;
            } Table[MaxLevel + 1] = {
                {ParseMode::None, 0, 0},      {ParseMode::Fastest, 0, 32}, {ParseMode::Greedy, 6, 10}, {ParseMode::Greedy, 12, 14},
                {ParseMode::Greedy, 16, 30},  {ParseMode::Lazy, 16, 30},   {ParseMode::Lazy, 35, 65},  {ParseMode::Lazy, 100, 130},
                {ParseMode::Lazy2, 300, 258}, {ParseMode::Lazy2, 600, 258},
            };
            return {Table[level].mode, Table[level].depth, Table[level].nice, level == 0 ? SIZE_MAX : static_cast<std::size_t>(55 - level * 4)};
        }
    };

    /// <summary>libdeflate: bsr32(), the index of the highest set bit (v != 0).</summary>
    inline std::int32_t Bsr32(std::uint32_t v) noexcept
    {
        return 31 - std::countl_zero(v);
    }

    /// <summary>libdeflate: struct block_split_stats, the observations that decide where a block ends.</summary>
    struct SplitStats {
        std::uint32_t newObservations[NumObservationTypes];
        std::uint32_t observations[NumObservationTypes];
        std::uint32_t numNewObservations;
        std::uint32_t numObservations;
    };

    /// <summary>libdeflate: merge_new_observations().</summary>
    inline void MergeNewObservations(SplitStats& s) noexcept
    {
        for (std::uint32_t i = 0; i < NumObservationTypes; ++i) {
            s.observations[i] += s.newObservations[i];
            s.newObservations[i] = 0;
        }
        s.numObservations += s.numNewObservations;
        s.numNewObservations = 0;
    }

    /// <summary>
    /// libdeflate: do_end_block_check(). True if the new observations differ enough from the block so far to end the
    /// block here; otherwise merges them into the block's.
    /// </summary>
    inline bool DoEndBlockCheck(SplitStats& s, std::uint32_t blockLength) noexcept
    {
        if (s.numObservations > 0) {
            // Sum of absolute differences of the observation probabilities, scaled by both counts.
            std::uint32_t totalDelta = 0;
            for (std::uint32_t i = 0; i < NumObservationTypes; ++i) {
                const std::uint32_t expected = s.observations[i] * s.numNewObservations;
                const std::uint32_t actual   = s.newObservations[i] * s.numObservations;
                totalDelta += actual > expected ? actual - expected : expected - actual;
            }
            const std::uint32_t numItems = s.numObservations + s.numNewObservations;
            // Ends the block if the delta is above 200/512 (about 39 %), more easily for short blocks.
            std::uint32_t cutoff = s.numNewObservations * 200 / 512 * s.numObservations;
            if (blockLength < 10000 && numItems < 8192)
                cutoff += static_cast<std::uint32_t>(static_cast<std::uint64_t>(cutoff) * (8192 - numItems) / 8192);
            if (totalDelta + (blockLength / 4096) * s.numObservations >= cutoff)
                return true;
        }
        MergeNewObservations(s);
        return false;
    }

    /// <summary>libdeflate: ready_to_check_block(), the precondition part of should_end_block(): enough new
    /// observations, and neither the block so far nor the rest of the input shorter than MinBlockLength.</summary>
    inline bool EndBlockCheckDue(const SplitStats& s, std::size_t blockLength, std::size_t remaining) noexcept
    {
        return s.numNewObservations >= ObservationsPerBlockCheck && blockLength >= MinBlockLength && remaining >= MinBlockLength;
    }

    /// <summary>libdeflate: choose_min_match_len(). Minimum match length for greedy/lazy parsing from the number of
    /// distinct literals: data with few literals (e.g. DNA) gains from longer minimum matches.</summary>
    inline std::uint32_t ChooseMinMatchLen(std::uint32_t numUsedLiterals, std::uint32_t maxSearchDepth) noexcept
    {
        static constexpr std::uint8_t MinLens[] = {
            9, 9, 9, 9, 9, 9, 8, 8, 7, 7, 6, 6, 6, 6, 6, 6, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
            5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        }; // the rest is 3

        if (numUsedLiterals >= sizeof(MinLens))
            return 3;
        std::uint32_t minLen = MinLens[numUsedLiterals];
        if (maxSearchDepth < 16) {
            const std::uint32_t cap = maxSearchDepth < 5 ? 4 : maxSearchDepth < 10 ? 5 : 7;
            if (minLen > cap)
                minLen = cap;
        }
        return minLen;
    }

    /// <summary>libdeflate: calculate_min_match_len(). Initial minimum match length of a block from the distinct bytes
    /// of its first 4 KiB.</summary>
    inline std::uint32_t CalculateMinMatchLen(const std::uint8_t* data, std::size_t dataLen, std::uint32_t maxSearchDepth) noexcept
    {
        if (dataLen < 512)
            return MinMatchLen;
        if (dataLen > 4096)
            dataLen = 4096;
        std::uint8_t used[256] = {};
        for (std::size_t i = 0; i < dataLen; ++i)
            used[data[i]] = 1;
        std::uint32_t numUsed = 0;
        for (std::uint32_t i = 0; i < 256; ++i)
            numUsed += used[i];
        return ChooseMinMatchLen(numUsed, maxSearchDepth);
    }

    /// <summary>libdeflate: recalculate_min_match_len(). Minimum match length recalculated from the literal frequencies
    /// of the block so far.</summary>
    inline std::uint32_t RecalculateMinMatchLen(const std::uint32_t* litlenFreqs, std::uint32_t maxSearchDepth) noexcept
    {
        std::uint32_t literalFreq = 0;
        for (std::uint32_t i = 0; i < NumLiterals; ++i)
            literalFreq += litlenFreqs[i];
        const std::uint32_t cutoff  = literalFreq >> 10; // ignore literals used very rarely
        std::uint32_t       numUsed = 0;
        for (std::uint32_t i = 0; i < NumLiterals; ++i)
            if (litlenFreqs[i] > cutoff)
                ++numUsed;
        return ChooseMinMatchLen(numUsed, maxSearchDepth);
    }

    /// <summary>libdeflate: choose_max_block_end(). End of a block starting at blockBegin: the soft maximum, or the input
    /// end if less than MinBlockLength would remain after it.</summary>
    inline std::size_t ChooseMaxBlockEnd(std::size_t blockBegin, std::size_t inSize, std::size_t softMaxLen) noexcept
    {
        if (inSize - blockBegin < softMaxLen + MinBlockLength)
            return inSize;
        return blockBegin + softMaxLen;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
