// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "SchedulerHandle.h"

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    SchedulerHandle& SchedulerHandle::operator=(SchedulerHandle&& other) noexcept
    {
        if (this != &other) {
            Reset();
            scheduler_ = other.Release();
        }
        return *this;
    }

    SchedulerHandle SchedulerHandle::Create(SchedulerKind kind, std::uint32_t numberOfThreads, ITaskSwitchPolicy* taskSwitchPolicy) noexcept
    {
        SchedulerInfo info;
        info.numberOfThreads  = numberOfThreads;
        info.taskSwitchPolicy = taskSwitchPolicy;
        return SchedulerHandle(CreateTaskScheduler(kind, &info));
    }

    ITaskScheduler* SchedulerHandle::Release() noexcept
    {
        ITaskScheduler* scheduler = scheduler_;
        scheduler_                = nullptr;
        return scheduler;
    }

    void SchedulerHandle::Reset() noexcept
    {
        if (scheduler_ != nullptr)
            Release()->Destroy();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
