// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

#if defined(_WIN32)
#    include <Windows.h>

#    include <Psapi.h>
#else
#    include <ctime>
#    include <fcntl.h>
#    include <sys/resource.h>
#    include <unistd.h>
#endif

#include "Text.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// Times and memory for the statistics a sample prints on request (--stats). With the preprocessor definition
    /// CT_SAMPLES_NO_TIMING the time functions return 0 and read no clock (Windows: QueryPerformanceCounter, the process
    /// times; Linux: clock_gettime, /proc/self/stat).
    /// <para>Header only, so each program decides on CT_SAMPLES_NO_TIMING. Preliminary.</para>
    /// </summary>
    class Diagnostics
    {
    public:
        /// <summary>Seconds of a monotonic clock; only differences have a meaning.</summary>
        static double Now() noexcept
        {
#if defined(CT_SAMPLES_NO_TIMING)
            return 0.0;
#elif defined(_WIN32)
            LARGE_INTEGER counter, frequency;
            QueryPerformanceCounter(&counter);
            QueryPerformanceFrequency(&frequency);
            return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
#else
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
#endif
        }

        /// <summary>Seconds from the creation of this process to now: the start-up before the program's own code.</summary>
        static double SinceProcessStart() noexcept
        {
#if defined(CT_SAMPLES_NO_TIMING)
            return 0.0;
#elif defined(_WIN32)
            FILETIME creation, exitTime, kernel, user, now;
            if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user))
                return 0.0;
            GetSystemTimePreciseAsFileTime(&now);
            const auto ticks = [](const FILETIME& f) noexcept { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
            return static_cast<double>(ticks(now) - ticks(creation)) * 1e-7;
#else
            // field 22 of /proc/self/stat: the start time in clock ticks after boot (CLOCK_BOOTTIME); the command name
            // in field 2 is in parentheses and may hold spaces, so the fields are counted after its last ')'
            char      text[1024];
            const int file = ::open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
            if (file < 0)
                return 0.0;
            const ssize_t length = ::read(file, text, sizeof(text) - 1);
            ::close(file);
            if (length <= 0)
                return 0.0;
            text[length]  = 0;
            const char* p = nullptr;
            for (const char* c = text; *c != 0; ++c)
                if (*c == ')')
                    p = c;
            if (p == nullptr)
                return 0.0;
            for (std::int32_t field = 2; field < 22 && *p != 0; ++p)
                if (*p == ' ')
                    ++field;
            const double startTicks = static_cast<double>(Text::ToUInt64(p));
            timespec     now;
            clock_gettime(CLOCK_BOOTTIME, &now);
            const double seconds = static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
            return seconds - startTicks / static_cast<double>(sysconf(_SC_CLK_TCK));
#endif
        }

        /// <summary>The peak working set (Windows) or peak resident set (Linux) of this process in MB (10^6 bytes).</summary>
        static double PeakMemoryMB() noexcept
        {
#if defined(_WIN32)
            PROCESS_MEMORY_COUNTERS counters{};
            counters.cb = sizeof(counters);
            return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? static_cast<double>(counters.PeakWorkingSetSize) / 1e6 : 0.0;
#else
            rusage usage{};
            return getrusage(RUSAGE_SELF, &usage) == 0 ? static_cast<double>(usage.ru_maxrss) * 1024.0 / 1e6 : 0.0;
#endif
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
