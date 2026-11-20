// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "TaskPool.h"

#include <new>

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    std::shared_ptr<TaskPool> TaskPool::Create(std::uint32_t workers)
    {
        return Create(workers, workers, {});
    }

    std::shared_ptr<TaskPool> TaskPool::Create(std::uint32_t workers, std::uint32_t threads, ParallelRunner::StartOrder order,
                                               std::uint32_t taskBudget)
    {
        if (taskBudget > TaskQueueCapacity / 2)
            taskBudget = TaskQueueCapacity / 2;
        std::shared_ptr<TaskPool> pool(new (std::nothrow) TaskPool(workers, threads, order, taskBudget));
        if (!pool || (threads > 0 && !pool->scheduler_))
            return nullptr;
        return pool;
    }

    TaskPool::TaskPool(std::uint32_t workers, std::uint32_t threads, ParallelRunner::StartOrder order, std::uint32_t taskBudget) noexcept
        : converted_((ConvertThreadToTask(), true)), scheduler_(SchedulerKind::Default, threads, TaskQueueCapacity),
          runner_(threads > 0 ? scheduler_.Get() : nullptr, workers, taskBudget, order)
    {}

    TaskPool::~TaskPool()
    {
        scheduler_.Stop();
        if (converted_)
            ConvertTaskToThread();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
