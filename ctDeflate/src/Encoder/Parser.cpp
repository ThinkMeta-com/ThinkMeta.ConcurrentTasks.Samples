//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#include "Parser.h"

#include <bit>

// New in ctDeflate, not in libdeflate (see Parser.h).

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {


    void Parser::Init(const LevelParams& params, const std::uint8_t* in, std::size_t inSize)
    {
        params_ = params;
        in_     = in;
        inSize_ = inSize;
        tracker_.Init(params, in, inSize);
        if (params.mode == ParseMode::Fastest) {
            if (!ht_)
                ht_ = std::make_unique<HtMatchfinder>();
        } else if (params.mode != ParseMode::None) {
            if (!hc_)
                hc_ = std::make_unique<HcMatchfinder>();
        }
    }

    void Parser::StartSpeculative(std::size_t pos) noexcept
    {
        tracker_.BeginBlock(pos);
        WarmUp(pos);
    }

    void Parser::StartExact(std::size_t pos, const BlockTracker& tracker) noexcept
    {
        tracker_ = tracker;
        WarmUp(pos);
    }

    void Parser::WarmUp(std::size_t pos) noexcept
    {
        // The matchfinder state at pos only depends on the 32 KiB before it (and on the base, which libdeflate advances
        // in window steps from the start of the input). Entries further back are out of reach anyway.
        pos_     = pos;
        maxLen_  = MaxMatchLen;
        niceLen_ = params_.niceMatchLength < MaxMatchLen ? params_.niceMatchLength : MaxMatchLen;
        chain_   = 0;

        const std::size_t from = pos >= MfWindowSize ? pos - MfWindowSize : 0;
        inBase_                = in_ + from / MfWindowSize * MfWindowSize;

        if (params_.mode == ParseMode::Fastest) {
            ht_->Init();
            // At the input start libdeflate begins with hash 0 instead of the hash of the first bytes.
            std::uint32_t hash = from == 0 ? 0 : LzHash(LoadU32(in_ + from), HtMatchfinder::HashOrder);
            for (std::size_t p = from; p < pos; ++p) {
                auto curPos = static_cast<std::uint32_t>(in_ + p - inBase_);
                if (curPos == MfWindowSize) {
                    ht_->SlideWindow();
                    inBase_ += MfWindowSize;
                    curPos = 0;
                }
                ht_->hashTab[hash][1] = ht_->hashTab[hash][0];
                ht_->hashTab[hash][0] = static_cast<MfPos>(curPos);
                hash                  = LzHash(LoadU32(in_ + p + 1), HtMatchfinder::HashOrder);
            }
            nextHashes_[0] = hash;
        } else {
            hc_->Init();
            if (from == 0) {
                nextHashes_[0] = 0;
                nextHashes_[1] = 0;
            } else {
                const std::uint32_t seq = LoadU32(in_ + from);
                nextHashes_[0]          = LzHash(seq & 0xFFFFFF, HcMatchfinder::Hash3Order);
                nextHashes_[1]          = LzHash(seq, HcMatchfinder::Hash4Order);
            }
            if (pos > from)
                hc_->SkipBytes(&inBase_, in_ + from, in_ + inSize_, static_cast<std::uint32_t>(pos - from), nextHashes_);
        }
    }

    // follows libdeflate: the block loop of deflate_compress_fastest() / deflate_compress_greedy() /
    // deflate_compress_lazy_generic() (deflate_compress.c), without the block output
    void Parser::Run(std::size_t limit, StepLog& log) noexcept
    {
        log_ = &log;
        while (pos_ < limit && pos_ < inSize_) {
            const std::uint32_t minLen = tracker_.IterationStart(pos_);
            if (log.minLens.empty() || log.minLens.back().minLen != minLen)
                log.minLens.push_back({pos_, minLen});

            switch (params_.mode) {
                case ParseMode::Fastest: IterationFastest(); break;
                case ParseMode::Greedy: IterationGreedy(minLen); break;
                default: IterationLazy(minLen); break;
            }

            if (pos_ < inSize_ && !tracker_.ContinueBlock(pos_))
                tracker_.BeginBlock(pos_);
        }
        log.end = pos_;
    }

    void Parser::EmitLiteral() noexcept
    {
        tracker_.Literal(in_[pos_]);
        auto& steps = log_->steps;
        if (!steps.empty() && steps.back().length == 0)
            ++steps.back().literals;
        else
            steps.push_back({1, 0, 0});
        ++pos_;
    }

    void Parser::EmitChainLiteral(std::size_t pos) noexcept
    {
        tracker_.Literal(in_[pos]);
        ++chain_;
    }

    void Parser::EmitMatch(std::uint32_t length, std::uint32_t offset) noexcept
    {
        tracker_.Match(length);
        log_->steps.push_back({chain_, static_cast<std::uint16_t>(length), static_cast<std::uint16_t>(offset)});
        chain_ = 0;
    }

    // follows libdeflate: one iteration of deflate_compress_fastest() (deflate_compress.c)
    void Parser::IterationFastest() noexcept
    {
        const std::size_t remaining = inSize_ - pos_;
        if (remaining < MaxMatchLen) {
            maxLen_ = static_cast<std::uint32_t>(remaining);
            if (maxLen_ < HtMatchfinder::RequiredBytes) {
                EmitLiteral(); // libdeflate ends the input with literals here
                return;
            }
            if (niceLen_ > maxLen_)
                niceLen_ = maxLen_;
        }
        std::uint32_t       offset;
        const std::uint32_t length = ht_->LongestMatch(&inBase_, in_ + pos_, maxLen_, niceLen_, nextHashes_, &offset);
        if (length != 0) {
            EmitMatch(length, offset);
            ht_->SkipBytes(&inBase_, in_ + pos_ + 1, in_ + inSize_, length - 1, nextHashes_);
            pos_ += length;
        } else {
            EmitLiteral();
        }
    }

    // follows libdeflate: one iteration of deflate_compress_greedy() (deflate_compress.c)
    void Parser::IterationGreedy(std::uint32_t minLen) noexcept
    {
        AdjustMaxAndNiceLen();
        std::uint32_t       offset;
        const std::uint32_t length
            = hc_->LongestMatch(&inBase_, in_ + pos_, minLen - 1, maxLen_, niceLen_, params_.maxSearchDepth, nextHashes_, &offset);
        if (length >= minLen && (length > MinMatchLen || offset <= 4096)) {
            EmitMatch(length, offset);
            hc_->SkipBytes(&inBase_, in_ + pos_ + 1, in_ + inSize_, length - 1, nextHashes_);
            pos_ += length;
        } else {
            EmitLiteral();
        }
    }

    // follows libdeflate: one iteration of deflate_compress_lazy_generic() (deflate_compress.c)
    void Parser::IterationLazy(std::uint32_t minLen) noexcept
    {
        const bool         lazy2 = params_.mode == ParseMode::Lazy2;
        const std::uint8_t* inEnd = in_ + inSize_;
        std::uint32_t       curOffset;
        std::uint32_t       nextOffset;

        AdjustMaxAndNiceLen();
        std::uint32_t curLen = hc_->LongestMatch(&inBase_, in_ + pos_, minLen - 1, maxLen_, niceLen_, params_.maxSearchDepth, nextHashes_, &curOffset);
        if (curLen < minLen || (curLen == MinMatchLen && curOffset > 8192)) {
            EmitLiteral();
            return;
        }
        ++pos_;

        for (;;) {
            // A very long match is taken immediately.
            if (curLen >= niceLen_) {
                EmitMatch(curLen, curOffset);
                hc_->SkipBytes(&inBase_, in_ + pos_, inEnd, curLen - 1, nextHashes_);
                pos_ += curLen - 1;
                return;
            }

            // A better match at the next position? (half the search depth there)
            AdjustMaxAndNiceLen();
            std::uint32_t nextLen
                = hc_->LongestMatch(&inBase_, in_ + pos_, curLen - 1, maxLen_, niceLen_, params_.maxSearchDepth >> 1, nextHashes_, &nextOffset);
            ++pos_;
            if (nextLen >= curLen && 4 * static_cast<std::int32_t>(nextLen - curLen) + (Bsr32(curOffset) - Bsr32(nextOffset)) > 2) {
                EmitChainLiteral(pos_ - 2);
                curLen    = nextLen;
                curOffset = nextOffset;
                continue;
            }

            if (lazy2) {
                // Two positions ahead (a quarter of the search depth).
                AdjustMaxAndNiceLen();
                nextLen = hc_->LongestMatch(&inBase_, in_ + pos_, curLen - 1, maxLen_, niceLen_, params_.maxSearchDepth >> 2, nextHashes_, &nextOffset);
                ++pos_;
                if (nextLen >= curLen && 4 * static_cast<std::int32_t>(nextLen - curLen) + (Bsr32(curOffset) - Bsr32(nextOffset)) > 6) {
                    EmitChainLiteral(pos_ - 3);
                    EmitChainLiteral(pos_ - 2);
                    curLen    = nextLen;
                    curOffset = nextOffset;
                    continue;
                }
                EmitMatch(curLen, curOffset);
                if (curLen > 3) {
                    hc_->SkipBytes(&inBase_, in_ + pos_, inEnd, curLen - 3, nextHashes_);
                    pos_ += curLen - 3;
                }
            } else {
                EmitMatch(curLen, curOffset);
                hc_->SkipBytes(&inBase_, in_ + pos_, inEnd, curLen - 2, nextHashes_);
                pos_ += curLen - 2;
            }
            return;
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
