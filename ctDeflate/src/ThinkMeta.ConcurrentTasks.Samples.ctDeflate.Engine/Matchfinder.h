//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <immintrin.h>
#include <intrin.h>

// Matchfinders of libdeflate 1.26 (lib/matchfinder_common.h, hc_matchfinder.h, ht_matchfinder.h).
// Positions are stored as 16-bit values relative to a base pointer that advances in steps of the 32 KiB window.

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    using MfPos = std::int16_t;

    constexpr std::uint32_t MfWindowSize = 32768;
    constexpr MfPos         MfInitVal    = static_cast<MfPos>(-32768);

    inline std::uint32_t LoadU32(const std::uint8_t* p) noexcept
    {
        std::uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    inline std::uint64_t LoadU64(const std::uint8_t* p) noexcept
    {
        std::uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    /// <summary>Prefetch for writing (the hash buckets are updated next), as libdeflate's prefetchw().</summary>
    __forceinline void PrefetchW(const void* p) noexcept
    {
        _m_prefetchw(p);
    }

    inline std::uint32_t LzHash(std::uint32_t seq, std::uint32_t numBits) noexcept
    {
        return (seq * 0x1E35A7BDu) >> (32 - numBits);
    }

    /// <summary>Length of the common prefix of strPtr and matchPtr, starting at startLen, at most maxLen.</summary>
    __forceinline std::uint32_t LzExtend(const std::uint8_t* strPtr, const std::uint8_t* matchPtr, std::uint32_t startLen, std::uint32_t maxLen) noexcept
    {
        std::uint32_t len = startLen;
        if (maxLen - len >= 32) {
            // Four words without the bounds check, as libdeflate does.
            for (std::int32_t i = 0; i < 4; ++i) {
                const std::uint64_t v = LoadU64(matchPtr + len) ^ LoadU64(strPtr + len);
                if (v != 0)
                    return len + (static_cast<std::uint32_t>(std::countr_zero(v)) >> 3);
                len += 8;
            }
        }
        while (len + 8 <= maxLen) {
            const std::uint64_t v = LoadU64(matchPtr + len) ^ LoadU64(strPtr + len);
            if (v != 0)
                return len + (static_cast<std::uint32_t>(std::countr_zero(v)) >> 3);
            len += 8;
        }
        while (len < maxLen && matchPtr[len] == strPtr[len])
            ++len;
        return len;
    }

    inline void MatchfinderInit(MfPos* data, std::size_t count) noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
            data[i] = MfInitVal;
    }

    /// <summary>Subtracts the window size with signed saturation: negative entries become MfInitVal.</summary>
    inline void MatchfinderRebase(MfPos* data, std::size_t count) noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
            data[i] = static_cast<MfPos>(0x8000 | (data[i] & ~(data[i] >> 15)));
    }

    /// <summary>Hash chains matchfinder (greedy, lazy, lazy2): a singleton bucket for length-3 matches and linked lists
    /// for length-4+ matches.</summary>
    struct HcMatchfinder {
        static constexpr std::uint32_t Hash3Order = 15;
        static constexpr std::uint32_t Hash4Order = 16;

        alignas(32) MfPos hash3Tab[1u << Hash3Order];
        MfPos hash4Tab[1u << Hash4Order];
        MfPos nextTab[MfWindowSize];

        void Init() noexcept { MatchfinderInit(hash3Tab, (1u << Hash3Order) + (1u << Hash4Order)); }

        void SlideWindow() noexcept { MatchfinderRebase(hash3Tab, (1u << Hash3Order) + (1u << Hash4Order) + MfWindowSize); }

        /// <summary>
        /// Inserts in_next and returns the longest match longer than bestLen (else bestLen), searching at most
        /// maxSearchDepth list nodes and stopping at niceLen. nextHashes holds the hashes of in_next and receives those of
        /// in_next + 1. Nothing is inserted if maxLen &lt; 5.
        /// </summary>
        __forceinline std::uint32_t LongestMatch(const std::uint8_t** inBaseP, const std::uint8_t* inNext, std::uint32_t bestLen, std::uint32_t maxLen,
                                                 std::uint32_t niceLen, std::uint32_t maxSearchDepth, std::uint32_t* nextHashes,
                                                 std::uint32_t* offsetRet) noexcept
        {
            std::uint32_t       depthRemaining = maxSearchDepth;
            const std::uint8_t* bestMatchPtr   = inNext;
            auto                curPos         = static_cast<std::uint32_t>(inNext - *inBaseP);

            if (curPos == MfWindowSize) {
                SlideWindow();
                *inBaseP += MfWindowSize;
                curPos = 0;
            }

            const std::uint8_t* inBase = *inBaseP;
            const auto          cutoff = static_cast<MfPos>(curPos - MfWindowSize);
            const std::uint8_t* matchPtr;
            std::uint32_t       len;
            std::uint32_t       seq4;
            MfPos               curNode3;
            MfPos               curNode4;

            if (maxLen < 5)
                goto out;

            {
                const std::uint32_t hash3 = nextHashes[0];
                const std::uint32_t hash4 = nextHashes[1];
                curNode3                  = hash3Tab[hash3];
                curNode4                  = hash4Tab[hash4];
                hash3Tab[hash3]           = static_cast<MfPos>(curPos);
                hash4Tab[hash4]           = static_cast<MfPos>(curPos);
                nextTab[curPos]           = curNode4;

                const std::uint32_t nextSeq = LoadU32(inNext + 1);
                nextHashes[0]               = LzHash(nextSeq & 0xFFFFFF, Hash3Order);
                nextHashes[1]               = LzHash(nextSeq, Hash4Order);
                PrefetchW(&hash3Tab[nextHashes[0]]);
                PrefetchW(&hash4Tab[nextHashes[1]]);
            }

            if (bestLen < 4) {
                if (curNode3 <= cutoff)
                    goto out;
                seq4 = LoadU32(inNext);
                if (bestLen < 3) {
                    matchPtr = inBase + curNode3;
                    if ((LoadU32(matchPtr) & 0xFFFFFF) == (seq4 & 0xFFFFFF)) {
                        bestLen      = 3;
                        bestMatchPtr = matchPtr;
                    }
                }
                if (curNode4 <= cutoff)
                    goto out;
                for (;;) {
                    matchPtr = inBase + curNode4;
                    if (LoadU32(matchPtr) == seq4)
                        break;
                    curNode4 = nextTab[static_cast<std::uint16_t>(curNode4) & (MfWindowSize - 1)];
                    if (curNode4 <= cutoff || --depthRemaining == 0)
                        goto out;
                }
                bestMatchPtr = matchPtr;
                bestLen      = LzExtend(inNext, bestMatchPtr, 4, maxLen);
                if (bestLen >= niceLen)
                    goto out;
                curNode4 = nextTab[static_cast<std::uint16_t>(curNode4) & (MfWindowSize - 1)];
                if (curNode4 <= cutoff || --depthRemaining == 0)
                    goto out;
            } else if (curNode4 <= cutoff || bestLen >= niceLen) {
                goto out;
            }

            for (;;) {
                for (;;) {
                    matchPtr = inBase + curNode4;
                    if (LoadU32(matchPtr + bestLen - 3) == LoadU32(inNext + bestLen - 3) && LoadU32(matchPtr) == LoadU32(inNext))
                        break;
                    curNode4 = nextTab[static_cast<std::uint16_t>(curNode4) & (MfWindowSize - 1)];
                    if (curNode4 <= cutoff || --depthRemaining == 0)
                        goto out;
                }
                len = LzExtend(inNext, matchPtr, 4, maxLen);
                if (len > bestLen) {
                    bestLen      = len;
                    bestMatchPtr = matchPtr;
                    if (bestLen >= niceLen)
                        goto out;
                }
                curNode4 = nextTab[static_cast<std::uint16_t>(curNode4) & (MfWindowSize - 1)];
                if (curNode4 <= cutoff || --depthRemaining == 0)
                    goto out;
            }
        out:
            *offsetRet = static_cast<std::uint32_t>(inNext - bestMatchPtr);
            return bestLen;
        }

        /// <summary>Inserts count positions from inNext without searching; nothing if fewer than count + 5 bytes remain.</summary>
        __forceinline void SkipBytes(const std::uint8_t** inBaseP, const std::uint8_t* inNext, const std::uint8_t* inEnd, std::uint32_t count,
                                     std::uint32_t* nextHashes) noexcept
        {
            if (count + 5 > static_cast<std::size_t>(inEnd - inNext))
                return;

            auto          curPos    = static_cast<std::uint32_t>(inNext - *inBaseP);
            std::uint32_t hash3     = nextHashes[0];
            std::uint32_t hash4     = nextHashes[1];
            std::uint32_t remaining = count;
            do {
                if (curPos == MfWindowSize) {
                    SlideWindow();
                    *inBaseP += MfWindowSize;
                    curPos = 0;
                }
                hash3Tab[hash3] = static_cast<MfPos>(curPos);
                nextTab[curPos] = hash4Tab[hash4];
                hash4Tab[hash4] = static_cast<MfPos>(curPos);

                const std::uint32_t nextSeq = LoadU32(++inNext);
                hash3                       = LzHash(nextSeq & 0xFFFFFF, Hash3Order);
                hash4                       = LzHash(nextSeq, Hash4Order);
                ++curPos;
            } while (--remaining != 0);

            PrefetchW(&hash3Tab[hash3]);
            PrefetchW(&hash4Tab[hash4]);
            nextHashes[0] = hash3;
            nextHashes[1] = hash4;
        }
    };

    /// <summary>Hash table matchfinder (level 1): buckets of two positions, no chains.</summary>
    struct HtMatchfinder {
        static constexpr std::uint32_t HashOrder     = 15;
        static constexpr std::uint32_t BucketSize    = 2;
        static constexpr std::uint32_t RequiredBytes = 5;

        alignas(32) MfPos hashTab[1u << HashOrder][BucketSize];

        void Init() noexcept { MatchfinderInit(&hashTab[0][0], (1u << HashOrder) * BucketSize); }

        void SlideWindow() noexcept { MatchfinderRebase(&hashTab[0][0], (1u << HashOrder) * BucketSize); }

        /// <summary>Inserts inNext and returns the longest match of length &gt;= 4 among the bucket entries (0 if none).
        /// maxLen must be &gt;= 5.</summary>
        __forceinline std::uint32_t LongestMatch(const std::uint8_t** inBaseP, const std::uint8_t* inNext, std::uint32_t maxLen, std::uint32_t niceLen,
                                                 std::uint32_t* nextHash, std::uint32_t* offsetRet) noexcept
        {
            std::uint32_t       bestLen      = 0;
            const std::uint8_t* bestMatchPtr = inNext;
            auto                curPos       = static_cast<std::uint32_t>(inNext - *inBaseP);

            if (curPos == MfWindowSize) {
                SlideWindow();
                *inBaseP += MfWindowSize;
                curPos = 0;
            }
            const std::uint8_t* inBase = *inBaseP;
            const auto          cutoff = static_cast<MfPos>(curPos - MfWindowSize);

            const std::uint32_t hash = *nextHash;
            *nextHash                = LzHash(LoadU32(inNext + 1), HashOrder);
            const std::uint32_t seq  = LoadU32(inNext);
            PrefetchW(&hashTab[*nextHash]);

            // Bucket of two, hand-unrolled as in libdeflate: the first entry moves to the second slot even if it
            // already reaches niceLen.
            MfPos curNode    = hashTab[hash][0];
            hashTab[hash][0] = static_cast<MfPos>(curPos);
            if (curNode <= cutoff)
                goto out;
            {
                const std::uint8_t* matchPtr = inBase + curNode;
                const MfPos         toInsert = curNode;
                curNode                      = hashTab[hash][1];
                hashTab[hash][1]             = toInsert;

                if (LoadU32(matchPtr) == seq) {
                    bestLen      = LzExtend(inNext, matchPtr, 4, maxLen);
                    bestMatchPtr = matchPtr;
                    if (curNode <= cutoff || bestLen >= niceLen)
                        goto out;
                    matchPtr = inBase + curNode;
                    if (LoadU32(matchPtr) == seq && LoadU32(matchPtr + bestLen - 3) == LoadU32(inNext + bestLen - 3)) {
                        const std::uint32_t len = LzExtend(inNext, matchPtr, 4, maxLen);
                        if (len > bestLen) {
                            bestLen      = len;
                            bestMatchPtr = matchPtr;
                        }
                    }
                } else {
                    if (curNode <= cutoff)
                        goto out;
                    matchPtr = inBase + curNode;
                    if (LoadU32(matchPtr) == seq) {
                        bestLen      = LzExtend(inNext, matchPtr, 4, maxLen);
                        bestMatchPtr = matchPtr;
                    }
                }
            }
        out:
            *offsetRet = static_cast<std::uint32_t>(inNext - bestMatchPtr);
            return bestLen;
        }

        /// <summary>
        /// Inserts count positions from inNext; nothing if fewer than count + 5 bytes remain. Slides the window up front if
        /// the range reaches the window end (that drops still valid negative entries, as in libdeflate).
        /// </summary>
        __forceinline void SkipBytes(const std::uint8_t** inBaseP, const std::uint8_t* inNext, const std::uint8_t* inEnd, std::uint32_t count,
                                     std::uint32_t* nextHash) noexcept
        {
            if (count + RequiredBytes > static_cast<std::size_t>(inEnd - inNext))
                return;

            auto curPos = static_cast<std::int32_t>(inNext - *inBaseP);
            if (curPos + static_cast<std::int32_t>(count) - 1 >= static_cast<std::int32_t>(MfWindowSize)) {
                SlideWindow();
                *inBaseP += MfWindowSize;
                curPos -= MfWindowSize;
            }

            std::uint32_t hash      = *nextHash;
            std::uint32_t remaining = count;
            do {
                hashTab[hash][1] = hashTab[hash][0];
                hashTab[hash][0] = static_cast<MfPos>(curPos);
                hash             = LzHash(LoadU32(++inNext), HashOrder);
                ++curPos;
            } while (--remaining != 0);

            PrefetchW(&hashTab[hash]);
            *nextHash = hash;
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
