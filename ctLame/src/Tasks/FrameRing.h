// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <new>

#if defined(_WIN32)
#    include <Windows.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "FrameData.h"
#include "Profiling.h"

import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// The frames in flight: a ring of FrameData slots, the edge that carries each frame through the five frame
    /// stages of the encoder graph (EncodeGraph.h), alternating between serial and parallel:
    /// <list type="number">
    /// <item>Dispatch, one task in frame order: fills a slot with the frame's window; holds back while all slots are in use;</item>
    /// <item>Prepare, any number of tasks in any order: psychoacoustic model without state, polyphase filterbank;</item>
    /// <item>Resolve, one task in frame order: psychoacoustic model with state, filterbank carry;</item>
    /// <item>Search, any number of tasks in any order: MDCT, stereo decision, quantization search, main data;</item>
    /// <item>Encode, one task in frame order: quantization with the reservoir, bit stream; frees the slot.</item>
    /// </list>
    /// Each stage acquires a frame (Acquire…, which waits) and completes it (Complete…). Each slot carries the
    /// number of its frame, which all stages check (asserts in Debug).
    /// <para>
    /// Two forms meet here. A parallel stage is a stage with several tasks: its tasks claim the frames one after
    /// the other, each frame goes to exactly one task, and the tasks complete their frames in any order. The serial
    /// stage behind it is an ordered join: its one task takes the frames in frame order and waits for the mark of
    /// the frame whose turn it is (a mark per slot, "prepared" and "searched"), however the tasks before it happen
    /// to finish.
    /// </para>
    /// <para>
    /// Who waits: Dispatch for a free slot; every other stage for a frame the stage before it has completed, a
    /// serial stage for the frame whose turn it is. Who wakes: the stage before, in its Complete…; Encode frees the
    /// slot and wakes Dispatch. A task of a parallel stage that claims a frame while more are left wakes the next
    /// task of its stage. The end: EndDispatch, on every path of Dispatch. Prepare, Resolve and Encode have ended
    /// once Dispatch has ended and they have taken every dispatched frame. Search may only take resolved frames, so
    /// it cannot tell its end from the dispatched ones: Resolve passes the end on to it. A task of a parallel stage
    /// that finds the end wakes the next task of its stage, so all of them end.
    /// </para>
    /// <para>
    /// How it is built: counters per stage (what a stage writes lies on a cache line of its own, apart from what all
    /// stages only read), the marks per slot and five events, each named after the stage that waits for it. A stage that completes a frame moves its counter or
    /// sets the frame's mark, and sets the event of the stage that may now go on; a waiting stage checks again after
    /// every wake-up, so an extra Set costs only a loop. Three events have exactly one waiter (Dispatch, Resolve,
    /// Encode: the cheaper _s variant); the tasks of Prepare share prepareReady_ and those of Search searchReady_.
    /// The ring does not use the library's Semaphore_s for the slots: the stages need more than a count (which
    /// frame), and a counter pair per stage plus an event costs one atomic operation per frame and stage.
    /// </para>
    /// </summary>
    class FrameRing
    {
    public:
        /// <summary>
        /// The slots for a number of compute threads: four per thread, from 16 to 128. Too few
        /// leave the parallel stages waiting for the serial ones, too many cost cache (measured on 8 cores
        /// with 16 threads: 64 slots 0.96 of the time with 32, 256 slots slower again; with 4
        /// and 8 threads 64 slots were slower than 16 and 32).
        /// </summary>
        static constexpr std::uint32_t SlotsFor(std::uint32_t threads) noexcept
        {
            const std::uint32_t slots = 4 * threads;
            return slots < 16 ? 16 : slots > 128 ? 128 : slots;
        }

        FrameRing() noexcept = default;
        ~FrameRing();

        FrameRing(const FrameRing&)            = delete;
        FrameRing& operator=(const FrameRing&) = delete;

        /// <summary>Allocates the slots; false if the memory is not available.</summary>
        bool Initialize(std::uint32_t slotCount) noexcept;

        /// <summary>
        /// CTLAME_PROFILE: the time the tasks of a stage wait here counts as idle time of that node; null: not
        /// measured. Call it before the tasks start.
        /// </summary>
        void MeasureWaits(TaskProfile* dispatch, TaskProfile* prepare, TaskProfile* resolve, TaskProfile* search, TaskProfile* encode) noexcept
        {
            dispatchIdle_ = IdleCounter(dispatch);
            prepareIdle_  = IdleCounter(prepare);
            resolveIdle_  = IdleCounter(resolve);
            searchIdle_   = IdleCounter(search);
            encodeIdle_   = IdleCounter(encode);
        }

        // --- 1. Dispatch (one task) ---

        /// <summary>The slot of the next frame; waits while all slots are in use.</summary>
        FrameData& AcquireDispatch() noexcept;

        /// <summary>Hands the acquired slot to Prepare.</summary>
        void CompleteDispatch() noexcept;

        /// <summary>No more frames will be dispatched.</summary>
        void EndDispatch() noexcept;

        // --- 2. Prepare (any number of tasks) ---

        /// <summary>
        /// The oldest dispatched frame no task has claimed for the preparation; waits for one. Null once all
        /// frames have been claimed and no more will be dispatched.
        /// </summary>
        FrameData* AcquirePrepare() noexcept;

        /// <summary>The claimed frame is prepared; Resolve may take it when its turn comes.</summary>
        void CompletePrepare(const FrameData& frame) noexcept;

        // --- 3. Resolve (one task) ---

        /// <summary>The next prepared frame; waits for it. Null once all frames have been resolved.</summary>
        FrameData* AcquireResolve() noexcept;

        /// <summary>The acquired frame is resolved; Search may take it.</summary>
        void CompleteResolve() noexcept;

        // --- 4. Search (any number of tasks) ---

        /// <summary>
        /// The oldest resolved frame no task has claimed for the search; waits for one. Null once all frames
        /// have been claimed and no more will be resolved.
        /// </summary>
        FrameData* AcquireSearch() noexcept;

        /// <summary>The claimed frame is searched; Encode may take it when its turn comes.</summary>
        void CompleteSearch(const FrameData& frame) noexcept;

        // --- 5. Encode (one task) ---

        /// <summary>The next searched frame; waits for it. Null once all frames have been taken.</summary>
        FrameData* AcquireEncode() noexcept;

        /// <summary>The acquired frame is done: its slot goes back to Dispatch.</summary>
        void CompleteEncode() noexcept;

    private:
        // read by all stages, written before the tasks start
        FrameData*                 slots_        = nullptr;
        std::size_t                slotBytes_    = 0;       // the size of slots_, for the release
        std::atomic<std::int32_t>* prepared_     = nullptr; // per slot: the preparation of its frame is done
        std::atomic<std::int32_t>* searched_     = nullptr; // per slot: the search of its frame is done
        std::uint32_t              slotCount_    = 0;
        std::atomic<std::int64_t>* dispatchIdle_ = nullptr;
        std::atomic<std::int64_t>* prepareIdle_  = nullptr;
        std::atomic<std::int64_t>* resolveIdle_  = nullptr;
        std::atomic<std::int64_t>* searchIdle_   = nullptr;
        std::atomic<std::int64_t>* encodeIdle_   = nullptr;

        // 1. Dispatch
        alignas(64) std::atomic<std::int64_t> dispatched_{0};
        std::int64_t dispatchAcquired_ = 0;                  // frames acquired for dispatch; only Dispatch uses it
        alignas(64) std::atomic<bool> dispatchEnded_{false}; // no more frames will be dispatched
        // 2. Prepare and 4. Search: the frames their tasks have claimed
        alignas(64) std::atomic<std::int64_t> prepareClaimed_{0};
        alignas(64) std::atomic<std::int64_t> searchClaimed_{0};
        // 3. Resolve
        alignas(64) std::atomic<std::int64_t> resolved_{0};
        alignas(64) std::atomic<bool> resolveEnded_{false}; // all frames are resolved: the end for Search
        // 5. Encode
        alignas(64) std::atomic<std::int64_t> encoded_{0};
        std::int64_t encodeAcquired_ = 0; // frames acquired for encoding; only Encode uses it

        // one event per stage: what it waits for, or the end
        alignas(64) Synchronization::AutoResetEvent prepareReady_; // a frame to prepare; many waiters: the Prepare tasks
        Synchronization::AutoResetEvent   searchReady_;            // a frame to search; many waiters: the Search tasks
        Synchronization::AutoResetEvent_s resolveReady_;           // a preparation completed; one waiter: Resolve
        Synchronization::AutoResetEvent_s encodeReady_;            // a search completed; one waiter: Encode
        Synchronization::AutoResetEvent_s dispatchReady_;          // Encode freed a slot; one waiter: Dispatch
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
