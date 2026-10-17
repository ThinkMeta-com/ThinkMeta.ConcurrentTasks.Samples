// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cassert>
#include <cstdint>

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "FrameData.h"

import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>
    /// The frames in flight: a ring of FrameData slots, the edge that
    /// carries each frame through the five frame stages of the encoder graph (EncodeGraph.h),
    /// alternating between serial and parallel:
    /// <list type="number">
    /// <item>Dispatch, one task in frame order: fills a slot with the frame's window; holds back while all slots are in use;</item>
    /// <item>Prepare, any number of workers in any order: psychoacoustic model without state, polyphase filterbank;</item>
    /// <item>Resolve, one task in frame order: psychoacoustic model with state, filterbank carry;</item>
    /// <item>Search, any number of workers in any order: MDCT, stereo decision, quantization search, main data;</item>
    /// <item>Encode, one task in frame order: quantization with the reservoir, bit stream; frees the slot.</item>
    /// </list>
    /// Each stage acquires a frame and completes it. The workers share one queue of both kinds
    /// of work; they take searches first, which belong to older frames and free slots sooner.
    /// Each slot carries the number of its frame, which all stages check.
    /// </summary>
    class FrameRing
    {
    public:
        /// <summary>
        /// The slots for a number of compute threads: four per thread, from 16 to 128. Too few
        /// leave workers waiting for the serial stages, too many cost cache (measured on 8 cores
        /// with 16 threads: 64 slots 0.96 of the time with 32, 256 slots slower again; with 4
        /// and 8 threads 64 slots were slower than 16 and 32).
        /// </summary>
        static constexpr std::uint32_t SlotsFor(std::uint32_t threads) noexcept
        {
            const std::uint32_t slots = 4 * threads;
            return slots < 16 ? 16 : slots > 128 ? 128 : slots;
        }

        enum class Work { Prepare, Search };

        FrameRing() noexcept = default;
        ~FrameRing();

        FrameRing(const FrameRing&)            = delete;
        FrameRing& operator=(const FrameRing&) = delete;

        /// <summary>Allocates the slots; false if the memory is not available.</summary>
        bool Initialize(std::uint32_t slotCount) noexcept;

        std::uint32_t SlotCount() const noexcept { return slotCount_; }

        // --- 1. Dispatch (one task) ---

        /// <summary>The slot of the next frame; waits while all slots are in use.</summary>
        FrameData& AcquireDispatch() noexcept;

        /// <summary>Hands the acquired slot to the workers for preparation.</summary>
        void CompleteDispatch() noexcept;

        /// <summary>No more frames will be dispatched.</summary>
        void EndDispatch() noexcept;

        // --- 2. Prepare and 4. Search (workers, any number of tasks) ---

        /// <summary>
        /// The next work: the oldest resolved frame no worker has claimed for the search, else the
        /// oldest dispatched frame no worker has claimed for the preparation. Waits for one. Null
        /// once all frames have been searched or claimed for it.
        /// </summary>
        FrameData* AcquireWork(Work& work) noexcept;

        /// <summary>The claimed work on a frame is done.</summary>
        void CompleteWork(const FrameData& frame, Work work) noexcept;

        // --- 3. Resolve (one task) ---

        /// <summary>The next prepared frame; waits for it. Null once all frames have been resolved.</summary>
        FrameData* AcquireResolve() noexcept;

        /// <summary>The acquired frame is resolved; the workers may search it.</summary>
        void CompleteResolve() noexcept;

        // --- 5. Encode (one task) ---

        /// <summary>The next searched frame; waits for it. Null once all frames have been taken.</summary>
        FrameData* AcquireEncode() noexcept;

        /// <summary>The acquired frame is done: its slot goes back to Dispatch.</summary>
        void CompleteEncode() noexcept;

    private:
        bool MoreWork() const noexcept;

        FrameData*     slots_     = nullptr;
        volatile long* prepared_  = nullptr; // per slot: the preparation of its frame is done
        volatile long* searched_  = nullptr; // per slot: the search of its frame is done
        std::uint32_t  slotCount_ = 0;

        // dispatch side
        std::int64_t written_                        = 0; // frames acquired for dispatch
        alignas(64) volatile std::int64_t committed_ = 0;
        alignas(64) volatile long ended_             = 0;
        // worker side
        alignas(64) volatile std::int64_t prepareClaimed_ = 0;
        alignas(64) volatile std::int64_t searchClaimed_  = 0;
        // resolver side
        alignas(64) volatile std::int64_t resolved_ = 0;
        alignas(64) volatile long resolveEnded_     = 0; // all frames resolved
        // encode side
        std::int64_t read_                          = 0; // frames acquired for encoding
        alignas(64) volatile std::int64_t released_ = 0;

        Synchronization::AutoResetEvent   workReady_;     // work to claim, or the end
        Synchronization::AutoResetEvent_s frameReady_;    // a preparation completed, or the end
        Synchronization::AutoResetEvent_s frameSearched_; // a search completed, or the end
        Synchronization::AutoResetEvent_s slotFree_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
