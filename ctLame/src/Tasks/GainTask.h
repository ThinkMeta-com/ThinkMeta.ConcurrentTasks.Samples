// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#if defined(_WIN32)
#    include <Windows.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "NodeProfile.h"
#include "SampleStream.h"

// headers with imports last
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// The edge between Dispatch, the gain tasks and Encode: Dispatch wakes the gain tasks when the sample stream has
    /// a new piece for them (or has ended), Encode waits for all gain tasks before it writes the LAME tag. Each event
    /// has one waiter (the _s variant).
    /// </summary>
    class GainSignals
    {
    public:
        explicit GainSignals(std::int32_t tasks) noexcept : pending_(tasks), tasks_(tasks) {}

        GainSignals(const GainSignals&)            = delete;
        GainSignals& operator=(const GainSignals&) = delete;

        /// <summary>Dispatch: the stream has grown; wakes the gain tasks once a piece more is there, or at the end.</summary>
        void Publish(const SampleStream& stream, bool ended) noexcept;

        /// <summary>A gain task: waits until Publish wakes it.</summary>
        void WaitForStream(std::int32_t task) noexcept { wake_[task].Wait(); }

        /// <summary>A gain task has analyzed its channel to the end.</summary>
        void TaskDone() noexcept
        {
            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                done_.Set();
        }

        /// <summary>Encode: waits until all gain tasks are done.</summary>
        void WaitDone() noexcept
        {
            while (pending_.load(std::memory_order_acquire) > 0)
                done_.Wait();
        }

    private:
        /// <summary>The pieces Publish waits for before it wakes the gain tasks.</summary>
        static constexpr std::uint64_t WakeBatch = 32;

        std::atomic<std::int32_t>                        pending_;
        std::int32_t                                     tasks_;
        std::uint64_t                                    signaled_ = 0; // Dispatch: the pieces the tasks were woken for
        std::array<Synchronization::AutoResetEvent_s, 2> wake_;
        Synchronization::AutoResetEvent_s                done_;
    };

    /// <summary>
    /// Node Gain of the parallel encoder graph (EncodeGraph.h), one per channel: the ReplayGain analysis of its channel
    /// (ChannelGain), in LAME's pieces (SampleStream::GainPiece) as the sample stream grows. The channels need nothing
    /// from each other until the end, where Encode joins their window sums (SampleStream::FinishGain); so the analysis,
    /// a serial chain of two filters, runs beside the frame stages instead of in Dispatch.
    /// </summary>
    class GainTask final : public Tasks::Task
    {
    public:
        GainTask(SampleStream& stream, std::int32_t channel, GainSignals& signals, TaskProfile* profile) noexcept
            : stream_(stream),
              channel_(channel),
              signals_(signals),
              profile_(profile)
        {}

        void Run() noexcept override;

    private:
        SampleStream& stream_;
        std::int32_t  channel_;
        GainSignals&  signals_;
        TaskProfile*  profile_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
