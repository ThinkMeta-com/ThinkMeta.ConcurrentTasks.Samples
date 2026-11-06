// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

#include "Text.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// The instruction sets of programs built for AVX2 (/arch:AVX2, clang -march=x86-64-v3). The compiler may use AVX2,
    /// FMA, BMI1/2, LZCNT, MOVBE and F16C anywhere in such code, already in static initializers, so a program checks the
    /// CPU before any of its own code runs:
    /// <list type="bullet">
    /// <item>Windows: in its entry point, before the C runtime starts: the linker option /ENTRY names
    /// CpuCheckedConsoleStartup (console programs, continues with wmainCRTStartup) or CpuCheckedWindowsStartup (Windows
    /// programs, continues with wWinMainCRTStartup).</item>
    /// <item>Linux: from the program's .preinit_array, which runs before its constructors (CpuCheckConsole.cpp, linked
    /// as an object file of its own).</item>
    /// </list>
    /// Without the instruction sets the program reports what is missing (stderr or a message box) and ends with exit
    /// code 3.
    /// <para>This code is compiled without AVX2 and without whole program optimization and uses only CpuFeatures (CPUID,
    /// XGETBV) and the operating system.</para>
    /// <para>Preliminary.</para>
    /// </summary>
    class CpuCheck
    {
    public:
        /// <summary>
        /// True if the CPU and the operating system support what AVX2 code uses (and PCLMULQDQ). Otherwise the message
        /// names the program and the missing instruction sets.
        /// </summary>
        static bool Supported(Char* message, std::int32_t size) noexcept;

        /// <summary>The exit code of a program that stops on an unsupported CPU.</summary>
        static constexpr std::uint32_t ExitCode = 3;

    private:
        static void Append(Char* buffer, std::int32_t size, std::int32_t& pos, const Char* text) noexcept;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
