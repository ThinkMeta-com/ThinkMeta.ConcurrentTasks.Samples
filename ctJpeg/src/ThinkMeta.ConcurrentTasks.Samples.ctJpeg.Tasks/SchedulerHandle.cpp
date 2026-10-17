// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "SchedulerHandle.h"

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

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

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
