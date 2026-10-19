//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "BlockWriter.h"
#include "Resolver.h"

import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>
    /// Edge "ring": Resolve → Encode → Stitch. Each resolved block passes three stages in one slot: Resolve publishes
    /// the blocks in order (waiting while the ring is full), any Encode claims the next published one and encodes it, and
    /// Stitch appends the encoded blocks in order and frees their slots.
    /// </summary>
    class BlockRing
    {
    public:
        static constexpr std::size_t Slots = 4096;

        BlockRing()
            : blocks_(std::make_unique<ResolvedBlock[]>(Slots)),
              encoded_(std::make_unique<EncodedBlock[]>(Slots)),
              encodedDone_(std::make_unique<std::atomic<std::uint8_t>[]>(Slots))
        {}

        std::size_t Published() const noexcept { return published_.load(std::memory_order_acquire); }
        std::size_t Stitched() const noexcept { return stitched_.load(std::memory_order_seq_cst); }

        // --- Resolve

        /// <summary>Waits for a free slot (back-pressure from Stitch) and returns it for the next block.</summary>
        ResolvedBlock& AcquireResolve() noexcept
        {
            const std::size_t n = published_.load(std::memory_order_relaxed);
            while (n - stitched_.load(std::memory_order_seq_cst) >= Slots)
                stitchedSome_.Wait();
            return blocks_[n % Slots];
        }

        /// <summary>The block in the acquired slot is published.</summary>
        void CompleteResolve() noexcept { published_.store(published_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

        /// <summary>No block follows any more.</summary>
        void CompleteAllResolved() noexcept { resolveDone_.store(true, std::memory_order_seq_cst); }

        // --- Encode

        /// <summary>Claims the next published block without waiting, if any is unclaimed; i: its number.</summary>
        bool TryAcquireEncode(std::size_t& i) noexcept
        {
            i = encodeNext_.load(std::memory_order_relaxed);
            while (i < published_.load(std::memory_order_acquire))
                if (encodeNext_.compare_exchange_weak(i, i + 1, std::memory_order_relaxed))
                    return true;
            return false;
        }

        const ResolvedBlock& Block(std::size_t i) const noexcept { return blocks_[i % Slots]; }
        EncodedBlock&        Encoded(std::size_t i) noexcept { return encoded_[i % Slots]; }

        void CompleteEncode(std::size_t i) noexcept { encodedDone_[i % Slots].store(1, std::memory_order_seq_cst); }

        /// <summary>Resolve, helping at the end: wakes after an Encode completed (and Stitch ran).</summary>
        void SignalEncoded() noexcept { encodes_.Set(); }
        void AwaitEncoded() noexcept { encodes_.Wait(); }

        /// <summary>Resolve is done and every published block is claimed.</summary>
        bool EncodesDone() const noexcept
        {
            return resolveDone_.load(std::memory_order_acquire) && encodeNext_.load(std::memory_order_relaxed) >= published_.load(std::memory_order_acquire);
        }

        // --- Stitch

        /// <summary>Block s (the next to append, called by the stitch flag's holder) is encoded.</summary>
        bool IsStitchReady(std::size_t s) const noexcept
        {
            return s < published_.load(std::memory_order_acquire) && encodedDone_[s % Slots].load(std::memory_order_acquire) != 0;
        }

        /// <summary>Like IsStitchReady for the next block, from outside the stitch flag (re-check after releasing it).</summary>
        bool NextStitchReady() const noexcept
        {
            const std::size_t s = stitched_.load(std::memory_order_seq_cst);
            return s < published_.load(std::memory_order_acquire) && encodedDone_[s % Slots].load(std::memory_order_seq_cst) != 0;
        }

        ResolvedBlock& StitchBlock(std::size_t s) noexcept { return blocks_[s % Slots]; }

        /// <summary>Block s is appended: its slot is free.</summary>
        void CompleteStitch(std::size_t s) noexcept
        {
            const std::size_t slot = s % Slots;
            blocks_[slot]          = ResolvedBlock{};
            encoded_[slot].bits    = std::vector<std::uint8_t>();
            encodedDone_[slot].store(0, std::memory_order_relaxed);
            stitched_.store(s + 1, std::memory_order_seq_cst);
        }

        /// <summary>After a run of CompleteStitch: wakes Resolve if it waits for a slot.</summary>
        void SignalRoom() noexcept { stitchedSome_.Set(); }

    private:
        std::unique_ptr<ResolvedBlock[]>             blocks_;
        std::unique_ptr<EncodedBlock[]>              encoded_;
        std::unique_ptr<std::atomic<std::uint8_t>[]> encodedDone_;
        std::atomic<std::size_t>                     published_{0};
        std::atomic<std::size_t>                     encodeNext_{0};
        std::atomic<std::size_t>                     stitched_{0};
        std::atomic<bool>                            resolveDone_{false};
        Synchronization::AutoResetEvent_s            encodes_;      // a block was encoded; one waiter: Resolve
        Synchronization::AutoResetEvent_s            stitchedSome_; // the ring has room again; one waiter: Resolve
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
