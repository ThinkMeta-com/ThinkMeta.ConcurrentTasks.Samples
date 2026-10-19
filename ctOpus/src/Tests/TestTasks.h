// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <utility>

#include "SchedulerHandle.h"

// headers with imports last
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ctOpusTests {

    namespace Ct = ThinkMeta::ConcurrentTasks;
    using ThinkMeta::ConcurrentTasks::Samples::ctOpus::SchedulerHandle;

    /// <summary>A task that runs a function (the tests' stand-ins for the graph's nodes).</summary>
    template <typename Function>
    class FunctionTask final : public Ct::Tasks::Task
    {
    public:
        explicit FunctionTask(Function function) noexcept : function_(std::move(function)) {}

        void Run() noexcept override { function_(); }

    private:
        Function function_;
    };

    /// <summary>
    /// The setup of every sample, for one test: this thread as a task (it waits for the group of the
    /// test's tasks) and a compute scheduler.
    /// </summary>
    class TestSchedulers
    {
    public:
        explicit TestSchedulers(std::uint32_t computeThreads) noexcept
        {
            Ct::ConvertThreadToTask();
            io_      = SchedulerHandle::Create(Ct::SchedulerKind::Default, 1);
            compute_ = SchedulerHandle::Create(Ct::SchedulerKind::Default, computeThreads);
            if (io_)
                io_->Start();
            if (compute_)
                compute_->Start();
        }
        ~TestSchedulers()
        {
            if (compute_)
                compute_->Stop();
            if (io_)
                io_->Stop();
            compute_.Reset();
            io_.Reset();
            Ct::ConvertTaskToThread();
        }

        TestSchedulers(const TestSchedulers&)            = delete;
        TestSchedulers& operator=(const TestSchedulers&) = delete;

        bool Valid() const noexcept { return io_ && compute_; }

        SchedulerHandle& Io() noexcept { return io_; }
        SchedulerHandle& Compute() noexcept { return compute_; }

    private:
        SchedulerHandle io_;
        SchedulerHandle compute_;
    };

} // namespace ctOpusTests
