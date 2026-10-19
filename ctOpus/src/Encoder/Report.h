// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdarg>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>Receives printf-style messages of the library (the frontend writes them to stderr).</summary>
    using ReportFunction = void (*)(const wchar_t* format, std::va_list args);

    inline void Report(ReportFunction report, const wchar_t* format, ...)
    {
        if (report == nullptr)
            return;
        std::va_list args;
        va_start(args, format);
        report(format, args);
        va_end(args);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
