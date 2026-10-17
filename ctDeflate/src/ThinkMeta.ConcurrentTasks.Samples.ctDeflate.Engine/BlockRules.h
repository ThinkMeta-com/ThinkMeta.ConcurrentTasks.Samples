//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include "DeflateTables.h"

#include <cstddef>
#include <cstdint>

// Block rules shared by the sequential compressor and the block tracker (libdeflate 1.26 deflate_compress.c).

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>Minimum match length for greedy/lazy parsing from the number of distinct literals: data with few
    /// literals (e.g. DNA) gains from longer minimum matches.</summary>
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

    /// <summary>Initial minimum match length of a block from the distinct bytes of its first 4 KiB.</summary>
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

    /// <summary>Minimum match length recalculated from the literal frequencies of the block so far.</summary>
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

    /// <summary>End of a block starting at blockBegin: the soft maximum, or the input end if less than MinBlockLength
    /// would remain after it.</summary>
    inline std::size_t ChooseMaxBlockEnd(std::size_t blockBegin, std::size_t inSize, std::size_t softMaxLen) noexcept
    {
        if (inSize - blockBegin < softMaxLen + MinBlockLength)
            return inSize;
        return blockBegin + softMaxLen;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
