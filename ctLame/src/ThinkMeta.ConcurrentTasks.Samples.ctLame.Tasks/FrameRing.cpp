// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "FrameRing.h"

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    FrameRing::~FrameRing()
    {
        if (slots_ != nullptr)
            VirtualFree(slots_, 0, MEM_RELEASE);
        if (prepared_ != nullptr)
            HeapFree(GetProcessHeap(), 0, const_cast<long*>(prepared_));
        if (searched_ != nullptr)
            HeapFree(GetProcessHeap(), 0, const_cast<long*>(searched_));
    }

    bool FrameRing::Initialize(std::uint32_t slotCount) noexcept
    {
        // zeroed memory, like the FrameData of the sequential encoder
        slots_
            = static_cast<FrameData*>(VirtualAlloc(nullptr, static_cast<std::size_t>(slotCount) * sizeof(FrameData), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        prepared_ = static_cast<volatile long*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, slotCount * sizeof(long)));
        searched_ = static_cast<volatile long*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, slotCount * sizeof(long)));
        if (slots_ == nullptr || prepared_ == nullptr || searched_ == nullptr)
            return false;
        slotCount_ = slotCount;
        for (std::uint32_t i = 0; i < slotCount; ++i)
            slots_[i].frameNumber = -1;
        return true;
    }

    FrameData& FrameRing::AcquireDispatch() noexcept
    {
        while (written_ - ReadAcquire64(&released_) >= slotCount_)
            slotFree_.Wait();

        // the slot held frame written_ - slotCount_, which Encode has completed
        const std::uint32_t slot  = static_cast<std::uint32_t>(written_ % slotCount_);
        FrameData&          frame = slots_[slot];
        assert(frame.frameNumber == (written_ >= slotCount_ ? written_ - slotCount_ : -1));
        assert(prepared_[slot] == 0 && searched_[slot] == 0);
        frame.frameNumber = written_++;
        return frame;
    }

    void FrameRing::CompleteDispatch() noexcept
    {
        assert(committed_ < written_);
        WriteRelease64(&committed_, committed_ + 1);
        workReady_.Set();
    }

    void FrameRing::EndDispatch() noexcept
    {
        assert(committed_ == written_);
        WriteRelease(&ended_, 1);
        frameReady_.Set();
        frameSearched_.Set();
    }

    bool FrameRing::MoreWork() const noexcept
    {
        return ReadAcquire64(&searchClaimed_) < ReadAcquire64(&resolved_) || ReadAcquire64(&prepareClaimed_) < ReadAcquire64(&committed_);
    }

    FrameData* FrameRing::AcquireWork(Work& work) noexcept
    {
        for (;;) {
            // read the end mark first: once it is set, every frame has been resolved before it
            const bool resolveEnded = ReadAcquire(&resolveEnded_) != 0;

            // searches first: they belong to older frames and free slots sooner
            const std::int64_t searchClaimed = ReadAcquire64(&searchClaimed_);
            if (searchClaimed < ReadAcquire64(&resolved_)) {
                if (InterlockedCompareExchange64(&searchClaimed_, searchClaimed + 1, searchClaimed) != searchClaimed)
                    continue; // another worker claimed it
                if (MoreWork())
                    workReady_.Set(); // one Set wakes one waiting worker
                work             = Work::Search;
                FrameData& frame = slots_[searchClaimed % slotCount_];
                assert(frame.frameNumber == searchClaimed);
                return &frame;
            }

            const std::int64_t prepareClaimed = ReadAcquire64(&prepareClaimed_);
            if (prepareClaimed < ReadAcquire64(&committed_)) {
                if (InterlockedCompareExchange64(&prepareClaimed_, prepareClaimed + 1, prepareClaimed) != prepareClaimed)
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
            assert(prepared_[slot] == 0);
            WriteRelease(&prepared_[slot], 1);
            frameReady_.Set();
        } else {
            assert(searched_[slot] == 0);
            WriteRelease(&searched_[slot], 1);
            frameSearched_.Set();
        }
    }

    FrameData* FrameRing::AcquireResolve() noexcept
    {
        const std::int64_t  next = resolved_;
        const std::uint32_t slot = static_cast<std::uint32_t>(next % slotCount_);
        for (;;) {
            // read the end mark first: once it is set, every frame has been dispatched before it
            const bool ended = ReadAcquire(&ended_) != 0;
            if (next < ReadAcquire64(&committed_)) {
                if (ReadAcquire(&prepared_[slot]) != 0)
                    break;
            } else if (ended) {
                WriteRelease(&resolveEnded_, 1);
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
        WriteRelease64(&resolved_, resolved_ + 1);
        workReady_.Set();
    }

    FrameData* FrameRing::AcquireEncode() noexcept
    {
        const std::uint32_t slot = static_cast<std::uint32_t>(read_ % slotCount_);
        for (;;) {
            // read the end mark first: once it is set, every frame has been dispatched before it
            const bool ended = ReadAcquire(&ended_) != 0;
            if (read_ < ReadAcquire64(&committed_)) {
                if (ReadAcquire(&searched_[slot]) != 0)
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
        assert(released_ < read_);
        const std::uint32_t slot = static_cast<std::uint32_t>(released_ % slotCount_);
        WriteRelease(&prepared_[slot], 0);
        WriteRelease(&searched_[slot], 0);
        WriteRelease64(&released_, released_ + 1);
        slotFree_.Set();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
