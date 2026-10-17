// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>
    /// Owns a scheduler from CreateTaskScheduler and destroys it at the end of its lifetime.
    /// ctLame runs two of them: the compute scheduler and the I/O scheduler. Start and Stop stay
    /// explicit, because the order in which the two are stopped matters.
    /// </summary>
    class SchedulerHandle
    {
    public:
        SchedulerHandle() noexcept = default;
        explicit SchedulerHandle(ITaskScheduler* scheduler) noexcept : scheduler_(scheduler) {}

        SchedulerHandle(SchedulerHandle&& other) noexcept : scheduler_(other.Release()) {}
        SchedulerHandle& operator=(SchedulerHandle&& other) noexcept;

        SchedulerHandle(const SchedulerHandle&)            = delete;
        SchedulerHandle& operator=(const SchedulerHandle&) = delete;

        ~SchedulerHandle() { Reset(); }

        /// <summary>Creates a scheduler; the handle is empty if the core rejects the arguments.</summary>
        static SchedulerHandle Create(SchedulerKind kind, std::uint32_t numberOfThreads, ITaskSwitchPolicy* taskSwitchPolicy = nullptr) noexcept;

        ITaskScheduler* Get() const noexcept { return scheduler_; }
        ITaskScheduler* operator->() const noexcept { return scheduler_; }
        explicit        operator bool() const noexcept { return scheduler_ != nullptr; }

        /// <summary>Gives up ownership without destroying the scheduler.</summary>
        ITaskScheduler* Release() noexcept;

        /// <summary>Destroys the scheduler, if any.</summary>
        void Reset() noexcept;

    private:
        ITaskScheduler* scheduler_ = nullptr;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
