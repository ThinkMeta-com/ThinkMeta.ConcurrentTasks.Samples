//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include "BlockTracker.h"
#include "ParseStep.h"
#include "Matchfinder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// New in ctDeflate, not in libdeflate: a single-path parse that starts anywhere (speculative chunk parses, exact
// re-parses of the resolver) and records its iterations in a StepLog.

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>The minimum match length used by the iterations from pos on.</summary>
    struct MinLenEvent {
        std::size_t   pos;
        std::uint32_t minLen;
    };

    /// <summary>A range of iterations in storage owned elsewhere.</summary>
    struct StepSpan {
        const ParseStep* steps = nullptr;
        std::size_t      count = 0;
        std::size_t      begin = 0; // first iteration start
        std::size_t      end   = 0; // position after the last iteration
    };

    /// <summary>The iterations a parser produced for one input range.</summary>
    struct StepLog {
        std::size_t              begin = 0; // first iteration start
        std::size_t              end   = 0; // position after the last iteration
        std::vector<ParseStep>   steps;
        std::vector<MinLenEvent> minLens;

        void Reset(std::size_t pos)
        {
            begin = end = pos;
            steps.clear();
            minLens.clear();
        }

        StepSpan Span() const noexcept { return {steps.data(), steps.size(), begin, end}; }
    };

    /// <summary>
    /// New in ctDeflate, not in libdeflate: a resumable parse; follows libdeflate: deflate_compress_fastest() /
    /// deflate_compress_greedy() / deflate_compress_lazy_generic() (deflate_compress.c).
    /// libdeflate's fastest/greedy/lazy/lazy2 parse loop, resumable at every iteration and startable anywhere:
    /// speculatively (a new block assumed at the start position) or exactly (from a known block state). The
    /// matchfinder is rebuilt from the 32 KiB before the start; its state only depends on that data.
    /// </summary>
    class Parser
    {
    public:
        void Init(const LevelParams& params, const std::uint8_t* in, std::size_t inSize);

        /// <summary>Starts at pos as if a block began there.</summary>
        void StartSpeculative(std::size_t pos) noexcept;
        /// <summary>Starts at pos with the given exact block state (taken before the iteration start at pos).</summary>
        void StartExact(std::size_t pos, const BlockTracker& tracker) noexcept;

        /// <summary>Parses iterations until the position reaches limit or the input end and appends them to log.</summary>
        void Run(std::size_t limit, StepLog& log) noexcept;

        std::size_t Pos() const noexcept { return pos_; }

    private:
        void WarmUp(std::size_t pos) noexcept;
        void IterationFastest() noexcept;
        void IterationGreedy(std::uint32_t minLen) noexcept;
        void IterationLazy(std::uint32_t minLen) noexcept;

        void EmitLiteral() noexcept; // an iteration of one literal
        void EmitChainLiteral(std::size_t pos) noexcept;
        void EmitMatch(std::uint32_t length, std::uint32_t offset) noexcept;

        // follows libdeflate: adjust_max_and_nice_len() (deflate_compress.c)
        void AdjustMaxAndNiceLen() noexcept
        {
            const std::size_t remaining = inSize_ - pos_;
            if (remaining < MaxMatchLen) {
                maxLen_ = static_cast<std::uint32_t>(remaining);
                if (niceLen_ > maxLen_)
                    niceLen_ = maxLen_;
            }
        }

        LevelParams                    params_{};
        const std::uint8_t*            in_     = nullptr;
        std::size_t                    inSize_ = 0;
        std::unique_ptr<HcMatchfinder> hc_;
        std::unique_ptr<HtMatchfinder> ht_;
        BlockTracker                   tracker_;

        const std::uint8_t* inBase_        = nullptr;
        std::size_t         pos_           = 0;
        std::uint32_t       maxLen_        = MaxMatchLen;
        std::uint32_t       niceLen_       = 0;
        std::uint32_t       nextHashes_[2] = {0, 0};
        std::uint32_t       chain_         = 0;
        StepLog*            log_           = nullptr;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
