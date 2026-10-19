//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include "BlockTracker.h"
#include "BlockWriter.h"
#include "MultiParser.h"
#include "Parser.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// New in ctDeflate, not in libdeflate: the resolver of the parallel algorithm, which builds the exact sequential parse
// and block boundaries from the speculative chunk parses, and the cursors it walks the step logs with.

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    /// <summary>Walks the iterations of a step log.</summary>
    class StepCursor
    {
    public:
        void Attach(const StepSpan& log) noexcept;

        bool             AtEnd() const noexcept { return pos_ >= log_->end; }
        std::size_t      Pos() const noexcept { return pos_; }
        std::size_t      End() const noexcept { return log_->end; }
        const ParseStep& Step() const noexcept { return log_->steps[step_]; }
        /// <summary>For a step of literal iterations: how many are left from the current position.</summary>
        std::uint32_t   LiteralsLeft() const noexcept { return log_->steps[step_].literals - litDone_; }
        const StepSpan* Log() const noexcept { return log_; }

        /// <summary>Moves past the current iteration.</summary>
        void Next() noexcept;
        /// <summary>Moves past count literal iterations of the current step (count &lt;= LiteralsLeft()).</summary>
        void SkipLiterals(std::uint32_t count) noexcept;
        /// <summary>The remaining steps of the log; litDone: literal iterations of the first one already consumed.</summary>
        const ParseStep* RemainingSteps(std::size_t* count, std::uint32_t* litDone) const noexcept
        {
            *count   = log_->count - step_;
            *litDone = litDone_;
            return log_->steps + step_;
        }
        /// <summary>Moves count steps further, to litDone literal iterations into the next one, at pos.</summary>
        void Advance(std::size_t count, std::uint32_t litDone, std::size_t pos) noexcept
        {
            step_ += count;
            litDone_ = litDone;
            pos_     = pos;
        }
        /// <summary>Skips iterations until the position is at least target.</summary>
        void AdvanceTo(std::size_t target) noexcept;

    private:
        const StepSpan* log_     = nullptr;
        std::size_t     step_    = 0;
        std::uint32_t   litDone_ = 0;
        std::size_t     pos_     = 0;
    };

    /// <summary>Walks one lane of a path log: a constant minimum match length, the schedule path or a single path.</summary>
    class PathCursor
    {
    public:
        static constexpr std::uint32_t AnyMinLen = 0xFFFFFFFFu;

        /// <summary>At the start of the lane (at its end if the log has none).</summary>
        void Attach(const PathLog& log, std::uint32_t lane) noexcept
        {
            path_   = &log;
            span_   = log.Lane(lane);
            events_ = nullptr;
            event_  = 0;
            minLen_ = log.mask == AllMinLens ? AnyMinLen : lane;
            if (span_ == nullptr)
                return;
            cursor_.Attach(*span_);
            if (log.mask != AllMinLens && lane == ScheduleLane) {
                events_ = &log.lanes[ScheduleLane].minLens;
                UpdateMinLen();
            }
        }

        bool        AtEnd() const noexcept { return span_ == nullptr || cursor_.AtEnd(); }
        std::size_t Pos() const noexcept { return cursor_.Pos(); }
        /// <summary>The minimum match length the lane used at the current position (AnyMinLen: a single path).</summary>
        std::uint32_t MinLen() const noexcept { return minLen_; }
        bool          Matches(std::uint32_t minLen) const noexcept { return minLen_ == AnyMinLen || minLen_ == minLen; }
        /// <summary>Whether this is the schedule lane of a log with several lanes.</summary>
        bool FollowsSchedule() const noexcept { return events_ != nullptr; }
        /// <summary>Position from which the lane's minimum match length changes next (SIZE_MAX if never).</summary>
        std::size_t NextMinLenChange() const noexcept { return events_ != nullptr && event_ + 1 < events_->size() ? (*events_)[event_ + 1].pos : SIZE_MAX; }
        const ParseStep&  Step() const noexcept { return cursor_.Step(); }
        std::uint32_t     LiteralsLeft() const noexcept { return cursor_.LiteralsLeft(); }
        const StepSpan*   Log() const noexcept { return cursor_.Log(); }
        const StepCursor& Segment() const noexcept { return cursor_; }

        void Next() noexcept
        {
            cursor_.Next();
            UpdateMinLen();
        }
        void SkipLiterals(std::uint32_t count) noexcept
        {
            cursor_.SkipLiterals(count);
            UpdateMinLen();
        }
        const ParseStep* RemainingSteps(std::size_t* count, std::uint32_t* litDone) const noexcept { return cursor_.RemainingSteps(count, litDone); }
        void             Advance(std::size_t count, std::uint32_t litDone, std::size_t pos) noexcept
        {
            cursor_.Advance(count, litDone, pos);
            UpdateMinLen();
        }
        void AdvanceTo(std::size_t target) noexcept
        {
            if (span_ != nullptr) {
                cursor_.AdvanceTo(target);
                UpdateMinLen();
            }
        }

    private:
        void UpdateMinLen() noexcept
        {
            if (events_ == nullptr || events_->empty())
                return;
            while (event_ + 1 < events_->size() && (*events_)[event_ + 1].pos <= cursor_.Pos())
                ++event_;
            minLen_ = (*events_)[event_].minLen;
        }

        const PathLog*                  path_   = nullptr;
        const StepSpan*                 span_   = nullptr;
        const std::vector<MinLenEvent>* events_ = nullptr;
        std::size_t                     event_  = 0;
        std::uint32_t                   minLen_ = 0;
        StepCursor                      cursor_;
    };

    /// <summary>The lanes of one chunk's path log, each walked forward as the true parse advances.</summary>
    class ChunkView
    {
    public:
        enum class Result { Found, NotHere, Exhausted };

        void Set(const PathLog* log) noexcept
        {
            log_ = log;
            for (bool& a : attached_)
                a = false;
        }
        const PathLog* Log() const noexcept { return log_; }

        /// <summary>A lane that has an iteration start at pos with minimum match length minLen there.</summary>
        Result Find(std::size_t pos, std::uint32_t minLen, PathCursor** found) noexcept;

    private:
        PathCursor& Lane(std::uint32_t lane) noexcept
        {
            if (!attached_[lane]) {
                lanes_[lane].Attach(*log_, lane);
                attached_[lane] = true;
            }
            return lanes_[lane];
        }

        const PathLog* log_ = nullptr;
        PathCursor     lanes_[ScheduleLane + 1];
        bool           attached_[ScheduleLane + 1] = {};
    };

    /// <summary>A stretch of a block taken from one step log: from the cursor's position to end.</summary>
    struct BlockPiece {
        StepCursor  start;
        std::size_t end;
    };

    /// <summary>A block with its final boundaries; its symbols are in the step logs the pieces point to.</summary>
    struct ResolvedBlock {
        std::size_t             begin   = 0;
        std::size_t             length  = 0;
        bool                    isFinal = false;
        std::vector<BlockPiece> pieces;
    };

    /// <summary>follows libdeflate: deflate_choose_literal(), deflate_choose_match() (deflate_compress.c). Builds the
    /// sequences (terminated by a match length 0) and symbol frequencies of a block.</summary>
    void BuildBlockSymbols(const std::uint8_t* in, const ResolvedBlock& block, std::vector<DeflateSequence>& sequences, DeflateFreqs& freqs);

    /// <summary>Supplies the speculative parse of chunk k (it starts at the chunk start and runs past the chunk end);
    /// may block until it is ready.</summary>
    class IChunkLogs
    {
    public:
        virtual const PathLog& AwaitLog(std::size_t chunk) = 0;
        /// <summary>
        /// The resolver no longer reads the parse of chunk k (blocks may still refer to it). misses: the schedule lane did
        /// not hold throughout (another lane or a re-parse was needed), or the chunk was not used.
        /// </summary>
        virtual void Release(std::size_t chunk, bool misses)
        {
            (void)chunk;
            (void)misses;
        }

    protected:
        ~IChunkLogs() = default;
    };

    /// <summary>
    /// Receives the blocks in order. The step logs they refer to stay valid until the sink reports the blocks as appended
    /// to the output (AppendedTo), or as long as the resolver.
    /// </summary>
    class IBlockSink
    {
    public:
        virtual void OnBlock(ResolvedBlock&& block) = 0;

        /// <summary>
        /// Input position below which every block is appended to the output, so their step logs are no longer read; the
        /// resolver frees its re-parse logs below it. Any thread may move it; 0: nothing is freed before the end.
        /// </summary>
        virtual std::size_t AppendedTo() const noexcept { return 0; }

    protected:
        ~IBlockSink() = default;
    };

    /// <summary>Counters of a resolve run.</summary>
    struct ResolveStats {
        std::size_t syncs                = 0;  // chunk changes at a matching state
        std::size_t repairs              = 0;  // exact re-parses started
        std::size_t repairedBytes        = 0;  // input covered by exact re-parses
        std::size_t maxSyncDistance      = 0;  // bytes after a chunk start until its parse was taken over
        std::size_t minLenMismatches     = 0;  // repairs because the chunk has no path for the true minimum match length
        std::size_t parseExhausted       = 0;  // repairs because the path ended (or left the true parse) before the next chunk agreed
        std::size_t minLenIterations[MaxMinLen + 1] = {}; // slow-path iteration starts per true minimum match length
        std::size_t minLenBytes[MaxMinLen + 1]      = {}; // input bytes per true minimum match length
    };

    /// <summary>
    /// New in ctDeflate, not in libdeflate: joins the speculative chunk parses into the sequential result.
    /// Builds the exact libdeflate iteration sequence from speculative chunk parses: follows a chunk's iterations while
    /// position and minimum match length agree with the true state, changes to the next chunk at the first iteration
    /// start (at or after its start) where they agree, and re-parses exactly from the true state where no chunk
    /// agrees. Replays the block decisions and emits the blocks as pieces of the step logs.
    /// </summary>
    class Resolver
    {
    public:
        Resolver(const LevelParams& params, const std::uint8_t* in, std::size_t inSize);

        /// <summary>chunkStarts[0] must be 0; the chunk parses come from logs. If recentMinLens is given, it receives
        /// the true minimum match lengths of the last blocks (bit m), so that later chunk parses can follow them.</summary>
        ResolveStats Run(const std::vector<std::size_t>& chunkStarts, IChunkLogs& logs, IBlockSink& sink, std::atomic<std::uint32_t>* recentMinLens = nullptr);

    private:
        /// <summary>Consumes iterations from source until pos reaches limit or the block ends (at least one).</summary>
        void Consume(PathCursor& source, std::size_t limit, IBlockSink& sink);
        void SwitchSource(const StepCursor& source);
        void EmitBlock(std::size_t end, IBlockSink& sink);
        void StartRepair(std::size_t pos, IBlockSink& sink);
        void RefillRepair(IBlockSink& sink);

        LevelParams         params_;
        const std::uint8_t* in_;
        std::size_t         inSize_;
        BlockTracker        tracker_;
        ResolvedBlock       block_;
        StepCursor          pieceStart_;
        bool                pieceOpen_ = false;
        std::size_t         pos_       = 0;

        Parser                                repairParser_;
        std::vector<std::unique_ptr<PathLog>> repairLogs_; // the re-parse batches; null once freed
        std::vector<std::size_t>              repairEnds_; // the input end of each batch
        std::size_t                           repairFreed_ = 0; // the batches below are freed
        PathCursor                            repair_;
        bool                                  haveTailSnapshot_ = false;
        std::size_t                           tailPos_          = 0;
        BlockTracker                          tailTracker_;
        ResolveStats                          stats_;
        std::atomic<std::uint32_t>*           recentMinLens_  = nullptr;
        std::uint32_t                         blockMinLens_   = 0;  // true minimum match lengths of the current block
        std::uint32_t                         lastBlocks_[16] = {}; // ... of the last blocks
        std::size_t                           blockCount_     = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
