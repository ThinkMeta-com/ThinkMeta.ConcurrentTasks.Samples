// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "CpuCheck.h"

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <unistd.h>
#endif

#include <cstdint>

#include "CpuFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    bool CpuCheck::Supported(Char* message, std::int32_t size) noexcept
    {
        // AVX, AVX2 and FMA include the operating system saving the YMM registers
        const CpuFeatures cpu = CpuFeatures::Detect();
        struct Feature {
            const Char* name;
            bool        present;
        };
        const Feature features[] = {
            {CT_TEXT("SSE3"), cpu.sse3},     {CT_TEXT("SSSE3"), cpu.ssse3},      {CT_TEXT("SSE4.1"), cpu.sse41}, {CT_TEXT("SSE4.2"), cpu.sse42},
            {CT_TEXT("POPCNT"), cpu.popcnt}, {CT_TEXT("PCLMULQDQ"), cpu.pclmul}, {CT_TEXT("AVX"), cpu.avx},      {CT_TEXT("AVX2"), cpu.avx2},
            {CT_TEXT("FMA"), cpu.fma},       {CT_TEXT("F16C"), cpu.f16c},        {CT_TEXT("BMI1"), cpu.bmi1},    {CT_TEXT("BMI2"), cpu.bmi2},
            {CT_TEXT("LZCNT"), cpu.lzcnt},   {CT_TEXT("MOVBE"), cpu.movbe},
        };
        bool missing = false;
        for (const Feature& f : features)
            missing = missing || !f.present;
        if (!missing)
            return true;

        // "<program> needs ...; this CPU lacks: AVX2 FMA ..."
        // stack frames below 4 KB: no stack probe, which would call into the C runtime (Windows)
#if defined(_WIN32)
        constexpr std::int32_t PathSize = MAX_PATH;
        Char                   path[PathSize];
        const std::int32_t     length = static_cast<std::int32_t>(GetModuleFileNameW(nullptr, path, PathSize));
#else
        constexpr std::int32_t PathSize = 1024;
        Char                   path[PathSize];
        const ssize_t          read   = ::readlink("/proc/self/exe", path, PathSize - 1);
        const std::int32_t     length = read > 0 ? static_cast<std::int32_t>(read) : 0;
#endif
        path[length > 0 && length < PathSize ? length : 0] = 0;
        const Char* name                                   = path;
        for (std::int32_t i = 0; i < length && path[i] != 0; ++i)
            if (path[i] == '\\' || path[i] == '/')
                name = path + i + 1;
        std::int32_t pos = 0;
        message[0]       = 0;
        Append(message, size, pos, name);
        Append(message, size, pos,
               CT_TEXT(" needs a CPU with AVX2, FMA, BMI2 and PCLMUL (Intel Haswell / AMD Excavator or later) and an operating system that")
                   CT_TEXT(" saves the AVX registers; this CPU lacks:"));
        for (const Feature& f : features) {
            if (!f.present) {
                Append(message, size, pos, CT_TEXT(" "));
                Append(message, size, pos, f.name);
            }
        }
        Append(message, size, pos, CT_TEXT("\n"));
        return false;
    }

    void CpuCheck::Append(Char* buffer, std::int32_t size, std::int32_t& pos, const Char* text) noexcept
    {
        while (*text != 0 && pos + 1 < size)
            buffer[pos++] = *text++;
        buffer[pos] = 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
