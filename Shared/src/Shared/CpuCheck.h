// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// The instruction sets of programs built with /arch:AVX2. The compiler may use AVX2, FMA, BMI1/2, LZCNT, MOVBE and
    /// F16C anywhere in such code, already in static initializers and in the start-up of the C runtime, so a program
    /// checks the CPU in its entry point, before the C runtime starts: the linker option /ENTRY names
    /// CpuCheckedConsoleStartup (console programs, continues with wmainCRTStartup) or CpuCheckedWindowsStartup (Windows
    /// programs, continues with wWinMainCRTStartup). Without the instruction sets the program reports what is missing
    /// (stderr or a message box) and ends with exit code 3.
    /// <para>This code is compiled without /arch:AVX2 and without whole program optimization and uses only CpuFeatures
    /// (CPUID, XGETBV) and kernel32.</para>
    /// <para>Preliminary.</para>
    /// </summary>
    class CpuCheck
    {
    public:
        /// <summary>
        /// True if the CPU and the operating system support what /arch:AVX2 code uses (and PCLMULQDQ). Otherwise the
        /// message names the program and the missing instruction sets.
        /// </summary>
        static bool Supported(wchar_t* message, std::int32_t size) noexcept;

        /// <summary>The exit code of a program that stops on an unsupported CPU.</summary>
        static constexpr std::uint32_t ExitCode = 3;

    private:
        static void Append(wchar_t* buffer, std::int32_t size, std::int32_t& pos, const wchar_t* text) noexcept;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
