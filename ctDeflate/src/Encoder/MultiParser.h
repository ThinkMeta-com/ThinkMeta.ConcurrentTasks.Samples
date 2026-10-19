//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include "BlockTracker.h"
#include "OrderedMatchfinder.h"
#include "Parser.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// New in ctDeflate, not in libdeflate: speculative parse of a chunk along several parse paths at once (one per minimum
// match length, plus the schedule path), recorded in a PathLog.

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>All minimum match lengths (the fastest parser has none).</summary>
    constexpr std::uint32_t AllMinLens = 0xFFFFFFFFu;

    /// <summary>Largest minimum match length libdeflate uses.</summary>
    constexpr std::uint32_t MaxMinLen = 9;

    /// <summary>
    /// The lane of the path that follows its own minimum match length schedule: the block logic run on that path from a
    /// block assumed to start at the chunk start (as a single speculative parse would). Its minimum match lengths are
    /// in lanes[ScheduleLane].minLens.
    /// </summary>
    constexpr std::uint32_t ScheduleLane = MaxMinLen + 1;
    constexpr std::uint32_t ScheduleBit  = 1u << ScheduleLane;

    /// <summary>Parse paths end this many bytes before the input end (libdeflate's insertions depend on the parse there).</summary>
    constexpr std::size_t PathTailStop = 1024;

    inline std::uint32_t MinLenBit(std::uint32_t minLen) noexcept
    {
        return 1u << minLen;
    }

    /// <summary>
    /// Parse paths of one input range: the path of the constant minimum match length m (bit m of mask) in lanes[m], the
    /// path with its own schedule (ScheduleBit) in lanes[ScheduleLane]. A single path that holds for every minimum match
    /// length (mask AllMinLens, the fastest parser and exact re-parses) is in lanes[0]. A path may end before the range
    /// end.
    /// </summary>
    struct PathLog {
        std::size_t   begin = 0;
        std::uint32_t mask  = 0;
        StepLog       lanes[ScheduleLane + 1];
        StepSpan      spans[ScheduleLane + 1]; // valid once the log is complete

        void Reset(std::size_t pos, std::uint32_t minLens)
        {
            begin = pos;
            mask  = minLens;
            for (auto& lane : lanes)
                lane.Reset(pos);
        }

        /// <summary>The path in lane (a minimum match length or ScheduleLane), or null if there is none.</summary>
        const StepSpan* Lane(std::uint32_t lane) const noexcept
        {
            if (mask == AllMinLens)
                return &spans[0];
            return lane <= ScheduleLane && (mask & (1u << lane)) != 0 ? &spans[lane] : nullptr;
        }

        /// <summary>Takes the span views (after the lanes stopped growing).</summary>
        void Finish() noexcept
        {
            for (std::uint32_t m = 0; m <= ScheduleLane; ++m)
                spans[m] = lanes[m].Span();
        }
    };

    /// <summary>
    /// New in ctDeflate, not in libdeflate: one parse for several minimum match lengths; follows libdeflate:
    /// deflate_compress_greedy() / deflate_compress_lazy_generic() (deflate_compress.c).
    /// libdeflate's greedy/lazy/lazy2 parse loop for several minimum match lengths at once. The first match search of an
    /// iteration does not depend on the minimum match length (searching with the smallest one gives the result for all),
    /// only the decision to take the match does; so the paths split only where that decision differs and join again
    /// where they meet at an iteration start. Each group of paths runs its iteration as a small state machine, one match
    /// search per step; the steps of all groups run in order of their search positions, so one OrderedMatchfinder serves
    /// them all with exactly libdeflate's matchfinder state. Each iteration is appended to the lanes of its group.
    /// </summary>
    class MultiParser
    {
    public:
        void Init(const LevelParams& params, const std::uint8_t* in, std::size_t inSize);

        /// <summary>Parses from pos until every path reaches limit, for the minimum match lengths in minLens (bits 3..9)
        /// and, with ScheduleBit, the path with its own schedule.</summary>
        void Run(std::size_t pos, std::size_t limit, std::uint32_t minLens, PathLog& out);

    private:
        /// <summary>A group of paths within an iteration: before its first search (stage 0) or in lazy evaluation of
        /// the match at matchPos (stage 1: search at matchPos + 1, stage 2: at matchPos + 2).</summary>
        struct Group {
            std::uint32_t mask;
            std::uint32_t stage;
            std::size_t   start;    // iteration start
            std::size_t   matchPos; // current match position (stages 1, 2)
            std::uint32_t curLen;
            std::uint32_t curOffset;
            std::uint32_t chain; // literals before the current match
            std::uint32_t nice;  // nice match length as last adjusted

            std::size_t SearchPos() const noexcept { return stage == 0 ? start : matchPos + stage; }
        };

        void Append(PathLog& out, std::uint32_t mask, std::size_t start, std::uint32_t literals, std::uint32_t length, std::uint32_t offset, std::size_t end);
        /// <summary>The group finished its iteration at end: it starts the next one there (joined with a group that
        /// already starts there).</summary>
        void          NextIteration(std::size_t index, std::size_t end);
        std::uint32_t MaxLenAt(std::size_t pos) const noexcept { return static_cast<std::uint32_t>(inSize_ - pos < MaxMatchLen ? inSize_ - pos : MaxMatchLen); }

        LevelParams                         params_{};
        const std::uint8_t*                 in_     = nullptr;
        std::size_t                         inSize_ = 0;
        std::unique_ptr<OrderedMatchfinder> mf_;
        std::vector<Group>                  groups_;
        BlockTracker                        schedule_; // the block logic of the schedule path
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
