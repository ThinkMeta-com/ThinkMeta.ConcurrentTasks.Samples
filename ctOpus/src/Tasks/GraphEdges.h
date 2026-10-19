// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <atomic>
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
    /// them (at 48 kHz a copy). Read writes and decommits, Resample reads; which blocks may be computed is the business
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
        bool              resampled   = true; // false: input at 48 kHz, the resampler copies (filter length 1)
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
    /// <para>
    /// How it is built: counters per stage (std::atomic, each on its own cache line; the stores are
    /// sequentially consistent, as the stages re-check counters of other stages after them) and
    /// four events. The slots are not counted with the library's Semaphore_s: a slot is free only
    /// once two stages (Encode and Analyse) have its block, and Read needs the number of the
    /// released block, not a count.
    /// </para>
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
            endKnown_.store(true); // publishes the three values above
        }

        std::int64_t BlockEnd(std::int64_t b) const noexcept
        {
            const std::int64_t end = (b + 1) * BlockFrames;
            return endKnown_.load(std::memory_order_acquire) ? MinI64(end, totalOutputs_) : end;
        }

        // --- abort (any node)

        bool Aborted() const noexcept { return abort_.load(std::memory_order_acquire); }

        /// <summary>Ends the run early: every stage that waits on the ring wakes and sees it.</summary>
        void Abort() noexcept
        {
            abort_.store(true);
            slotFree_.Set();
            resampleReady_.Set();
            encodeReady_.Set();
            analyseReady_.Set();
        }

        bool MemoryError() const noexcept { return memoryError_; }

        // --- Read

        /// <summary>Waits until block b has a free slot; false if aborted.</summary>
        bool AcquireRead(std::int64_t b) noexcept
        {
            while (b - released_.load(std::memory_order_acquire) >= slotCount_) {
                if (Aborted())
                    return false;
                slotFree_.Wait();
            }
            return true;
        }

        /// <summary>Block b may be computed.</summary>
        void CompleteRead(std::int64_t b) noexcept
        {
            dispatched_.store(b + 1);
            resampleReady_.Set();
        }

        /// <summary>No block follows: the stages that wait for blocks see the end.</summary>
        void EndRead() noexcept
        {
            ended_.store(true);
            resampleReady_.Set();
            encodeReady_.Set();
            analyseReady_.Set();
        }

        /// <summary>Read could not go on (out of memory): abort, the end is known, no block follows.</summary>
        void FailRead() noexcept
        {
            memoryError_ = true;
            endKnown_.store(true);
            Abort();
            EndRead();
        }

        /// <summary>The blocks before this one are released (Read decommits their input).</summary>
        std::int64_t Released() const noexcept { return released_.load(std::memory_order_acquire); }

        // --- Resample (any worker)

        /// <summary>The next dispatched block, or -1 at the end.</summary>
        std::int64_t AcquireResample() noexcept
        {
            for (;;) {
                std::int64_t c = claimed_.load(std::memory_order_acquire);
                if (c < dispatched_.load(std::memory_order_acquire)) {
                    if (!claimed_.compare_exchange_strong(c, c + 1))
                        continue;
                    if (c + 1 < dispatched_.load(std::memory_order_acquire))
                        resampleReady_.Set(); // more work: wake the next worker
                    return c;
                }
                if (ended_.load(std::memory_order_acquire) && claimed_.load(std::memory_order_acquire) >= dispatched_.load(std::memory_order_acquire)) {
                    resampleReady_.Set(); // let the other workers see the end too
                    return -1;
                }
                resampleReady_.Wait();
            }
        }

        /// <summary>Block b is in its slot.</summary>
        void CompleteResample(std::int64_t b) noexcept
        {
            SlotBlock(b).store(b + 1);
            encodeReady_.Set();
            analyseReady_.Set();
        }

        // --- Encode (blocks in order)

        /// <summary>Waits for block b; false at the end of the blocks.</summary>
        bool AcquireEncode(std::int64_t b) noexcept
        {
            while (SlotBlock(b).load(std::memory_order_acquire) != b + 1) {
                const std::int64_t dispatched = dispatched_.load(std::memory_order_acquire);
                if (ended_.load(std::memory_order_acquire) && claimed_.load(std::memory_order_acquire) >= dispatched && b >= dispatched)
                    return false;
                encodeReady_.Wait();
            }
            return true;
        }

        void CompleteEncode(std::int64_t b) noexcept
        {
            encodeReleased_.store(b + 1);
            UpdateReleased();
        }

        // --- Analyse (blocks in order; only with an analysis chain)

        std::int64_t AnalyseNext() const noexcept { return analyseCopied_.load(std::memory_order_acquire); }

        /// <summary>Waits for block b; false if aborted or at the end of the blocks.</summary>
        bool AcquireAnalyse(std::int64_t b) noexcept
        {
            while (SlotBlock(b).load(std::memory_order_acquire) != b + 1) {
                if (Aborted() || (ended_.load(std::memory_order_acquire) && b >= dispatched_.load(std::memory_order_acquire)))
                    return false;
                analyseReady_.Wait();
            }
            return true;
        }

        void CompleteAnalyse(std::int64_t b) noexcept
        {
            analyseCopied_.store(b + 1);
            UpdateReleased();
        }

        /// <summary>The chain has ended: the remaining blocks are Encode's alone.</summary>
        void DetachAnalyse() noexcept
        {
            analyseCopied_.store(Never);
            UpdateReleased();
        }

        /// <summary>Wakes Analyse (after an abort the encoder sets it, so the chain sees the end).</summary>
        void SignalAnalyse() noexcept { analyseReady_.Set(); }

    private:
        static constexpr std::int64_t Never = 1ll << 62;

        std::atomic_ref<std::int64_t> SlotBlock(std::int64_t b) noexcept { return std::atomic_ref<std::int64_t>(slotBlock_[static_cast<std::size_t>(b % slotCount_)]); }

        // released = min(encodeReleased, analyseCopied), only upwards
        void UpdateReleased() noexcept
        {
            std::int64_t old = released_.load(std::memory_order_acquire);
            for (;;) {
                const std::int64_t next = MinI64(encodeReleased_.load(std::memory_order_acquire), analyseCopied_.load(std::memory_order_acquire));
                if (next <= old)
                    return;
                if (released_.compare_exchange_strong(old, next)) {
                    slotFree_.Set();
                    return;
                }
            }
        }

        HeapArray<float>        slots_;     // slotCount blocks of interleaved output
        HeapArray<std::int64_t> slotBlock_; // per slot: 1 + the block it holds, once computed (accessed atomically: SlotBlock)
        std::int64_t            slotCount_ = 0;
        std::int32_t            channels_  = 0;

        // Read -> all (written before the dispatch counter moves)
        std::int64_t inputFrames_                     = 0;
        std::int64_t realOutputs_                     = 0;
        std::int64_t totalOutputs_                    = 0;
        alignas(64) std::atomic<bool> endKnown_{false};
        alignas(64) std::atomic<std::int64_t> dispatched_{0}; // blocks Resample may compute
        alignas(64) std::atomic<bool> ended_{false};          // dispatched is final
        // Resample
        alignas(64) std::atomic<std::int64_t> claimed_{0};
        // Encode, Analyse -> Read: a slot is free once both have its block
        alignas(64) std::atomic<std::int64_t> released_{0};
        alignas(64) std::atomic<std::int64_t> encodeReleased_{0};
        alignas(64) std::atomic<std::int64_t> analyseCopied_{Never}; // without a chain: never the limit
        alignas(64) std::atomic<bool> abort_{false};
        bool memoryError_ = false;

        Synchronization::AutoResetEvent   resampleReady_; // blocks to claim, or the end; many waiters: the Resample tasks
        Synchronization::AutoResetEvent_s encodeReady_;   // a block computed; one waiter: Encode
        Synchronization::AutoResetEvent_s analyseReady_;  // a block computed, the end, or an abort; one waiter: Analyse
        Synchronization::AutoResetEvent_s slotFree_;      // a block released; one waiter: Read
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
