// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "SchedulerHandle.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// A started scheduler: created and started by the constructor, stopped and destroyed by Stop or the destructor.
    /// Every task on it must have exited by then (the task graphs wait for their tasks before they return). Without
    /// threads (0) it stays empty.
    /// <para>Header only, like SchedulerHandle. Preliminary.</para>
    /// </summary>
    class RunningScheduler
    {
    public:
        RunningScheduler(SchedulerKind kind, std::uint32_t numberOfThreads) noexcept
            : handle_(numberOfThreads > 0 ? SchedulerHandle::Create(kind, numberOfThreads) : SchedulerHandle())
        {
            if (handle_)
                handle_->Start();
        }

        RunningScheduler(const RunningScheduler&)            = delete;
        RunningScheduler& operator=(const RunningScheduler&) = delete;

        ~RunningScheduler() { Stop(); }

        /// <summary>Stops and destroys the scheduler, if it is still running.</summary>
        void Stop() noexcept
        {
            if (handle_) {
                handle_->Stop();
                handle_.Reset();
            }
        }

        /// <summary>False if the core rejected the scheduler (or after Stop).</summary>
        explicit operator bool() const noexcept { return static_cast<bool>(handle_); }

        ITaskScheduler&  operator*() const noexcept { return *handle_.Get(); }
        ITaskScheduler*  operator->() const noexcept { return handle_.Get(); }
        ITaskScheduler*  Get() const noexcept { return handle_.Get(); }
        SchedulerHandle& Handle() noexcept { return handle_; }

    private:
        SchedulerHandle handle_;
    };

    /// <summary>
    /// The setup of ThinkMeta.ConcurrentTasks, the same in every sample:
    /// <list type="number">
    /// <item>ConvertThreadToTask: the calling thread becomes a task, so it can wait for the task graph (TaskGroup::Wait)
    /// and on its events.</item>
    /// <item>EnableAsyncIo: the core's completion port, for the overlapped file I/O of the nodes that read and write.</item>
    /// <item>An I/O scheduler for those nodes, started. The compute scheduler is a RunningScheduler of the sample: some
    /// samples create it only once their graph knows its thread count.</item>
    /// </list>
    /// End or the destructor undoes the steps in reverse order; every task must have exited by then.
    /// <para>Header only, like SchedulerHandle. Preliminary.</para>
    /// </summary>
    class TaskRuntime
    {
    public:
        /// <summary>The setup with an I/O scheduler of ioThreads threads; 0: none (a test, a graph without file I/O).</summary>
        explicit TaskRuntime(std::uint32_t ioThreads) noexcept
            : converted_((ConvertThreadToTask(), true)),
              asyncIo_(AsyncIo::EnableAsyncIo()),
              ioThreads_(ioThreads),
              io_(SchedulerKind::Default, ioThreads)
        {}

        TaskRuntime(const TaskRuntime&)            = delete;
        TaskRuntime& operator=(const TaskRuntime&) = delete;

        ~TaskRuntime() { End(); }

        /// <summary>True if every step of the setup worked.</summary>
        explicit operator bool() const noexcept { return asyncIo_ && (ioThreads_ == 0 || static_cast<bool>(io_)); }

        /// <summary>The I/O scheduler.</summary>
        RunningScheduler& Io() noexcept { return io_; }

        /// <summary>Undoes the setup now instead of in the destructor: stops the I/O scheduler, disables async I/O and turns the task back into a
        /// thread.</summary>
        void End() noexcept
        {
            io_.Stop();
            if (asyncIo_) {
                AsyncIo::DisableAsyncIo();
                asyncIo_ = false;
            }
            if (converted_) {
                ConvertTaskToThread();
                converted_ = false;
            }
        }

        /// <summary>The default number of worker threads: one per logical processor in all processor groups.</summary>
        static std::uint32_t LogicalProcessors() noexcept { return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); }

    private:
        bool             converted_;
        bool             asyncIo_;
        std::uint32_t    ioThreads_;
        RunningScheduler io_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
