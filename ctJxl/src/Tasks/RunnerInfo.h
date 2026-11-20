// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

// Included by the libjxl copies (ctJxl.Encoder, C++17): no ConcurrentTasks or libjxl-internal types here, only the
// public runner type of libjxl (plan section 4.6, project boundary).
#include <jxl/parallel_runner.h>

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    /// <summary>
    /// Whether a region on runner may itself run regions that fork workers: true for the ConcurrentTasks runner with at
    /// least two workers. libjxl's own thread pool rejects a region inside a region (plan D4.2).
    /// <para>Preliminary.</para>
    /// </summary>
    [[nodiscard]] bool RunnerNestsRegions(JxlParallelRunner runner, void* runnerOpaque) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl