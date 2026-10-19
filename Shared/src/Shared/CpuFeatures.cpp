// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "CpuFeatures.h"

#include <intrin.h>

#include <cstdint>

#include "Formatter.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    CpuFeatures CpuFeatures::Detect() noexcept
    {
        int regs[4];
        __cpuid(regs, 0);
        const int maxLeaf = regs[0];
        __cpuid(regs, 1);
        const auto    ecx1 = static_cast<std::uint32_t>(regs[2]);
        std::uint32_t ebx7 = 0;
        if (maxLeaf >= 7) {
            __cpuidex(regs, 7, 0);
            ebx7 = static_cast<std::uint32_t>(regs[1]);
        }
        __cpuid(regs, static_cast<int>(0x80000000u));
        std::uint32_t ecxExt = 0;
        if (static_cast<std::uint32_t>(regs[0]) >= 0x80000001u) {
            __cpuid(regs, static_cast<int>(0x80000001u));
            ecxExt = static_cast<std::uint32_t>(regs[2]);
        }
        // the operating system must save the XMM and YMM registers (OSXSAVE, XCR0 bits 1 and 2)
        const bool ymm = (ecx1 & (1u << 27)) != 0 && (_xgetbv(0) & 6) == 6;

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

    std::int32_t CpuFeatures::FormatLine(const wchar_t* levelName, wchar_t* text, std::int32_t size) const noexcept
    {
        const std::int32_t length = Formatter::Format(text, size, L"cpu SSE2%ls%ls%ls, using %ls (bit-identical)", sse41 ? L" SSE4.1" : L"",
                                                      avx2 ? L" AVX2" : L"", fma ? L" FMA" : L"", levelName);
        return length < size ? length : (size > 0 ? size - 1 : 0);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
