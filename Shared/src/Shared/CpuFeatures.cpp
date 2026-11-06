// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "CpuFeatures.h"

#if defined(_WIN32)
#    include <intrin.h>
#else
#    include <cpuid.h>
#endif

#include <cstdint>

#include "Formatter.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    // CPUID leaf and subleaf: eax, ebx, ecx, edx
    static void Cpuid(std::uint32_t regs[4], std::uint32_t leaf, std::uint32_t subleaf) noexcept
    {
#if defined(_WIN32)
        int values[4];
        __cpuidex(values, static_cast<int>(leaf), static_cast<int>(subleaf));
        for (std::int32_t i = 0; i < 4; ++i)
            regs[i] = static_cast<std::uint32_t>(values[i]);
#else
        __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#endif
    }

    // XCR0: the register states the operating system saves (only after OSXSAVE is checked)
    static std::uint64_t ReadXcr0() noexcept
    {
#if defined(_WIN32)
        return _xgetbv(0);
#else
        std::uint32_t low = 0, high = 0;
        __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
        return static_cast<std::uint64_t>(high) << 32 | low;
#endif
    }

    CpuFeatures CpuFeatures::Detect() noexcept
    {
        std::uint32_t regs[4];
        Cpuid(regs, 0, 0);
        const std::uint32_t maxLeaf = regs[0];
        Cpuid(regs, 1, 0);
        const std::uint32_t ecx1 = regs[2];
        std::uint32_t       ebx7 = 0;
        if (maxLeaf >= 7) {
            Cpuid(regs, 7, 0);
            ebx7 = regs[1];
        }
        Cpuid(regs, 0x80000000u, 0);
        std::uint32_t ecxExt = 0;
        if (regs[0] >= 0x80000001u) {
            Cpuid(regs, 0x80000001u, 0);
            ecxExt = regs[2];
        }
        // the operating system must save the XMM and YMM registers (OSXSAVE, XCR0 bits 1 and 2)
        const bool ymm = (ecx1 & (1u << 27)) != 0 && (ReadXcr0() & 6) == 6;

        CpuFeatures f;
        f.sse3   = (ecx1 & (1u << 0)) != 0;
        f.ssse3  = (ecx1 & (1u << 9)) != 0;
        f.sse41  = (ecx1 & (1u << 19)) != 0;
        f.sse42  = (ecx1 & (1u << 20)) != 0;
        f.popcnt = (ecx1 & (1u << 23)) != 0;
        f.pclmul = (ecx1 & (1u << 1)) != 0;
        f.avx    = (ecx1 & (1u << 28)) != 0 && ymm;
        f.avx2   = (ebx7 & (1u << 5)) != 0 && ymm;
        f.fma    = (ecx1 & (1u << 12)) != 0 && ymm;
        f.f16c   = (ecx1 & (1u << 29)) != 0;
        f.bmi1   = (ebx7 & (1u << 3)) != 0;
        f.bmi2   = (ebx7 & (1u << 8)) != 0;
        f.lzcnt  = (ecxExt & (1u << 5)) != 0;
        f.movbe  = (ecx1 & (1u << 22)) != 0;
        return f;
    }

    std::int32_t CpuFeatures::FormatLine(const Char* levelName, Char* text, std::int32_t size) const noexcept
    {
        const std::int32_t length = Formatter::Format(text, size, CT_TEXT("cpu SSE2%s%s%s, using %s (bit-identical)"), sse41 ? CT_TEXT(" SSE4.1") : CT_TEXT(""),
                                                      avx2 ? CT_TEXT(" AVX2") : CT_TEXT(""), fma ? CT_TEXT(" FMA") : CT_TEXT(""), levelName);
        return length < size ? length : (size > 0 ? size - 1 : 0);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
