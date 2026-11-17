// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "FrameRing.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "Memory.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        std::atomic<std::int32_t>* NewMarks(std::uint32_t count) noexcept
        {
            void* memory = Shared::Heap::Allocate(count * sizeof(std::atomic<std::int32_t>));
            if (memory == nullptr)
                return nullptr;
            auto* marks = static_cast<std::atomic<std::int32_t>*>(memory);
            for (std::uint32_t i = 0; i < count; ++i)
                new (&marks[i]) std::atomic<std::int32_t>(0);
            return marks;
        }

    } // namespace

    FrameRing::~FrameRing()
    {
        Shared::AddressSpace::Release(slots_, slotBytes_);
        Shared::Heap::Free(prepared_);
        Shared::Heap::Free(searched_);
    }

    bool FrameRing::Initialize(std::uint32_t slotCount) noexcept
    {
        // zeroed memory, like the FrameData of the sequential encoder
        slotBytes_ = static_cast<std::size_t>(slotCount) * sizeof(FrameData);
        slots_     = static_cast<FrameData*>(Shared::AddressSpace::Allocate(slotBytes_));
        prepared_  = NewMarks(slotCount);
        searched_  = NewMarks(slotCount);
        if (slots_ == nullptr || prepared_ == nullptr || searched_ == nullptr)
            return false;
        slotCount_ = slotCount;
        for (std::uint32_t i = 0; i < slotCount; ++i)
            slots_[i].frameNumber = -1;
        return true;
    }

    // --- 1. Dispatch

    FrameData& FrameRing::AcquireDispatch() noexcept
    {
        while (dispatchAcquired_ - encoded_.load(std::memory_order_acquire) >= slotCount_) {
            const ProfileScope idle(dispatchIdle_);
            dispatchReady_.Wait();
        }

        // the slot held frame dispatchAcquired_ - slotCount_, which Encode has completed
        const std::uint32_t slot  = static_cast<std::uint32_t>(dispatchAcquired_ % slotCount_);
        FrameData&          frame = slots_[slot];
        assert(frame.frameNumber == (dispatchAcquired_ >= slotCount_ ? dispatchAcquired_ - slotCount_ : -1));
        assert(prepared_[slot].load(std::memory_order_acquire) == 0 && searched_[slot].load(std::memory_order_acquire) == 0);
        frame.frameNumber = dispatchAcquired_++;
        return frame;
    }

    void FrameRing::CompleteDispatch() noexcept
    {
        assert(dispatched_.load(std::memory_order_relaxed) < dispatchAcquired_);
        dispatched_.store(dispatched_.load(std::memory_order_relaxed) + 1, std::memory_order_release); // only Dispatch moves it
        prepareReady_.Set();
    }

    void FrameRing::EndDispatch() noexcept
    {
        assert(dispatched_.load(std::memory_order_relaxed) == dispatchAcquired_);
        dispatchEnded_.store(true, std::memory_order_release);
        // every stage that tells its end from the dispatched frames; Search hears it from Resolve
        prepareReady_.Set();
        resolveReady_.Set();
        encodeReady_.Set();
    }

    // --- 2. Prepare

    FrameData* FrameRing::AcquirePrepare() noexcept
    {
        for (;;) {
            // read the end mark first: once it is set, every frame has been dispatched before it
            const bool ended = dispatchEnded_.load(std::memory_order_acquire);

            std::int64_t       claimed    = prepareClaimed_.load(std::memory_order_acquire);
            const std::int64_t dispatched = dispatched_.load(std::memory_order_acquire);
            if (claimed < dispatched) {
                if (!prepareClaimed_.compare_exchange_strong(claimed, claimed + 1))
                    continue; // another task claimed it
                if (claimed + 1 < dispatched)
                    prepareReady_.Set(); // more are left: wake the next task (Sets merge, one Set wakes one task)
                FrameData& frame = slots_[claimed % slotCount_];
                assert(frame.frameNumber == claimed);
                return &frame;
            }

            if (ended) {
                prepareReady_.Set(); // pass the end on to the next waiting task
                return nullptr;
            }
            const ProfileScope idle(prepareIdle_);
            prepareReady_.Wait();
        }
    }

    void FrameRing::CompletePrepare(const FrameData& frame) noexcept
    {
        const std::uint32_t slot = static_cast<std::uint32_t>(frame.frameNumber % slotCount_);
        assert(prepared_[slot].load(std::memory_order_relaxed) == 0);
        prepared_[slot].store(1, std::memory_order_release);
        resolveReady_.Set();
    }

    // --- 3. Resolve

    FrameData* FrameRing::AcquireResolve() noexcept
    {
        const std::int64_t  next = resolved_.load(std::memory_order_relaxed); // only Resolve moves it
        const std::uint32_t slot = static_cast<std::uint32_t>(next % slotCount_);
        for (;;) {
            // read the end mark first: once it is set, every frame has been dispatched before it
            const bool ended = dispatchEnded_.load(std::memory_order_acquire);
            if (next < dispatched_.load(std::memory_order_acquire)) {
                if (prepared_[slot].load(std::memory_order_acquire) != 0)
                    break; // the frame whose turn it is has been prepared
            } else if (ended) {
                // Search may only take resolved frames: it hears the end from here
                resolveEnded_.store(true, std::memory_order_release);
                searchReady_.Set();
                return nullptr;
            }
            const ProfileScope idle(resolveIdle_);
            resolveReady_.Wait();
        }
        FrameData& frame = slots_[slot];
        assert(frame.frameNumber == next);
        return &frame;
    }

    void FrameRing::CompleteResolve() noexcept
    {
        resolved_.store(resolved_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        searchReady_.Set();
    }

    // --- 4. Search

    FrameData* FrameRing::AcquireSearch() noexcept
    {
        for (;;) {
            // read the end mark first: once it is set, every frame has been resolved before it
            const bool ended = resolveEnded_.load(std::memory_order_acquire);

            std::int64_t       claimed  = searchClaimed_.load(std::memory_order_acquire);
            const std::int64_t resolved = resolved_.load(std::memory_order_acquire);
            if (claimed < resolved) {
                if (!searchClaimed_.compare_exchange_strong(claimed, claimed + 1))
                    continue; // another task claimed it
                if (claimed + 1 < resolved)
                    searchReady_.Set(); // more are left: wake the next task (Sets merge, one Set wakes one task)
                FrameData& frame = slots_[claimed % slotCount_];
                assert(frame.frameNumber == claimed);
                return &frame;
            }

            if (ended) {
                searchReady_.Set(); // pass the end on to the next waiting task
                return nullptr;
            }
            const ProfileScope idle(searchIdle_);
            searchReady_.Wait();
        }
    }

    void FrameRing::CompleteSearch(const FrameData& frame) noexcept
    {
        const std::uint32_t slot = static_cast<std::uint32_t>(frame.frameNumber % slotCount_);
        assert(searched_[slot].load(std::memory_order_relaxed) == 0);
        searched_[slot].store(1, std::memory_order_release);
        encodeReady_.Set();
    }

    // --- 5. Encode

    FrameData* FrameRing::AcquireEncode() noexcept
    {
        const std::uint32_t slot = static_cast<std::uint32_t>(encodeAcquired_ % slotCount_);
        for (;;) {
            // read the end mark first: once it is set, every frame has been dispatched before it
            const bool ended = dispatchEnded_.load(std::memory_order_acquire);
            if (encodeAcquired_ < dispatched_.load(std::memory_order_acquire)) {
                if (searched_[slot].load(std::memory_order_acquire) != 0)
                    break; // the frame whose turn it is has been searched
            } else if (ended) {
                return nullptr;
            }
            const ProfileScope idle(encodeIdle_);
            encodeReady_.Wait();
        }
        FrameData& frame = slots_[slot];
        assert(frame.frameNumber == encodeAcquired_);
        ++encodeAcquired_;
        return &frame;
    }

    void FrameRing::CompleteEncode() noexcept
    {
        const std::int64_t encoded = encoded_.load(std::memory_order_relaxed); // only Encode moves it
        assert(encoded < encodeAcquired_);
        const std::uint32_t slot = static_cast<std::uint32_t>(encoded % slotCount_);
        prepared_[slot].store(0, std::memory_order_release);
        searched_[slot].store(0, std::memory_order_release);
        encoded_.store(encoded + 1, std::memory_order_release);
        dispatchReady_.Set();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
