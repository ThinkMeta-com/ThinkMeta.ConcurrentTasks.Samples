// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    /// <summary>Owns a scheduler from CreateTaskScheduler and destroys it at the end of its lifetime.</summary>
    class SchedulerHandle
    {
    public:
        SchedulerHandle() noexcept = default;
        explicit SchedulerHandle(ITaskScheduler* scheduler) noexcept : scheduler_(scheduler) {}
        SchedulerHandle(SchedulerHandle&& other) noexcept : scheduler_(other.Release()) {}
        SchedulerHandle& operator=(SchedulerHandle&& other) noexcept
        {
            if (this != &other) {
                Reset();
                scheduler_ = other.Release();
            }
            return *this;
        }
        SchedulerHandle(const SchedulerHandle&)            = delete;
        SchedulerHandle& operator=(const SchedulerHandle&) = delete;
        ~SchedulerHandle() { Reset(); }

        static SchedulerHandle Create(SchedulerKind kind, std::uint32_t numberOfThreads) noexcept
        {
            SchedulerInfo info;
            info.numberOfThreads = numberOfThreads;
            return SchedulerHandle(CreateTaskScheduler(kind, &info));
        }

        ITaskScheduler* operator->() const noexcept { return scheduler_; }
        explicit        operator bool() const noexcept { return scheduler_ != nullptr; }

        ITaskScheduler* Release() noexcept
        {
            ITaskScheduler* scheduler = scheduler_;
            scheduler_                = nullptr;
            return scheduler;
        }

        void Reset() noexcept
        {
            if (scheduler_ != nullptr)
                Release()->Destroy();
        }

    private:
        ITaskScheduler* scheduler_ = nullptr;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
