//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Checksums.h"
#include "ChunkedDeflate.h"

import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>
    /// Edge "logs": Parse → Resolve. One step log per chunk, written by the Parse that claimed the chunk and read by
    /// Resolve in chunk order. The parse window: Parse claims chunks in order, at most ahead chunks past the chunk
    /// Resolve waits for (all once Resolve is done). Also carries the feedback Resolve → Parse: the minimum match
    /// lengths of the last blocks and how many chunks in a row the schedule lane held.
    /// </summary>
    class ChunkLogs
    {
    public:
        static constexpr std::uint32_t QuietChunks = 8; // from then on chunks parse only the schedule lane

        void Plan(std::vector<std::size_t> starts, std::size_t inSize, const ChunkOptions& options, std::size_t ahead)
        {
            starts_  = std::move(starts);
            chunks_  = starts_.size();
            inSize_  = inSize;
            overlap_ = options.overlap;
            minLens_ = options.minLens;
            ahead_   = ahead;
            logs_.resize(chunks_);
            logEnds_.resize(chunks_);
            crcs_.resize(chunks_);
            parsed_        = std::make_unique<Synchronization::ManualResetEvent[]>(chunks_);
            parsedFlags_   = std::make_unique<std::atomic<std::uint8_t>[]>(chunks_);
            releasedFlags_ = std::make_unique<std::atomic<std::uint8_t>[]>(chunks_);
        }

        const std::vector<std::size_t>& Starts() const noexcept { return starts_; }
        std::size_t                     Chunks() const noexcept { return chunks_; }
        std::size_t                     Overlap() const noexcept { return overlap_; }
        std::size_t                     Ahead() const noexcept { return ahead_; }
        std::size_t                     End(std::size_t k) const noexcept { return k + 1 < chunks_ ? starts_[k + 1] : inSize_; }

        // --- Parse

        /// <summary>The next unclaimed chunk (may be Chunks(): none left).</summary>
        std::size_t NextParse() const noexcept { return parseNext_.load(std::memory_order_relaxed); }

        bool MayParse(std::size_t k) const noexcept
        {
            return k <= resolverAt_.load(std::memory_order_seq_cst) + ahead_ || resolveDone_.load(std::memory_order_seq_cst);
        }

        /// <summary>Claims chunk k (NextParse() was k); false if another Parse was faster.</summary>
        bool AcquireParse(std::size_t k) noexcept { return parseNext_.compare_exchange_strong(k, k + 1, std::memory_order_relaxed); }

        /// <summary>The paths chunk k's parse follows: fixed, or chosen from the feedback of Resolve.</summary>
        std::uint32_t MinLens(const LevelParams& params, const std::uint8_t* in, std::size_t k) const noexcept
        {
            return minLens_ != 0 ? minLens_
                                 : ChunkMinLens(params, in, inSize_, starts_[k], recentMinLens_.load(std::memory_order_relaxed),
                                                quietChunks_.load(std::memory_order_relaxed) >= QuietChunks);
        }

        PathLog& Log(std::size_t k) noexcept { return logs_[k]; }

        /// <summary>Chunk k's log is complete; crc: CRC-32 of the chunk's input.</summary>
        void CompleteParse(std::size_t k, std::uint32_t crc) noexcept
        {
            const PathLog& log    = logs_[k];
            std::size_t    logEnd = log.begin;
            for (std::uint32_t lane = 0; lane <= ScheduleLane; ++lane)
                if (const StepSpan* span = log.Lane(lane); span != nullptr && span->end > logEnd)
                    logEnd = span->end;
            logEnds_[k] = logEnd;
            crcs_[k]    = crc;
            parsedFlags_[k].store(1, std::memory_order_release);
            parsed_[k].Set();
        }

        /// <summary>No chunk is left to claim and Resolve is done.</summary>
        bool ParsesDone(std::size_t next) const noexcept { return resolveDone_.load(std::memory_order_acquire) && next >= chunks_; }

        // --- Resolve

        /// <summary>Resolve waits for chunk from now on; returns by how many chunks the parse window moved (0: not).</summary>
        std::size_t MoveWindow(std::size_t chunk) noexcept
        {
            const std::size_t at = resolverAt_.load(std::memory_order_relaxed);
            if (chunk <= at)
                return 0;
            resolverAt_.store(chunk, std::memory_order_seq_cst);
            return chunk - at;
        }

        /// <summary>Waits until chunk's log is complete.</summary>
        const PathLog& AcquireLog(std::size_t chunk) noexcept
        {
            parsed_[chunk].Wait();
            return logs_[chunk];
        }

        /// <summary>Resolve no longer reads chunk's log; misses: its schedule lane did not hold.</summary>
        void CompleteLog(std::size_t chunk, bool misses) noexcept
        {
            quietChunks_.store(misses ? 0 : quietChunks_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            releasedFlags_[chunk].store(1, std::memory_order_release);
        }

        std::atomic<std::uint32_t>* RecentMinLens() noexcept { return &recentMinLens_; }

        /// <summary>All blocks are resolved: the window opens to the end.</summary>
        void CompleteResolve() noexcept { resolveDone_.store(true, std::memory_order_seq_cst); }

        // --- Stitch

        /// <summary>Frees the logs Resolve left whose pieces the bitstream (up to stitchedTo) has taken.</summary>
        void FreeBelow(std::size_t stitchedTo) noexcept
        {
            while (freeNext_ < chunks_ && releasedFlags_[freeNext_].load(std::memory_order_acquire) != 0 && logEnds_[freeNext_] <= stitchedTo) {
                logs_[freeNext_] = PathLog{};
                ++freeNext_;
            }
        }

        /// <summary>Start of the lowest chunk whose parse may still run (inSize: none).</summary>
        std::size_t OldestParseStart() noexcept
        {
            while (parseLow_ < chunks_ && parsedFlags_[parseLow_].load(std::memory_order_acquire) != 0)
                ++parseLow_;
            return parseLow_ < chunks_ ? starts_[parseLow_] : inSize_;
        }

        // --- the graph, at the end

        /// <summary>CRC-32 of the whole input.</summary>
        std::uint32_t Crc() const noexcept
        {
            std::uint32_t crc = crcs_[0];
            for (std::size_t k = 1; k < chunks_; ++k)
                crc = Crc32Combine(crc, crcs_[k], End(k) - starts_[k]);
            return crc;
        }

    private:
        std::vector<std::size_t>                             starts_;
        std::size_t                                          chunks_  = 0;
        std::size_t                                          inSize_  = 0;
        std::size_t                                          overlap_ = 0;
        std::uint32_t                                        minLens_ = 0; // fixed, or 0: chosen per chunk
        std::size_t                                          ahead_   = 0;
        std::vector<PathLog>                                 logs_;
        std::vector<std::size_t>                             logEnds_; // end of the furthest lane of each log
        std::vector<std::uint32_t>                           crcs_;
        std::unique_ptr<Synchronization::ManualResetEvent[]> parsed_;
        std::unique_ptr<std::atomic<std::uint8_t>[]>         parsedFlags_;
        std::unique_ptr<std::atomic<std::uint8_t>[]>         releasedFlags_; // Resolve is done with the log
        std::atomic<std::size_t>                             parseNext_{0};
        std::atomic<std::size_t>                             resolverAt_{0}; // chunk Resolve waited for last
        std::atomic<bool>                                    resolveDone_{false};
        std::atomic<std::uint32_t>                           recentMinLens_{0};
        std::atomic<std::uint32_t>                           quietChunks_{0}; // resolved chunks in a row whose schedule lane held
        std::size_t                                          parseLow_ = 0;   // Stitch's: lowest chunk whose parse may still run
        std::size_t                                          freeNext_ = 0;   // Stitch's: lowest log not yet freed
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
