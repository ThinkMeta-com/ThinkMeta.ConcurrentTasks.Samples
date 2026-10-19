// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>

#include <Windows.h>

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// The environment variables of the unit tests, the same scheme in every sample:
    /// <list type="bullet">
    /// <item>CT&lt;SAMPLE&gt;_TEST_&lt;WHAT&gt;: input of a correctness test that is too large or not free to ship (a file, a
    /// folder, or a list of files separated by ';'). Without it the test is skipped.</item>
    /// <item>CT&lt;SAMPLE&gt;_BENCH_&lt;WHAT&gt;: switches on a measurement (a file to measure, or any value). Measurements
    /// print numbers and never fail; without the variable they do not run.</item>
    /// </list>
    /// A skipped test writes "skipped: set NAME (what it is)" (SkipText) to the test log.
    /// <para>Header only, for the test projects. Preliminary.</para>
    /// </summary>
    class TestEnvironment
    {
    public:
        /// <summary>The value of the variable; false and empty if it is not set, empty or does not fit.</summary>
        static bool Get(const wchar_t* name, wchar_t* value, std::uint32_t size) noexcept
        {
            const unsigned long length = GetEnvironmentVariableW(name, value, size); // DWORD of GetEnvironmentVariableW
            if (length == 0 || length >= size) {
                if (size > 0)
                    value[0] = L'\0';
                return false;
            }
            return true;
        }

        template <std::size_t N>
        static bool Get(const wchar_t* name, wchar_t (&value)[N]) noexcept
        {
            return Get(name, value, static_cast<std::uint32_t>(N));
        }

        /// <summary>A folder: the value with a trailing backslash.</summary>
        template <std::size_t N>
        static bool Folder(const wchar_t* name, wchar_t (&value)[N]) noexcept
        {
            if (!Get(name, value, static_cast<std::uint32_t>(N - 1)))
                return false;
            std::size_t length = 0;
            while (value[length] != L'\0')
                ++length;
            if (value[length - 1] != L'\\') {
                value[length]     = L'\\';
                value[length + 1] = L'\0';
            }
            return true;
        }

        /// <summary>
        /// The next path of a list separated by ';' (from Get): copies it to path and moves list behind it. False at the
        /// end of the list; empty entries and paths that do not fit are passed over.
        /// </summary>
        template <std::size_t N>
        static bool NextPath(const wchar_t*& list, wchar_t (&path)[N]) noexcept
        {
            while (*list != L'\0') {
                std::size_t length = 0;
                while (list[length] != L'\0' && list[length] != L';')
                    ++length;
                const wchar_t* entry = list;
                list += list[length] == L';' ? length + 1 : length;
                if (length == 0 || length >= N)
                    continue;
                for (std::size_t i = 0; i < length; ++i)
                    path[i] = entry[i];
                path[length] = L'\0';
                return true;
            }
            return false;
        }

        /// <summary>The log line of a skipped test: "skipped: set NAME (what)".</summary>
        template <std::size_t N>
        static const wchar_t* SkipText(wchar_t (&text)[N], const wchar_t* name, const wchar_t* what) noexcept
        {
            std::size_t pos    = 0;
            const auto  append = [&](const wchar_t* s) noexcept {
                while (*s != L'\0' && pos + 1 < N)
                    text[pos++] = *s++;
            };
            append(L"skipped: set ");
            append(name);
            append(L" (");
            append(what);
            append(L")");
            text[pos] = L'\0';
            return text;
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
