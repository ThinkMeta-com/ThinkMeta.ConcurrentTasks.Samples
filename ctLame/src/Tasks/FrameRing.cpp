// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "FrameRing.h"

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        constexpr auto Acquire = std::memory_order_acquire;
        constexpr auto Release = std::memory_order_release;

        std::atomic<std::int32_t>* NewFlags(std::uint32_t count) noexcept
        {
            void* memory = HeapAlloc(GetProcessHeap(), 0, count * sizeof(std::atomic<std::int32_t>));
            if (memory == nullptr)
                return nullptr;
            auto* flags = static_cast<std::atomic<std::int32_t>*>(memory);
            for (std::uint32_t i = 0; i < count; ++i)
                new (&flags[i]) std::atomic<std::int32_t>(0);
            return flags;
        }

    } // namespace

    FrameRing::~FrameRing()
    {
        if (slots_ != nullptr)
            VirtualFree(slots_, 0, MEM_RELEASE);
        if (prepared_ != nullptr)
            HeapFree(GetProcessHeap(), 0, prepared_);
        if (searched_ != nullptr)
            HeapFree(GetProcessHeap(), 0, searched_);
    }

    bool FrameRing::Initialize(std::uint32_t slotCount) noexcept
    {
        // zeroed memory, like the FrameData of the sequential encoder
        slots_
            = static_cast<FrameData*>(VirtualAlloc(nullptr, static_cast<std::size_t>(slotCount) * sizeof(FrameData), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        prepared_ = NewFlags(slotCount);
        searched_ = NewFlags(slotCount);
        if (slots_ == nullptr || prepared_ == nullptr || searched_ == nullptr)
            return false;
        slotCount_ = slotCount;
        for (std::uint32_t i = 0; i < slotCount; ++i)
            slots_[i].frameNumber = -1;
        return true;
    }

    FrameData& FrameRing::AcquireDispatch() noexcept
    {
        while (written_ - released_.load(Acquire) >= slotCount_)
            slotFree_.Wait();

        // the slot held frame written_ - slotCount_, which Encode has completed
        const std::uint32_t slot  = static_cast<std::uint32_t>(written_ % slotCount_);
        FrameData&          frame = slots_[slot];
        assert(frame.frameNumber == (written_ >= slotCount_ ? written_ - slotCount_ : -1));
        assert(prepared_[slot].load(Acquire) == 0 && searched_[slot].load(Acquire) == 0);
        frame.frameNumber = written_++;
        return frame;
    }

    void FrameRing::CompleteDispatch() noexcept
    {
        assert(committed_.load(std::memory_order_relaxed) < written_);
        committed_.store(committed_.load(std::memory_order_relaxed) + 1, Release);
        workReady_.Set();
    }

    void FrameRing::EndDispatch() noexcept
    {
        assert(committed_.load(std::memory_order_relaxed) == written_);
        ended_.store(true, Release);
        frameReady_.Set();
        frameSearched_.Set();
    }

    bool FrameRing::MoreWork() const noexcept
    {
        return searchClaimed_.load(Acquire) < resolved_.load(Acquire) || prepareClaimed_.load(Acquire) < committed_.load(Acquire);
    }

    FrameData* FrameRing::AcquireWork(Work& work) noexcept
    {
        for (;;) {
            // read the end mark first: once it is set, every frame has been resolved before it
            const bool resolveEnded = resolveEnded_.load(Acquire);

            // searches first: they belong to older frames and free slots sooner
            std::int64_t searchClaimed = searchClaimed_.load(Acquire);
            if (searchClaimed < resolved_.load(Acquire)) {
                if (!searchClaimed_.compare_exchange_strong(searchClaimed, searchClaimed + 1))
                    continue; // another worker claimed it
                if (MoreWork())
                    workReady_.Set(); // one Set wakes one waiting worker
                work             = Work::Search;
                FrameData& frame = slots_[searchClaimed % slotCount_];
                assert(frame.frameNumber == searchClaimed);
                return &frame;
            }

            std::int64_t prepareClaimed = prepareClaimed_.load(Acquire);
            if (prepareClaimed < committed_.load(Acquire)) {
                if (!prepareClaimed_.compare_exchange_strong(prepareClaimed, prepareClaimed + 1))
                    continue;
                if (MoreWork())
                    workReady_.Set();
                work             = Work::Prepare;
                FrameData& frame = slots_[prepareClaimed % slotCount_];
                assert(frame.frameNumber == prepareClaimed);
                return &frame;
            }

            if (resolveEnded) {
                workReady_.Set(); // pass the end on to the next waiting worker
                return nullptr;
            }
            workReady_.Wait();
        }
    }

    void FrameRing::CompleteWork(const FrameData& frame, Work work) noexcept
    {
        const std::uint32_t slot = static_cast<std::uint32_t>(frame.frameNumber % slotCount_);
        if (work == Work::Prepare) {
            assert(prepared_[slot].load(std::memory_order_relaxed) == 0);
            prepared_[slot].store(1, Release);
            frameReady_.Set();
        } else {
            assert(searched_[slot].load(std::memory_order_relaxed) == 0);
            searched_[slot].store(1, Release);
            frameSearched_.Set();
        }
    }

    FrameData* FrameRing::AcquireResolve() noexcept
    {
        const std::int64_t  next = resolved_.load(std::memory_order_relaxed); // only Resolve moves it
        const std::uint32_t slot = static_cast<std::uint32_t>(next % slotCount_);
        for (;;) {
            // read the end mark first: once it is set, every frame has been dispatched before it
            const bool ended = ended_.load(Acquire);
            if (next < committed_.load(Acquire)) {
                if (prepared_[slot].load(Acquire) != 0)
                    break;
            } else if (ended) {
                resolveEnded_.store(true, Release);
                workReady_.Set();
                return nullptr;
            }
            frameReady_.Wait();
        }
        FrameData& frame = slots_[slot];
        assert(frame.frameNumber == next);
        return &frame;
    }

    void FrameRing::CompleteResolve() noexcept
    {
        resolved_.store(resolved_.load(std::memory_order_relaxed) + 1, Release);
        workReady_.Set();
    }

    FrameData* FrameRing::AcquireEncode() noexcept
    {
        const std::uint32_t slot = static_cast<std::uint32_t>(read_ % slotCount_);
        for (;;) {
            // read the end mark first: once it is set, every frame has been dispatched before it
            const bool ended = ended_.load(Acquire);
            if (read_ < committed_.load(Acquire)) {
                if (searched_[slot].load(Acquire) != 0)
                    break;
            } else if (ended) {
                return nullptr;
            }
            frameSearched_.Wait();
        }
        FrameData& frame = slots_[slot];
        assert(frame.frameNumber == read_);
        ++read_;
        return &frame;
    }

    void FrameRing::CompleteEncode() noexcept
    {
        const std::int64_t released = released_.load(std::memory_order_relaxed); // only Encode moves it
        assert(released < read_);
        const std::uint32_t slot = static_cast<std::uint32_t>(released % slotCount_);
        prepared_[slot].store(0, Release);
        searched_[slot].store(0, Release);
        released_.store(released + 1, Release);
        slotFree_.Set();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
