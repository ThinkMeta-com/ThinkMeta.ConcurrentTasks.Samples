//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include "Matchfinder.h"

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>
    /// libdeflate's hash chains matchfinder with insertion separated from the search: positions are inserted strictly in
    /// input order, and a search at pos runs after all positions before pos and none from pos on are inserted. Several
    /// searches at the same position (several parse paths) then see exactly the state libdeflate's
    /// hc_matchfinder_longest_match() sees there. Same tables, window slides and results as HcMatchfinder, except in the
    /// last bytes of the input, where libdeflate skips inserting positions depending on the parse.
    /// </summary>
    class OrderedMatchfinder
    {
    public:
        /// <summary>Empty, with the next position to insert at from.</summary>
        void Init(const std::uint8_t* in, std::size_t inSize, std::size_t from) noexcept
        {
            in_       = in;
            inSize_   = inSize;
            inserted_ = from;
            base_     = from / MfWindowSize * MfWindowSize;
            mf_.Init();
            if (from + 4 <= inSize)
                Hashes(from, &nextH3_, &nextH4_);
        }

        /// <summary>Inserts the positions before end.</summary>
        void InsertUpTo(std::size_t end) noexcept
        {
            if (end + 4 > inSize_)
                end = inSize_ >= 4 ? inSize_ - 3 : 0;
            if (inserted_ >= end)
                return;
            std::uint32_t h3 = nextH3_;
            std::uint32_t h4 = nextH4_;
            while (inserted_ < end) {
                Slide(inserted_);
                // Up to the window end, and so that the next position's 4 bytes can be read.
                std::size_t stop = base_ + MfWindowSize;
                if (stop > end)
                    stop = end;
                if (stop + 4 > inSize_)
                    stop = inSize_ - 4;
                auto curPos = static_cast<std::uint32_t>(inserted_ - base_);
                for (; inserted_ < stop; ++inserted_, ++curPos) {
                    mf_.hash3Tab[h3]        = static_cast<MfPos>(curPos);
                    mf_.nextTab[curPos]     = mf_.hash4Tab[h4];
                    mf_.hash4Tab[h4]        = static_cast<MfPos>(curPos);
                    const std::uint32_t seq = LoadU32(in_ + inserted_ + 1);
                    h3                      = LzHash(seq & 0xFFFFFF, HcMatchfinder::Hash3Order);
                    h4                      = LzHash(seq, HcMatchfinder::Hash4Order);
                }
                if (inserted_ < end && inserted_ == stop && stop + 4 >= inSize_) {
                    // The last positions: the next one has no 4 bytes left.
                    Slide(inserted_);
                    curPos              = static_cast<std::uint32_t>(inserted_ - base_);
                    mf_.hash3Tab[h3]    = static_cast<MfPos>(curPos);
                    mf_.nextTab[curPos] = mf_.hash4Tab[h4];
                    mf_.hash4Tab[h4]    = static_cast<MfPos>(curPos);
                    ++inserted_;
                    Hashes(inserted_, &h3, &h4);
                }
            }
            nextH3_ = h3;
            nextH4_ = h4;
            PrefetchW(&mf_.hash3Tab[h3]);
            PrefetchW(&mf_.hash4Tab[h4]);
        }

        /// <summary>
        /// The search of hc_matchfinder_longest_match() at pos; exactly the positions before pos must be inserted. With
        /// insert (no other search at pos follows) it also inserts pos, as libdeflate does in the search.
        /// </summary>
        __forceinline std::uint32_t LongestMatch(std::size_t pos, std::uint32_t bestLen, std::uint32_t maxLen, std::uint32_t niceLen,
                                                 std::uint32_t maxSearchDepth, std::uint32_t* offsetRet, bool insert = false) noexcept
        {
            *offsetRet = 0;
            if (maxLen < 5)
                return bestLen;
            Slide(pos);

            const auto          curPos = static_cast<std::uint32_t>(pos - base_);
            const auto          cutoff = static_cast<MfPos>(curPos - MfWindowSize);
            const std::uint8_t* inBase = in_ + base_;
            const std::uint8_t* inNext = in_ + pos;
            // pos is the next position to insert, so its hashes are ready.
            std::uint32_t h3 = nextH3_;
            std::uint32_t h4 = nextH4_;
            if (inserted_ != pos)
                Hashes(pos, &h3, &h4);
            const MfPos curNode3 = mf_.hash3Tab[h3];
            MfPos       curNode4 = mf_.hash4Tab[h4];
            if (insert && inserted_ == pos && pos + 5 <= inSize_) {
                mf_.hash3Tab[h3]        = static_cast<MfPos>(curPos);
                mf_.hash4Tab[h4]        = static_cast<MfPos>(curPos);
                mf_.nextTab[curPos]     = curNode4;
                const std::uint32_t seq = LoadU32(in_ + ++inserted_);
                nextH3_                 = LzHash(seq & 0xFFFFFF, HcMatchfinder::Hash3Order);
                nextH4_                 = LzHash(seq, HcMatchfinder::Hash4Order);
                PrefetchW(&mf_.hash3Tab[nextH3_]);
                PrefetchW(&mf_.hash4Tab[nextH4_]);
            }

            std::uint32_t       depthRemaining = maxSearchDepth;
            const std::uint8_t* bestMatchPtr   = inNext;
            const std::uint8_t* matchPtr;

            if (bestLen < 4) {
                if (curNode3 <= cutoff)
                    goto out;
                const std::uint32_t seq4 = LoadU32(inNext);
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
                    curNode4 = mf_.nextTab[static_cast<std::uint16_t>(curNode4) & (MfWindowSize - 1)];
                    if (curNode4 <= cutoff || --depthRemaining == 0)
                        goto out;
                }
                bestMatchPtr = matchPtr;
                bestLen      = LzExtend(inNext, bestMatchPtr, 4, maxLen);
                if (bestLen >= niceLen)
                    goto out;
                curNode4 = mf_.nextTab[static_cast<std::uint16_t>(curNode4) & (MfWindowSize - 1)];
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
                    curNode4 = mf_.nextTab[static_cast<std::uint16_t>(curNode4) & (MfWindowSize - 1)];
                    if (curNode4 <= cutoff || --depthRemaining == 0)
                        goto out;
                }
                const std::uint32_t len = LzExtend(inNext, matchPtr, 4, maxLen);
                if (len > bestLen) {
                    bestLen      = len;
                    bestMatchPtr = matchPtr;
                    if (bestLen >= niceLen)
                        goto out;
                }
                curNode4 = mf_.nextTab[static_cast<std::uint16_t>(curNode4) & (MfWindowSize - 1)];
                if (curNode4 <= cutoff || --depthRemaining == 0)
                    goto out;
            }
        out:
            *offsetRet = static_cast<std::uint32_t>(inNext - bestMatchPtr);
            return bestLen;
        }

    private:
        /// <summary>libdeflate moves the window base when a position reaches a full window past it.</summary>
        void Slide(std::size_t pos) noexcept
        {
            if (pos - base_ == MfWindowSize) {
                mf_.SlideWindow();
                base_ += MfWindowSize;
            }
        }

        void Hashes(std::size_t pos, std::uint32_t* h3, std::uint32_t* h4) const noexcept
        {
            if (pos == 0) {
                // libdeflate starts with hash codes 0 instead of those of the first bytes.
                *h3 = 0;
                *h4 = 0;
                return;
            }
            if (pos + 4 > inSize_) {
                *h3 = 0;
                *h4 = 0;
                return;
            }
            const std::uint32_t seq = LoadU32(in_ + pos);
            *h3                     = LzHash(seq & 0xFFFFFF, HcMatchfinder::Hash3Order);
            *h4                     = LzHash(seq, HcMatchfinder::Hash4Order);
        }

        const std::uint8_t* in_       = nullptr;
        std::size_t         inSize_   = 0;
        std::size_t         inserted_ = 0;
        std::size_t         base_     = 0;
        std::uint32_t       nextH3_   = 0; // hashes of the position inserted_
        std::uint32_t       nextH4_   = 0;
        HcMatchfinder       mf_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
