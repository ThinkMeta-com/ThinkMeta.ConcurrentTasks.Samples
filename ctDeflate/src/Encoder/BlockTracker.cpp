//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#include "BlockTracker.h"

#include "BlockRules.h"

#include <cstring>

// New in ctDeflate, not in libdeflate (see BlockTracker.h).

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    void BlockTracker::Init(const LevelParams& params, const std::uint8_t* in, std::size_t inSize) noexcept
    {
        params_           = params;
        in_               = in;
        inSize_           = inSize;
        gatherSplitStats_ = params.mode == ParseMode::Greedy || params.mode == ParseMode::Lazy || params.mode == ParseMode::Lazy2;
        countLiterals_    = params.mode == ParseMode::Lazy || params.mode == ParseMode::Lazy2;
        seqLimit_         = params.mode == ParseMode::Fastest ? FastSeqStoreLength : SeqStoreLength;
    }

    // follows libdeflate: the block start of deflate_compress_fastest() / deflate_compress_greedy() /
    // deflate_compress_lazy_generic() (deflate_compress.c): init_block_split_stats(), deflate_begin_sequences(),
    // choose_max_block_end(), calculate_min_match_len()
    void BlockTracker::BeginBlock(std::size_t pos) noexcept
    {
        blockBegin_ = pos;
        seqCount_   = 0;
        if (countLiterals_)
            std::memset(literalCounts_, 0, sizeof(literalCounts_));
        if (gatherSplitStats_)
            std::memset(windowLiterals_, 0, sizeof(windowLiterals_));
        std::memset(&split_, 0, sizeof(split_));
        if (params_.mode == ParseMode::Fastest) {
            maxBlockEnd_ = ChooseMaxBlockEnd(pos, inSize_, FastSoftMaxBlockLength);
            minLen_      = 0;
            nextRecalc_  = SIZE_MAX;
            return;
        }
        maxBlockEnd_ = ChooseMaxBlockEnd(pos, inSize_, SoftMaxBlockLength);
        minLen_      = CalculateMinMatchLen(in_ + pos, maxBlockEnd_ - pos, params_.maxSearchDepth);
        nextRecalc_  = params_.mode == ParseMode::Greedy ? SIZE_MAX : pos + (inSize_ - pos < 10000 ? inSize_ - pos : 10000);
    }

    // follows libdeflate: the min_len recalculation in deflate_compress_lazy_generic() (deflate_compress.c)
    std::uint32_t BlockTracker::PeekMinLen(std::size_t pos, std::size_t* nextRecalc) const noexcept
    {
        if (pos >= nextRecalc_) {
            const std::size_t toEnd      = inSize_ - nextRecalc_;
            const std::size_t blockSoFar = pos - blockBegin_;
            *nextRecalc                  = nextRecalc_ + (toEnd < blockSoFar ? toEnd : blockSoFar);
            std::uint32_t counts[NumLiterals];
            for (std::uint32_t i = 0; i < NumLiterals; ++i)
                counts[i] = literalCounts_[i] + windowLiterals_[i];
            return RecalculateMinMatchLen(counts, params_.maxSearchDepth);
        }
        *nextRecalc = nextRecalc_;
        return minLen_;
    }

    // follows libdeflate: the block loop condition of deflate_compress_fastest() / deflate_compress_greedy() /
    // deflate_compress_lazy_generic() (deflate_compress.c)
    bool BlockTracker::ContinueBlock(std::size_t pos) noexcept
    {
        return pos < maxBlockEnd_ && seqCount_ < seqLimit_ && (!gatherSplitStats_ || !ShouldEndBlock(pos));
    }

    std::size_t BlockTracker::LiteralsUntilCheck(std::size_t pos) const noexcept
    {
        std::size_t n = maxBlockEnd_ - pos;
        if (gatherSplitStats_) {
            // The split check is due once 512 new observations, 5000 block bytes and 5000 remaining bytes come together.
            std::size_t ready = split_.numNewObservations < ObservationsPerBlockCheck ? ObservationsPerBlockCheck - split_.numNewObservations : 1;
            if (pos + ready < blockBegin_ + MinBlockLength)
                ready = blockBegin_ + MinBlockLength - pos;
            if (pos + ready + MinBlockLength <= inSize_ && ready < n)
                n = ready;
        }
        return n != 0 ? n : 1;
    }

    // follows libdeflate: observe_literal() (deflate_compress.c)
    void BlockTracker::LiteralObservations(std::uint32_t* types) const noexcept
    {
        for (std::uint32_t t = 0; t < NumLiteralObservations; ++t)
            types[t] = 0;
        for (std::uint32_t lit = 0; lit < NumLiterals; ++lit)
            types[((lit >> 5) & 0x6) | (lit & 1)] += windowLiterals_[lit];
    }

    // libdeflate: should_end_block() (deflate_compress.c), with the literal observations derived from the literal
    // counts of the window
    bool BlockTracker::ShouldEndBlock(std::size_t pos) noexcept
    {
        if (!EndBlockCheckDue(split_, pos - blockBegin_, inSize_ - pos))
            return false;
        LiteralObservations(split_.newObservations);
        if (DoEndBlockCheck(split_, static_cast<std::uint32_t>(pos - blockBegin_)))
            return true;
        // the observations are merged; the window's literals go to the block's literal counts
        if (countLiterals_)
            for (std::uint32_t i = 0; i < NumLiterals; ++i)
                literalCounts_[i] += windowLiterals_[i];
        std::memset(windowLiterals_, 0, sizeof(windowLiterals_));
        return false;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
