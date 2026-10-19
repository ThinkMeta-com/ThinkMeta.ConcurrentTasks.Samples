//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include "BlockRules.h"
#include "DeflateTables.h"
#include "ParseStep.h"

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    /// <summary>
    /// New in ctDeflate, not in libdeflate: the block logic split off the parse loop, so that the resolver can replay
    /// it over speculative parses. The block state machine of libdeflate's greedy/lazy/fastest compressors, apart from
    /// the parsing itself: the minimum match length (from the first 4 KiB of the block, recalculated from the literal
    /// frequencies in lazy mode), the block split statistics and the end-of-block decision. Driven one parser iteration
    /// at a time; positions are offsets into the input. It keeps only what these decisions need (the literal counts in
    /// lazy mode, the observations, the number of matches), not the symbol frequencies of the block.
    /// </summary>
    class BlockTracker
    {
    public:
        void Init(const LevelParams& params, const std::uint8_t* in, std::size_t inSize) noexcept;

        /// <summary>follows libdeflate: the block start of deflate_compress_fastest() / deflate_compress_greedy() /
        /// deflate_compress_lazy_generic() (deflate_compress.c). Starts a block at pos (resets the counts, computes the
        /// minimum match length).</summary>
        void BeginBlock(std::size_t pos) noexcept;

        /// <summary>follows libdeflate: the min_len recalculation in deflate_compress_lazy_generic() (deflate_compress.c).
        /// The minimum match length an iteration starting at pos uses. In lazy mode this recalculates it from the literal
        /// counts when pos reached the recalculation point; Commit applies that.</summary>
        std::uint32_t PeekMinLen(std::size_t pos, std::size_t* nextRecalc) const noexcept;
        void          CommitIterationStart(std::uint32_t minLen, std::size_t nextRecalc) noexcept
        {
            minLen_     = minLen;
            nextRecalc_ = nextRecalc;
        }
        std::uint32_t IterationStart(std::size_t pos) noexcept
        {
            std::size_t nextRecalc;
            const auto  minLen = PeekMinLen(pos, &nextRecalc);
            CommitIterationStart(minLen, nextRecalc);
            return minLen;
        }

        // Literals are counted per value since the last merge of the split statistics (windowLiterals_); their
        // observation types and the literal counts of the block are derived from that when needed.
        // follows libdeflate: observe_literal() (deflate_compress.c)
        void Literal(std::uint32_t literal) noexcept
        {
            if (gatherSplitStats_) {
                ++windowLiterals_[literal];
                ++split_.numNewObservations;
            }
        }

        /// <summary>follows libdeflate: observe_literal() (deflate_compress.c); count literals in a row, without the
        /// iteration checks between them.</summary>
        void Literals(const std::uint8_t* literals, std::size_t count) noexcept
        {
            if (gatherSplitStats_) {
                for (std::size_t i = 0; i < count; ++i)
                    ++windowLiterals_[literals[i]];
                split_.numNewObservations += static_cast<std::uint32_t>(count);
            }
        }

        // follows libdeflate: observe_match() (deflate_compress.c)
        void Match(std::uint32_t length) noexcept
        {
            if (gatherSplitStats_) {
                ++split_.newObservations[NumLiteralObservations + (length >= 9 ? 1 : 0)];
                ++split_.numNewObservations;
            }
            ++seqCount_;
        }

        /// <summary>
        /// Consumes iterations from steps[0..count) (litDone literal iterations of steps[0] already consumed) until pos
        /// reaches limit, the steps end, or an iteration end where the block decision is due (maximum block end, sequence
        /// limit, split check); the caller then calls ContinueBlock for that end. Returns the steps finished; litDone and
        /// pos advance. Consumes at least one iteration.
        /// </summary>
        std::size_t ConsumeSteps(const ParseStep* steps, std::size_t count, std::uint32_t& litDone, const std::uint8_t* in, std::size_t& pos,
                                 std::size_t limit) noexcept
        {
            if (limit <= pos)
                limit = pos + 1;
            std::size_t i = 0;
            while (i < count) {
                const ParseStep& s = steps[i];
                if (s.length == 0) {
                    // Single-literal iterations: up to the first end where a check can be due.
                    std::size_t k = s.literals - litDone;
                    std::size_t n = LiteralsUntilCheck(pos);
                    if (k > n)
                        k = n;
                    if (k > limit - pos)
                        k = limit - pos;
                    if (gatherSplitStats_) {
                        for (std::size_t j = 0; j < k; ++j)
                            ++windowLiterals_[in[pos + j]];
                        split_.numNewObservations += static_cast<std::uint32_t>(k);
                    }
                    pos += k;
                    litDone += static_cast<std::uint32_t>(k);
                    if (litDone == s.literals) {
                        litDone = 0;
                        ++i;
                    }
                } else {
                    if (gatherSplitStats_) {
                        for (std::uint32_t j = 0; j < s.literals; ++j)
                            ++windowLiterals_[in[pos + j]];
                        ++split_.newObservations[NumLiteralObservations + (s.length >= 9 ? 1 : 0)];
                        split_.numNewObservations += s.literals + 1;
                    }
                    ++seqCount_;
                    pos += s.literals + s.length;
                    ++i;
                }
                if (pos >= limit || pos >= maxBlockEnd_ || seqCount_ >= seqLimit_ || (gatherSplitStats_ && EndBlockCheckDue(split_, pos - blockBegin_, inSize_ - pos)))
                    break;
            }
            return i;
        }

        /// <summary>follows libdeflate: the block loop condition of deflate_compress_fastest() /
        /// deflate_compress_greedy() / deflate_compress_lazy_generic() (deflate_compress.c). The loop condition after
        /// an iteration ending at pos: true if the block continues.</summary>
        bool ContinueBlock(std::size_t pos) noexcept;

        /// <summary>
        /// How many single-literal iterations can follow from pos before an iteration end needs ContinueBlock (the block
        /// reaches its maximum end, or the split check becomes due): at least 1.
        /// </summary>
        std::size_t LiteralsUntilCheck(std::size_t pos) const noexcept;

        std::size_t        BlockBegin() const noexcept { return blockBegin_; }
        std::size_t        NextRecalc() const noexcept { return nextRecalc_; }
        std::uint32_t      MinLen() const noexcept { return minLen_; }
        const LevelParams& Params() const noexcept { return params_; }

    private:
        bool ShouldEndBlock(std::size_t pos) noexcept;
        /// <summary>follows libdeflate: observe_literal() (deflate_compress.c), for a whole window. The literal
        /// observation types of the current window (newObservations[0..7]).</summary>
        void LiteralObservations(std::uint32_t* types) const noexcept;

        LevelParams         params_{};
        const std::uint8_t* in_               = nullptr;
        std::size_t         inSize_           = 0;
        bool                gatherSplitStats_ = false;
        bool                countLiterals_    = false;
        std::uint32_t       seqLimit_         = 0;

        std::size_t   blockBegin_  = 0;
        std::size_t   maxBlockEnd_ = 0;
        std::size_t   nextRecalc_  = 0;
        std::uint32_t minLen_      = MinMatchLen;
        std::uint32_t seqCount_    = 0;
        std::uint32_t literalCounts_[NumLiterals]{};  // literals of the block before the current window (lazy)
        std::uint32_t windowLiterals_[NumLiterals]{}; // literals since the last merge of the split statistics
        SplitStats    split_{};                       // newObservations: only the match types are counted directly
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
