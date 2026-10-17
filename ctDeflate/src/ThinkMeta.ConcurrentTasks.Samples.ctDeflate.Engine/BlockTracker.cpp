//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#include "BlockTracker.h"

#include "BlockRules.h"

#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    LevelParams LevelParams::For(std::int32_t level) noexcept
    {
        static constexpr struct {
            ParseMode     mode;
            std::uint32_t depth;
            std::uint32_t nice;
        } Table[] = {
            {ParseMode::None, 0, 0},       {ParseMode::Fastest, 0, 32},  {ParseMode::Greedy, 6, 10},   {ParseMode::Greedy, 12, 14},
            {ParseMode::Greedy, 16, 30},   {ParseMode::Lazy, 16, 30},    {ParseMode::Lazy, 35, 65},    {ParseMode::Lazy, 100, 130},
            {ParseMode::Lazy2, 300, 258},  {ParseMode::Lazy2, 600, 258},
        };
        LevelParams p;
        p.mode               = Table[level].mode;
        p.maxSearchDepth     = Table[level].depth;
        p.niceMatchLength    = Table[level].nice;
        p.maxPassthroughSize = level == 0 ? SIZE_MAX : static_cast<std::size_t>(55 - level * 4);
        return p;
    }

    void BlockTracker::Init(const LevelParams& params, const std::uint8_t* in, std::size_t inSize) noexcept
    {
        params_           = params;
        in_               = in;
        inSize_           = inSize;
        gatherSplitStats_ = params.mode == ParseMode::Greedy || params.mode == ParseMode::Lazy || params.mode == ParseMode::Lazy2;
        countLiterals_    = params.mode == ParseMode::Lazy || params.mode == ParseMode::Lazy2;
        seqLimit_         = params.mode == ParseMode::Fastest ? FastSeqStoreLength : SeqStoreLength;
    }

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

    void BlockTracker::LiteralObservations(std::uint32_t* types) const noexcept
    {
        for (std::uint32_t t = 0; t < NumLiteralObservations; ++t)
            types[t] = 0;
        for (std::uint32_t lit = 0; lit < NumLiterals; ++lit)
            types[((lit >> 5) & 0x6) | (lit & 1)] += windowLiterals_[lit];
    }

    bool BlockTracker::ShouldEndBlock(std::size_t pos) noexcept
    {
        auto& s = split_;
        if (s.numNewObservations < ObservationsPerBlockCheck || pos - blockBegin_ < MinBlockLength || inSize_ - pos < MinBlockLength)
            return false;
        LiteralObservations(s.newObservations);

        const auto blockLength = static_cast<std::uint32_t>(pos - blockBegin_);
        if (s.numObservations > 0) {
            std::uint32_t totalDelta = 0;
            for (std::uint32_t i = 0; i < NumObservationTypes; ++i) {
                const std::uint32_t expected = s.observations[i] * s.numNewObservations;
                const std::uint32_t actual   = s.newObservations[i] * s.numObservations;
                totalDelta += actual > expected ? actual - expected : expected - actual;
            }
            const std::uint32_t numItems = s.numObservations + s.numNewObservations;
            std::uint32_t       cutoff   = s.numNewObservations * 200 / 512 * s.numObservations;
            if (blockLength < 10000 && numItems < 8192)
                cutoff += static_cast<std::uint32_t>(static_cast<std::uint64_t>(cutoff) * (8192 - numItems) / 8192);
            if (totalDelta + (blockLength / 4096) * s.numObservations >= cutoff)
                return true;
        }
        for (std::uint32_t i = 0; i < NumObservationTypes; ++i) {
            s.observations[i] += s.newObservations[i];
            s.newObservations[i] = 0;
        }
        s.numObservations += s.numNewObservations;
        s.numNewObservations = 0;
        if (countLiterals_)
            for (std::uint32_t i = 0; i < NumLiterals; ++i)
                literalCounts_[i] += windowLiterals_[i];
        std::memset(windowLiterals_, 0, sizeof(windowLiterals_));
        return false;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
