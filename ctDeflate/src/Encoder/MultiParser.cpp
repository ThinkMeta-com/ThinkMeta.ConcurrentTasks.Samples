//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#include "MultiParser.h"

#include <bit>

// New in ctDeflate, not in libdeflate (see MultiParser.h).

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    namespace {
        /// <summary>Bits 0..length: the minimum match lengths that take a match of this length.</summary>
        std::uint32_t BitsUpTo(std::uint32_t length) noexcept
        {
            return length >= 31 ? AllMinLens : (2u << length) - 1;
        }
    } // namespace

    void MultiParser::Init(const LevelParams& params, const std::uint8_t* in, std::size_t inSize)
    {
        params_ = params;
        in_     = in;
        inSize_ = inSize;
        if (!mf_)
            mf_ = std::make_unique<OrderedMatchfinder>();
    }

    void MultiParser::Append(PathLog& out, std::uint32_t mask, std::size_t start, std::uint32_t literals, std::uint32_t length, std::uint32_t offset,
                             std::size_t end)
    {
        if ((mask & ScheduleBit) != 0) {
            // The schedule path drives its block logic like a single parse.
            if (length == 0) {
                schedule_.Literal(in_[start]);
            } else {
                schedule_.Literals(in_ + start, literals);
                schedule_.Match(length);
            }
            if (end < inSize_ && !schedule_.ContinueBlock(end))
                schedule_.BeginBlock(end);
        }
        for (std::uint32_t bits = mask & ((ScheduleBit << 1) - MinLenBit(MinMatchLen)); bits != 0; bits &= bits - 1) {
            StepLog& lane = out.lanes[std::countr_zero(bits)];
            if (length == 0) {
                if (!lane.steps.empty() && lane.steps.back().length == 0)
                    ++lane.steps.back().literals;
                else
                    lane.steps.push_back({1, 0, 0});
            } else {
                lane.steps.push_back({literals, static_cast<std::uint16_t>(length), static_cast<std::uint16_t>(offset)});
            }
            lane.end = end;
        }
    }

    void MultiParser::NextIteration(std::size_t index, std::size_t end)
    {
        if (groups_.size() == 1) {
            groups_[0].stage = 0;
            groups_[0].start = end;
            return;
        }
        for (std::size_t j = 0; j < groups_.size(); ++j) {
            if (j != index && groups_[j].stage == 0 && groups_[j].start == end) {
                groups_[j].mask |= groups_[index].mask;
                groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(index));
                return;
            }
        }
        groups_[index].stage = 0;
        groups_[index].start = end;
    }

    // follows libdeflate: deflate_compress_greedy() / deflate_compress_lazy_generic() (deflate_compress.c), per group of
    // paths
    void MultiParser::Run(std::size_t pos, std::size_t limit, std::uint32_t minLens, PathLog& out)
    {
        out.Reset(pos, minLens);
        // Near the input end libdeflate's insertions depend on the parse; the paths stop before.
        const std::size_t tailStop = inSize_ > PathTailStop ? inSize_ - PathTailStop : 0;
        if (limit > tailStop)
            limit = tailStop;
        if (pos >= limit || minLens == 0) {
            out.Finish();
            return;
        }

        // Room for a typical parse (several bytes per step), so that the lanes rarely grow and copy.
        for (std::uint32_t bits = minLens & ((ScheduleBit << 1) - MinLenBit(MinMatchLen)); bits != 0; bits &= bits - 1)
            out.lanes[std::countr_zero(bits)].steps.reserve((limit - pos) / 5 + 64);

        const std::size_t from = pos >= MfWindowSize ? pos - MfWindowSize : 0;
        mf_->Init(in_, inSize_, from);

        const bool          greedy = params_.mode == ParseMode::Greedy;
        const bool          lazy2  = params_.mode == ParseMode::Lazy2;
        const std::uint32_t depth  = params_.maxSearchDepth;
        groups_.clear();
        groups_.push_back({minLens, 0, pos, 0, 0, 0, 0, 0});
        if ((minLens & ScheduleBit) != 0) {
            schedule_.Init(params_, in_, inSize_);
            schedule_.BeginBlock(pos);
        }

        for (;;) {
            // The pending search with the lowest position.
            std::size_t gi = 0;
            if (groups_.size() > 1) {
                for (std::size_t i = 1; i < groups_.size(); ++i)
                    if (groups_[i].SearchPos() < groups_[gi].SearchPos())
                        gi = i;
            }
            Group&            g  = groups_[gi];
            const std::size_t sp = g.SearchPos();
            if (g.stage == 0 && g.start >= limit)
                break; // iterations still in lazy evaluation past the limit are dropped
            mf_->InsertUpTo(sp);
            // The last search at sp may insert sp, as libdeflate does within the search.
            bool lastAtSp = true;
            if (groups_.size() > 1) {
                for (std::size_t i = 0; i < groups_.size(); ++i)
                    if (i != gi && groups_[i].SearchPos() == sp)
                        lastAtSp = false;
            }

            const std::uint32_t maxLen = MaxLenAt(sp);
            if (g.stage == 0) {
                g.nice = params_.niceMatchLength < maxLen ? params_.niceMatchLength : maxLen;

                // The schedule path's minimum match length for this iteration.
                const std::uint32_t constMask = g.mask & ~ScheduleBit;
                std::uint32_t       lowest    = constMask != 0 ? static_cast<std::uint32_t>(std::countr_zero(constMask)) : MaxMinLen;
                std::uint32_t       schedMin  = 0;
                if ((g.mask & ScheduleBit) != 0) {
                    schedMin = schedule_.IterationStart(sp);
                    auto& ev = out.lanes[ScheduleLane].minLens;
                    if (ev.empty() || ev.back().minLen != schedMin)
                        ev.push_back({sp, schedMin});
                    if (schedMin < lowest)
                        lowest = schedMin;
                }
                std::uint32_t       offset;
                const std::uint32_t length = mf_->LongestMatch(sp, lowest - 1, maxLen, g.nice, depth, &offset, lastAtSp);

                const bool    takeable = greedy ? (length > MinMatchLen || offset <= 4096) : !(length == MinMatchLen && offset > 8192);
                std::uint32_t accMask  = 0;
                if (takeable) {
                    accMask = constMask & BitsUpTo(length);
                    if ((g.mask & ScheduleBit) != 0 && length >= schedMin)
                        accMask |= ScheduleBit;
                }
                const std::uint32_t litMask = g.mask & ~accMask;

                if (litMask != 0)
                    Append(out, litMask, sp, 0, 0, 0, sp + 1);
                if (accMask == 0) {
                    NextIteration(gi, sp + 1);
                    continue;
                }
                if (litMask != 0) {
                    // The paths that took the literal start their next iteration at sp + 1 (g may move: re-taken below).
                    bool joined = false;
                    for (Group& other : groups_)
                        if (other.stage == 0 && other.start == sp + 1) {
                            other.mask |= litMask;
                            joined = true;
                        }
                    if (!joined)
                        groups_.push_back({litMask, 0, sp + 1, 0, 0, 0, 0, 0});
                }
                Group& a = groups_[gi];
                a.mask   = accMask;
                if (greedy || length >= a.nice) {
                    Append(out, accMask, sp, 0, length, offset, sp + length);
                    NextIteration(gi, sp + length);
                } else {
                    a.stage     = 1;
                    a.matchPos  = sp;
                    a.curLen    = length;
                    a.curOffset = offset;
                    a.chain     = 0;
                }
                continue;
            }

            // Lazy evaluation: is the match one (stage 1) or two (stage 2) bytes later better?
            if (g.nice > maxLen)
                g.nice = maxLen;
            std::uint32_t       nextOffset;
            const std::uint32_t nextLen   = mf_->LongestMatch(sp, g.curLen - 1, maxLen, g.nice, g.stage == 1 ? depth >> 1 : depth >> 2, &nextOffset, lastAtSp);
            const std::int32_t  threshold = g.stage == 1 ? 2 : 6;
            if (nextLen >= g.curLen && 4 * static_cast<std::int32_t>(nextLen - g.curLen) + (Bsr32(g.curOffset) - Bsr32(nextOffset)) > threshold) {
                g.chain += g.stage; // the literals before the better match
                g.matchPos  = sp;
                g.curLen    = nextLen;
                g.curOffset = nextOffset;
                if (g.curLen >= g.nice) {
                    Append(out, g.mask, g.start, g.chain, g.curLen, g.curOffset, g.matchPos + g.curLen);
                    NextIteration(gi, g.matchPos + g.curLen);
                } else {
                    g.stage = 1;
                }
                continue;
            }
            if (g.stage == 1 && lazy2) {
                g.stage = 2;
                continue;
            }
            Append(out, g.mask, g.start, g.chain, g.curLen, g.curOffset, g.matchPos + g.curLen);
            NextIteration(gi, g.matchPos + g.curLen);
        }
        out.Finish();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
