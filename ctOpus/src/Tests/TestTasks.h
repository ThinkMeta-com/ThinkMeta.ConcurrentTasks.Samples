// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <utility>

#include "TaskRuntime.h"

// headers with imports last
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace OpusTests {

    namespace Ct = ThinkMeta::ConcurrentTasks;
    namespace Shared = ThinkMeta::ConcurrentTasks::Samples::Shared;

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

} // namespace OpusTests
