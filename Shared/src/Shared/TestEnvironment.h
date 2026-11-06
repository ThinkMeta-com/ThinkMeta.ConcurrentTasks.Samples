// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <cstdlib>
#endif

#include "Text.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// The environment variables of the unit tests, the same scheme in every sample:
    /// <list type="bullet">
    /// <item>CT&lt;SAMPLE&gt;_TEST_&lt;WHAT&gt;: input of a correctness test that is too large or not free to ship (a file, a
    /// folder, or a list of files separated by ';'). Without it the test is skipped.</item>
    /// <item>CT&lt;SAMPLE&gt;_BENCH_&lt;WHAT&gt;: switches on a measurement (a file to measure, or any value). Measurements
    /// print numbers and never fail; without the variable they do not run.</item>
    /// </list>
    /// A skipped test writes "skipped: set NAME (what it is)" (SkipText) to the test log. Text in the samples' character
    /// type (Char).
    /// <para>Header only, for the test projects. Preliminary.</para>
    /// </summary>
    class TestEnvironment
    {
    public:
        /// <summary>The value of the variable; false and empty if it is not set, empty or does not fit.</summary>
        static bool Get(const Char* name, Char* value, std::uint32_t size) noexcept
        {
#if defined(_WIN32)
            const unsigned long length = GetEnvironmentVariableW(name, value, size); // DWORD of GetEnvironmentVariableW
#else
            const char*       text   = std::getenv(name);
            const std::size_t length = text != nullptr ? Text::Length(text) : 0;
            if (length > 0 && length < size)
                Text::Copy(value, size, text);
#endif
            if (length == 0 || length >= size) {
                if (size > 0)
                    value[0] = 0;
                return false;
            }
            return true;
        }

        template <std::size_t N>
        static bool Get(const Char* name, Char (&value)[N]) noexcept
        {
            return Get(name, value, static_cast<std::uint32_t>(N));
        }

        /// <summary>A folder: the value with a trailing path separator (\ on Windows, / on Linux).</summary>
        template <std::size_t N>
        static bool Folder(const Char* name, Char (&value)[N]) noexcept
        {
            if (!Get(name, value, static_cast<std::uint32_t>(N - 1)))
                return false;
#if defined(_WIN32)
            constexpr Char Separator = L'\\';
#else
            constexpr Char Separator = '/';
#endif
            const std::size_t length = Text::Length(value);
            if (value[length - 1] != Separator) {
                value[length]     = Separator;
                value[length + 1] = 0;
            }
            return true;
        }

        /// <summary>
        /// The next path of a list separated by ';' (from Get): copies it to path and moves list behind it. False at the
        /// end of the list; empty entries and paths that do not fit are passed over.
        /// </summary>
        template <std::size_t N>
        static bool NextPath(const Char*& list, Char (&path)[N]) noexcept
        {
            while (*list != 0) {
                std::size_t length = 0;
                while (list[length] != 0 && list[length] != ';')
                    ++length;
                const Char* entry = list;
                list += list[length] == ';' ? length + 1 : length;
                if (length == 0 || length >= N)
                    continue;
                for (std::size_t i = 0; i < length; ++i)
                    path[i] = entry[i];
                path[length] = 0;
                return true;
            }
            return false;
        }

        /// <summary>The log line of a skipped test: "skipped: set NAME (what)".</summary>
        template <std::size_t N>
        static const Char* SkipText(Char (&text)[N], const Char* name, const Char* what) noexcept
        {
            std::size_t pos    = 0;
            const auto  append = [&](const Char* s) noexcept {
                while (*s != 0 && pos + 1 < N)
                    text[pos++] = *s++;
            };
            append(CT_TEXT("skipped: set "));
            append(name);
            append(CT_TEXT(" ("));
            append(what);
            append(CT_TEXT(")"));
            text[pos] = 0;
            return text;
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
