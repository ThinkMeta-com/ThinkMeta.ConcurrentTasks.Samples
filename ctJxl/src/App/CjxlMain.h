// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

// The entry point of cjxl inside ctJxl (CjxlMain.cc, a marked copy of libjxl's tools/cjxl_main.cc) and the
// parallel runner it gets from ctJxl instead of libjxl's own thread pool.

#pragma once

#include <jxl/parallel_runner.h>

#include <cstddef>
#include <functional>
#include <memory>

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    /// <summary>A parallel runner for libjxl: the function and its opaque state, which the holder keeps alive.</summary>
    struct CjxlRunner {
        JxlParallelRunner     runner = nullptr;
        std::shared_ptr<void> opaque;
    };

    /// <summary>Creates the runner for the number of worker threads cjxl would use (--num_threads; 0: none).</summary>
    using CjxlRunnerFactory = std::function<CjxlRunner(std::size_t numWorkerThreads)>;

    /// <summary>cjxl's main: the arguments as UTF-8, without the program's own options.</summary>
    int CjxlMain(int argc, char** argv, const CjxlRunnerFactory& makeRunner);

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
