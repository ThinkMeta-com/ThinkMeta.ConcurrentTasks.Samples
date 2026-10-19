// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// Owns a scheduler from CreateTaskScheduler and destroys it at the end of its lifetime. Start and Stop stay
    /// explicit: a sample starts its schedulers before the task graph runs and stops them after the graph has ended,
    /// so every task has exited before Stop.
    /// <para>Header only: it uses the core of ThinkMeta.ConcurrentTasks, which the library of Shared does not.</para>
    /// <para>Preliminary.</para>
    /// </summary>
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

        /// <summary>Creates a scheduler with numberOfThreads threads; the handle is empty if the core rejects the arguments.</summary>
        static SchedulerHandle Create(SchedulerKind kind, std::uint32_t numberOfThreads) noexcept
        {
            SchedulerInfo info;
            info.numberOfThreads = numberOfThreads;
            return SchedulerHandle(CreateTaskScheduler(kind, &info));
        }

        ITaskScheduler* Get() const noexcept { return scheduler_; }
        ITaskScheduler* operator->() const noexcept { return scheduler_; }
        explicit        operator bool() const noexcept { return scheduler_ != nullptr; }

        /// <summary>Gives up ownership without destroying the scheduler.</summary>
        ITaskScheduler* Release() noexcept
        {
            ITaskScheduler* scheduler = scheduler_;
            scheduler_                = nullptr;
            return scheduler;
        }

        /// <summary>Destroys the scheduler, if any.</summary>
        void Reset() noexcept
        {
            if (scheduler_ != nullptr)
                Release()->Destroy();
        }

    private:
        ITaskScheduler* scheduler_ = nullptr;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
