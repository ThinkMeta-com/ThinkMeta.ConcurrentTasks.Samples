// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>

#include "HeapArray.h"
#include "ParallelResampler.h"
#include "PlanarStore.h"

// headers with imports last
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    inline std::int64_t MaxI64(std::int64_t a, std::int64_t b) noexcept
    {
        return a > b ? a : b;
    }
    inline std::int64_t MinI64(std::int64_t a, std::int64_t b) noexcept
    {
        return a < b ? a : b;
    }

    /// <summary>
    /// The edge Read → Resample: the input samples in a planar store (padded with filter length - 1
    /// zeros in front, the drain extension and zeros behind) and the parallel resampler that reads
    /// them. Read writes and decommits, Resample reads; which blocks may be computed is the business
    /// of the block ring.
    /// </summary>
    class PcmFeed
    {
    public:
        static constexpr std::int32_t ReadFrames   = 16384; // input frames per read
        static constexpr std::int64_t CommitMargin = 64;    // readable frames beyond the data (AVX2 loads)

        ParallelResampler resampler;
        PlanarStore       input;
        std::int32_t      channels    = 0;
        std::int32_t      filterLen   = 0;
        std::int32_t      drainFrames = 0;
    };

    /// <summary>
    /// The edge Resample → Encode and Resample → Analyse: a ring of slots, each one block of
    /// BlockFrames 48 kHz output frames (interleaved). A block passes the stages in order:
    /// <code>
    ///  Read: AcquireRead (a free slot) → CompleteRead (the block may be computed)
    ///  Resample: AcquireResample (the next dispatched block, any worker) → CompleteResample (computed)
    ///  Encode: AcquireEncode (blocks in order) → CompleteEncode
    ///  Analyse: AcquireAnalyse (blocks in order) → CompleteAnalyse    (only with an analysis chain)
    /// </code>
    /// A slot is free again once both Encode and Analyse have completed its block. Read also publishes
    /// the end of the stream (PublishEnd), and any node can abort the run (Abort).
    /// Created on a task: its events belong to the calling task.
    /// </summary>
    class BlockRing
    {
    public:
        static constexpr std::int64_t BlockFrames = 16384; // 48 kHz output frames per block (a multiple of 16)

        /// <summary>slotCount slots of channels; analysed: an analysis chain reads the blocks too.</summary>
        bool Initialize(std::int64_t slotCount, std::int32_t channels, bool analysed) noexcept
        {
            slotCount_ = slotCount;
            channels_  = channels;
            if (analysed)
                analyseCopied_ = 0; // the chain's next block
            return slots_.Allocate(static_cast<std::size_t>(slotCount * BlockFrames * channels)) && slotBlock_.Allocate(static_cast<std::size_t>(slotCount));
        }

        std::int64_t SlotCount() const noexcept { return slotCount_; }
        float*       SlotOf(std::int64_t b) noexcept { return slots_.Data() + static_cast<std::size_t>(b % slotCount_) * BlockFrames * channels_; }

        // --- the end of the stream (Read publishes it once)

        std::int64_t InputFrames() const noexcept { return inputFrames_; }   // the samples per channel
        std::int64_t RealOutputs() const noexcept { return realOutputs_; }   // outputs of the samples (ope_encoder_write_float)
        std::int64_t TotalOutputs() const noexcept { return totalOutputs_; } // plus the drain outputs
        bool         EndKnown() const noexcept { return endKnown_ != 0; }

        void PublishEnd(std::int64_t inputFrames, std::int64_t realOutputs, std::int64_t totalOutputs) noexcept
        {
            inputFrames_  = inputFrames;
            realOutputs_  = realOutputs;
            totalOutputs_ = totalOutputs;
            InterlockedExchange(&endKnown_, 1);
        }

        std::int64_t BlockEnd(std::int64_t b) const noexcept
        {
            const std::int64_t end = (b + 1) * BlockFrames;
            return endKnown_ != 0 ? MinI64(end, totalOutputs_) : end;
        }

        // --- abort (any node)

        bool Aborted() const noexcept { return abort_ != 0; }
        void Abort() noexcept { InterlockedExchange(&abort_, 1); }
        bool MemoryError() const noexcept { return memoryError_; }

        // --- Read

        /// <summary>Waits until block b has a free slot; false if aborted.</summary>
        bool AcquireRead(std::int64_t b) noexcept
        {
            while (b - released_ >= slotCount_) {
                if (abort_ != 0)
                    return false;
                slotFree_.Wait();
            }
            return true;
        }

        /// <summary>Block b may be computed.</summary>
        void CompleteRead(std::int64_t b) noexcept
        {
            InterlockedExchange64(&dispatched_, b + 1);
            resampleReady_.Set();
        }

        /// <summary>No block follows: the stages that wait for blocks see the end.</summary>
        void EndRead() noexcept
        {
            InterlockedExchange(&ended_, 1);
            resampleReady_.Set();
            encodeReady_.Set();
            analyseReady.Set();
        }

        /// <summary>Read could not go on (out of memory): abort, the end is known, no block follows.</summary>
        void FailRead() noexcept
        {
            memoryError_ = true;
            InterlockedExchange(&abort_, 1);
            InterlockedExchange(&endKnown_, 1);
            EndRead();
        }

        /// <summary>The blocks before this one are released (Read decommits their input).</summary>
        std::int64_t Released() const noexcept { return released_; }

        // --- Resample (any worker)

        /// <summary>The next dispatched block, or -1 at the end.</summary>
        std::int64_t AcquireResample() noexcept
        {
            for (;;) {
                const std::int64_t c = claimed_;
                if (c < dispatched_) {
                    if (InterlockedCompareExchange64(&claimed_, c + 1, c) != c)
                        continue;
                    if (c + 1 < dispatched_)
                        resampleReady_.Set(); // more work: wake the next worker
                    return c;
                }
                if (ended_ != 0 && claimed_ >= dispatched_) {
                    resampleReady_.Set(); // let the other workers see the end too
                    return -1;
                }
                resampleReady_.Wait();
            }
        }

        /// <summary>Block b is in its slot.</summary>
        void CompleteResample(std::int64_t b) noexcept
        {
            InterlockedExchange64(&slotBlock_[static_cast<std::size_t>(b % slotCount_)], b + 1);
            encodeReady_.Set();
            analyseReady.Set();
        }

        // --- Encode (blocks in order)

        /// <summary>Waits for block b; false at the end of the blocks.</summary>
        bool AcquireEncode(std::int64_t b) noexcept
        {
            while (slotBlock_[static_cast<std::size_t>(b % slotCount_)] != b + 1) {
                if (ended_ != 0 && claimed_ >= dispatched_ && b >= dispatched_)
                    return false;
                encodeReady_.Wait();
            }
            return true;
        }

        void CompleteEncode(std::int64_t b) noexcept
        {
            InterlockedExchange64(&encodeReleased_, b + 1);
            UpdateReleased();
        }

        // --- Analyse (blocks in order; only with an analysis chain)

        std::int64_t AnalyseNext() const noexcept { return analyseCopied_; }

        /// <summary>Waits for block b; false if aborted or at the end of the blocks.</summary>
        bool AcquireAnalyse(std::int64_t b) noexcept
        {
            while (slotBlock_[static_cast<std::size_t>(b % slotCount_)] != b + 1) {
                if (abort_ != 0 || (ended_ != 0 && b >= dispatched_))
                    return false;
                analyseReady.Wait();
            }
            return true;
        }

        void CompleteAnalyse(std::int64_t b) noexcept
        {
            InterlockedExchange64(&analyseCopied_, b + 1);
            UpdateReleased();
        }

        /// <summary>The chain has ended: the remaining blocks are Encode's alone.</summary>
        void DetachAnalyse() noexcept
        {
            InterlockedExchange64(&analyseCopied_, Never);
            UpdateReleased();
        }

        Synchronization::AutoResetEvent_s analyseReady; // a block computed, the end, or an abort (for Analyse)

    private:
        static constexpr std::int64_t Never = 1ll << 62;

        // released = min(encodeReleased, analyseCopied), only upwards
        void UpdateReleased() noexcept
        {
            for (;;) {
                const std::int64_t old  = released_;
                const std::int64_t next = MinI64(encodeReleased_, analyseCopied_);
                if (next <= old)
                    return;
                if (InterlockedCompareExchange64(&released_, next, old) == old) {
                    slotFree_.Set();
                    return;
                }
            }
        }

        HeapArray<float>        slots_;     // slotCount blocks of interleaved output
        HeapArray<std::int64_t> slotBlock_; // per slot: 1 + the block it holds, once computed
        std::int64_t            slotCount_ = 0;
        std::int32_t            channels_  = 0;

        // Read -> all (written before the dispatch counter moves)
        std::int64_t inputFrames_                     = 0;
        std::int64_t realOutputs_                     = 0;
        std::int64_t totalOutputs_                    = 0;
        alignas(64) volatile long endKnown_           = 0;
        alignas(64) volatile std::int64_t dispatched_ = 0; // blocks Resample may compute
        alignas(64) volatile long ended_              = 0; // dispatched is final
        // Resample
        alignas(64) volatile std::int64_t claimed_ = 0;
        // Encode, Analyse -> Read: a slot is free once both have its block
        alignas(64) volatile std::int64_t released_       = 0;
        alignas(64) volatile std::int64_t encodeReleased_ = 0;
        alignas(64) volatile std::int64_t analyseCopied_  = Never; // without a chain: never the limit
        alignas(64) volatile long abort_                  = 0;
        bool memoryError_                                 = false;

        Synchronization::AutoResetEvent   resampleReady_; // blocks to claim, or the end
        Synchronization::AutoResetEvent_s encodeReady_;   // a block computed
        Synchronization::AutoResetEvent_s slotFree_;      // a block released
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
