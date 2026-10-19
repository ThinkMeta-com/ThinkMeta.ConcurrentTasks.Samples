// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

#include <Windows.h>

#include <Psapi.h>

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// Times and memory for the statistics a sample prints on request (--stats). With the preprocessor definition
    /// CT_SAMPLES_NO_TIMING the time functions return 0 and read no clock (QueryPerformanceCounter, the process times).
    /// <para>Header only, so each program decides on CT_SAMPLES_NO_TIMING. Preliminary.</para>
    /// </summary>
    class Diagnostics
    {
    public:
        /// <summary>Seconds of the performance counter; only differences have a meaning.</summary>
        static double Now() noexcept
        {
#ifdef CT_SAMPLES_NO_TIMING
            return 0.0;
#else
            LARGE_INTEGER counter, frequency;
            QueryPerformanceCounter(&counter);
            QueryPerformanceFrequency(&frequency);
            return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
#endif
        }

        /// <summary>Seconds from the creation of this process to now: the start-up before the program's own code.</summary>
        static double SinceProcessStart() noexcept
        {
#ifdef CT_SAMPLES_NO_TIMING
            return 0.0;
#else
            FILETIME creation, exitTime, kernel, user, now;
            if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user))
                return 0.0;
            GetSystemTimePreciseAsFileTime(&now);
            const auto ticks = [](const FILETIME& f) noexcept { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
            return static_cast<double>(ticks(now) - ticks(creation)) * 1e-7;
#endif
        }

        /// <summary>The peak working set of this process in MB (10^6 bytes).</summary>
        static double PeakMemoryMB() noexcept
        {
            PROCESS_MEMORY_COUNTERS counters{};
            counters.cb = sizeof(counters);
            return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? static_cast<double>(counters.PeakWorkingSetSize) / 1e6 : 0.0;
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
