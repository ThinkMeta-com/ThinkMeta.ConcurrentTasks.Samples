//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#include "Resolver.h"

#include <atomic>
#include <cstring>
#include <utility>

// New in ctDeflate, not in libdeflate (see Resolver.h).

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    namespace {
        // Near the input end libdeflate skips inserting positions depending on the parse; there a chunk is never taken
        // over and a re-parse restarts from a snapshot taken before that zone.
        constexpr std::size_t TailGuard    = 512;
        constexpr std::size_t TailSnapshot = 1024;
        constexpr std::size_t RepairBatch  = 4096;
    } // namespace

    // ---- StepCursor ----

    void StepCursor::Attach(const StepSpan& log) noexcept
    {
        log_     = &log;
        step_    = 0;
        litDone_ = 0;
        pos_     = log.begin;
    }

    void StepCursor::Next() noexcept
    {
        const ParseStep& s = log_->steps[step_];
        if (s.length == 0) {
            ++pos_;
            if (++litDone_ == s.literals) {
                ++step_;
                litDone_ = 0;
            }
        } else {
            pos_ += s.literals + s.length;
            ++step_;
        }
    }

    void StepCursor::SkipLiterals(std::uint32_t count) noexcept
    {
        pos_ += count;
        litDone_ += count;
        if (litDone_ == log_->steps[step_].literals) {
            ++step_;
            litDone_ = 0;
        }
    }

    void StepCursor::AdvanceTo(std::size_t target) noexcept
    {
        while (!AtEnd() && pos_ < target) {
            const ParseStep& s = log_->steps[step_];
            if (s.length == 0) {
                const std::size_t left = s.literals - litDone_;
                if (pos_ + left <= target) {
                    pos_ += left;
                    ++step_;
                    litDone_ = 0;
                } else {
                    litDone_ += static_cast<std::uint32_t>(target - pos_);
                    pos_ = target;
                }
            } else {
                pos_ += s.literals + s.length;
                ++step_;
            }
        }
    }

    // ---- block symbols ----

    // follows libdeflate: deflate_begin_sequences(), deflate_choose_literal(), deflate_choose_match() (deflate_compress.c)
    void BuildBlockSymbols(const std::uint8_t* in, const ResolvedBlock& block, std::vector<DeflateSequence>& sequences, DeflateFreqs& freqs)
    {
        sequences.clear();
        sequences.push_back({0, 0, 0});
        std::memset(&freqs, 0, sizeof(freqs));
        for (const BlockPiece& piece : block.pieces) {
            StepCursor  c   = piece.start;
            std::size_t pos = c.Pos();
            while (pos < piece.end) {
                const ParseStep& s = c.Step();
                if (s.length == 0) {
                    std::uint32_t k = c.LiteralsLeft();
                    if (k > piece.end - pos)
                        k = static_cast<std::uint32_t>(piece.end - pos);
                    for (std::uint32_t i = 0; i < k; ++i)
                        ++freqs.litlen[in[pos + i]];
                    sequences.back().litrunlenAndLength += k;
                    pos += k;
                    c.SkipLiterals(k);
                    continue;
                }
                for (std::uint32_t i = 0; i < s.literals; ++i)
                    ++freqs.litlen[in[pos + i]];
                const std::uint32_t offsetSlot = OffsetSlot(s.offset);
                ++freqs.litlen[FirstLenSym + LengthSlot(s.length)];
                ++freqs.offset[offsetSlot];
                DeflateSequence& seq = sequences.back();
                seq.litrunlenAndLength += s.literals;
                seq.litrunlenAndLength |= static_cast<std::uint32_t>(s.length) << SeqLengthShift;
                seq.offset     = s.offset;
                seq.offsetSlot = static_cast<std::uint16_t>(offsetSlot);
                sequences.push_back({0, 0, 0});
                pos += s.literals + s.length;
                c.Next();
            }
        }
    }

    // ---- ChunkView ----

    ChunkView::Result ChunkView::Find(std::size_t pos, std::uint32_t minLen, PathCursor** found) noexcept
    {
        // The schedule lane first: where it holds, the constant lanes were not needed (see IChunkLogs::Release).
        const std::uint32_t candidates[2] = {log_->mask == AllMinLens ? 0u : ScheduleLane, minLen};
        const std::size_t   count         = log_->mask == AllMinLens ? 1 : 2;
        bool                exhausted     = true;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t lane = candidates[i];
            if (lane > ScheduleLane || log_->Lane(lane) == nullptr)
                continue;
            PathCursor& cursor = Lane(lane);
            cursor.AdvanceTo(pos);
            if (cursor.AtEnd())
                continue;
            exhausted = false;
            if (cursor.Pos() == pos && cursor.Matches(minLen)) {
                *found = &cursor;
                return Result::Found;
            }
        }
        return exhausted ? Result::Exhausted : Result::NotHere;
    }

    // ---- Resolver ----

    Resolver::Resolver(const LevelParams& params, const std::uint8_t* in, std::size_t inSize) : params_(params), in_(in), inSize_(inSize)
    {
        tracker_.Init(params, in, inSize);
        repairParser_.Init(params, in, inSize);
    }

    void Resolver::SwitchSource(const StepCursor& source)
    {
        if (pieceOpen_ && pieceStart_.Pos() < pos_)
            block_.pieces.push_back({pieceStart_, pos_});
        pieceStart_ = source;
        pieceOpen_  = true;
    }

    void Resolver::EmitBlock(std::size_t end, IBlockSink& sink)
    {
        if (pieceOpen_ && pieceStart_.Pos() < end)
            block_.pieces.push_back({pieceStart_, end});
        pieceOpen_ = false;
        if (recentMinLens_ != nullptr) {
            lastBlocks_[blockCount_++ % 16] = blockMinLens_;
            std::uint32_t recent            = 0;
            for (const std::uint32_t m : lastBlocks_)
                recent |= m;
            recentMinLens_->store(recent, std::memory_order_relaxed);
        }
        blockMinLens_  = 0;
        block_.length  = end - block_.begin;
        block_.isFinal = end == inSize_;
        sink.OnBlock(std::move(block_));
        block_       = ResolvedBlock{};
        block_.begin = end;
    }

    void Resolver::RefillRepair(IBlockSink& sink)
    {
        // Free the batches whose blocks are all in the output (never the current one), so the memory of the re-parses
        // stays bounded by the blocks in flight, like that of the chunk logs.
        const std::size_t appended = sink.AppendedTo();
        while (repairFreed_ + 1 < repairLogs_.size() && repairEnds_[repairFreed_] <= appended)
            repairLogs_[repairFreed_++].reset();

        auto path = std::make_unique<PathLog>();
        path->Reset(pos_, AllMinLens);
        repairParser_.Run(pos_ + RepairBatch, path->lanes[0]);
        path->Finish();
        repair_.Attach(*path, 0);
        repairEnds_.push_back(path->lanes[0].end);
        repairLogs_.push_back(std::move(path));
    }

    void Resolver::StartRepair(std::size_t pos, IBlockSink& sink)
    {
        ++stats_.repairs;
        if (pos + TailGuard > inSize_ && haveTailSnapshot_) {
            // Replay from the snapshot: the iterations up to pos are already known to be exact.
            repairParser_.StartExact(tailPos_, tailTracker_);
            StepLog discard;
            discard.Reset(tailPos_);
            repairParser_.Run(pos, discard);
        } else {
            repairParser_.StartExact(pos, tracker_);
        }
        RefillRepair(sink);
    }

    void Resolver::Consume(PathCursor& source, std::size_t limit, IBlockSink& sink)
    {
        do {
            std::size_t       count;
            std::uint32_t     litDone;
            const ParseStep*  steps = source.RemainingSteps(&count, &litDone);
            const std::size_t done  = tracker_.ConsumeSteps(steps, count, litDone, in_, pos_, limit);
            source.Advance(done, litDone, pos_);
            if (!source.AtEnd() && source.Log() != pieceStart_.Log())
                SwitchSource(source.Segment()); // the path continues in another segment
            if (pos_ < inSize_ && !tracker_.ContinueBlock(pos_)) {
                EmitBlock(pos_, sink);
                tracker_.BeginBlock(pos_);
                return;
            }
        } while (pos_ < limit && pos_ < inSize_ && !source.AtEnd());
    }

    ResolveStats Resolver::Run(const std::vector<std::size_t>& chunkStarts, IChunkLogs& logs, IBlockSink& sink, std::atomic<std::uint32_t>* recentMinLens)
    {
        recentMinLens_ = recentMinLens;
        blockMinLens_  = 0;
        blockCount_    = 0;
        for (auto& m : lastBlocks_)
            m = 0;
        stats_            = ResolveStats{};
        haveTailSnapshot_ = false;
        pos_              = 0;
        pieceOpen_        = false;
        // The first chunk's parse comes first: the logs may be the only guarantee that the input is there.
        ChunkView current;
        current.Set(&logs.AwaitLog(0));
        tracker_.BeginBlock(0);
        block_       = ResolvedBlock{};
        block_.begin = 0;

        const std::size_t numChunks = chunkStarts.size();
        PathCursor*       source    = nullptr; // the lane followed in current
        ChunkView         candidate;
        bool              candidateSet = false;
        std::size_t       next         = 1;
        std::size_t       currentChunk = 0;
        bool              chunkMisses  = false; // the current chunk's schedule lane did not hold somewhere
        bool              inRepair     = false;
        auto              taken        = [&](const PathCursor* lane) {
            if (!lane->FollowsSchedule() && current.Log()->mask != AllMinLens)
                chunkMisses = true;
        };

        while (pos_ < inSize_) {
            if (!haveTailSnapshot_ && pos_ + TailSnapshot >= inSize_) {
                tailTracker_      = tracker_;
                tailPos_          = pos_;
                haveTailSnapshot_ = true;
            }

            std::size_t         nextRecalc;
            const std::uint32_t minLen  = tracker_.PeekMinLen(pos_, &nextRecalc);
            const bool          canSync = pos_ + TailGuard <= inSize_;
            blockMinLens_ |= MinLenBit(minLen);

            // Take over the next chunk where one of its lanes meets the true parse.
            while (next < numChunks && pos_ >= chunkStarts[next]) {
                if (!candidateSet) {
                    candidate.Set(&logs.AwaitLog(next));
                    candidateSet = true;
                }
                PathCursor* found = nullptr;
                const auto  r     = candidate.Find(pos_, minLen, &found);
                if (r == ChunkView::Result::Exhausted) {
                    logs.Release(next, true);
                    ++next; // no lane reached this far
                    candidateSet = false;
                    continue;
                }
                if (canSync && r == ChunkView::Result::Found) {
                    ++stats_.syncs;
                    if (pos_ - chunkStarts[next] > stats_.maxSyncDistance)
                        stats_.maxSyncDistance = pos_ - chunkStarts[next];
                    logs.Release(currentChunk, chunkMisses);
                    currentChunk = next;
                    chunkMisses  = false;
                    current      = candidate;
                    source       = nullptr;
                    current.Find(pos_, minLen, &source);
                    taken(source);
                    inRepair     = false;
                    candidateSet = false;
                    ++next;
                    continue;
                }
                break;
            }

            // Keep following the lane while it agrees; else another lane of the chunk that does.
            const bool sourceOk = source != nullptr && !source->AtEnd() && source->Pos() == pos_ && source->Matches(minLen);
            if (!inRepair && !sourceOk) {
                source = nullptr;
                if (current.Find(pos_, minLen, &source) != ChunkView::Result::Found) {
                    if (current.Log()->Lane(minLen) == nullptr && current.Log()->mask != AllMinLens)
                        ++stats_.minLenMismatches;
                    else
                        ++stats_.parseExhausted;
                    StartRepair(pos_, sink);
                    inRepair    = true;
                    chunkMisses = true;
                } else {
                    taken(source);
                }
            } else if (inRepair && canSync) {
                // Back from the re-parse once a lane of the chunk meets the true parse again.
                PathCursor* found = nullptr;
                if (current.Find(pos_, minLen, &found) == ChunkView::Result::Found) {
                    source   = found;
                    inRepair = false;
                    taken(source);
                }
            }
            if (inRepair && repair_.AtEnd())
                RefillRepair(sink);

            tracker_.CommitIterationStart(minLen, nextRecalc);
            ++stats_.minLenIterations[minLen < MaxMinLen ? minLen : MaxMinLen];

            PathCursor& active = inRepair ? repair_ : *source;
            if (!pieceOpen_ || pieceStart_.Log() != active.Log())
                SwitchSource(active.Segment());

            // Up to the next position where the true state may change.
            std::size_t limit = pos_ + 1;
            if (!inRepair) {
                limit = nextRecalc;
                // A pending chunk change is checked at every iteration start until the chunk agrees.
                if (next < numChunks && chunkStarts[next] < limit)
                    limit = chunkStarts[next] > pos_ ? chunkStarts[next] : pos_ + 1;
                if (!haveTailSnapshot_ && inSize_ - TailSnapshot < limit)
                    limit = inSize_ - TailSnapshot;
                if (canSync && inSize_ - TailGuard < limit)
                    limit = inSize_ - TailGuard;
                if (active.NextMinLenChange() < limit)
                    limit = active.NextMinLenChange();
            }

            const std::size_t before = pos_;
            Consume(active, limit, sink);
            stats_.minLenBytes[minLen < MaxMinLen ? minLen : MaxMinLen] += pos_ - before;
            if (inRepair)
                stats_.repairedBytes += pos_ - before;
        }
        EmitBlock(inSize_, sink);
        logs.Release(currentChunk, chunkMisses);
        if (candidateSet)
            logs.Release(next, true);
        return stats_;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
