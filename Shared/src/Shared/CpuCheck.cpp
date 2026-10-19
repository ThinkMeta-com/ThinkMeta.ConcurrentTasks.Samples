// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "CpuCheck.h"

#include <Windows.h>

#include <cstdint>

#include "CpuFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    bool CpuCheck::Supported(wchar_t* message, std::int32_t size) noexcept
    {
        // AVX, AVX2 and FMA include the operating system saving the YMM registers
        const CpuFeatures cpu = CpuFeatures::Detect();
        struct Feature {
            const wchar_t* name;
            bool           present;
        };
        const Feature features[] = {
            {L"SSE3", cpu.sse3},        {L"SSSE3", cpu.ssse3}, {L"SSE4.1", cpu.sse41}, {L"SSE4.2", cpu.sse42}, {L"POPCNT", cpu.popcnt},
            {L"PCLMULQDQ", cpu.pclmul}, {L"AVX", cpu.avx},     {L"AVX2", cpu.avx2},    {L"FMA", cpu.fma},      {L"F16C", cpu.f16c},
            {L"BMI1", cpu.bmi1},        {L"BMI2", cpu.bmi2},   {L"LZCNT", cpu.lzcnt},  {L"MOVBE", cpu.movbe},
        };
        bool missing = false;
        for (const Feature& f : features)
            missing = missing || !f.present;
        if (!missing)
            return true;

        // "<program> needs ...; this CPU lacks: AVX2 FMA ..."
        wchar_t             path[MAX_PATH];
        const unsigned long length = GetModuleFileNameW(nullptr, path, MAX_PATH); // DWORD of GetModuleFileNameW
        const wchar_t*      name   = path;
        for (unsigned long i = 0; i < length; ++i)
            if (path[i] == L'\\' || path[i] == L'/')
                name = path + i + 1;
        if (length == 0)
            path[0] = 0;
        std::int32_t pos = 0;
        message[0]       = 0;
        Append(message, size, pos, name);
        Append(message, size, pos,
               L" needs a CPU with AVX2, FMA, BMI2 and PCLMUL (Intel Haswell / AMD Excavator or later) and an operating system that"
               L" saves the AVX registers; this CPU lacks:");
        for (const Feature& f : features) {
            if (!f.present) {
                Append(message, size, pos, L" ");
                Append(message, size, pos, f.name);
            }
        }
        Append(message, size, pos, L"\n");
        return false;
    }

    void CpuCheck::Append(wchar_t* buffer, std::int32_t size, std::int32_t& pos, const wchar_t* text) noexcept
    {
        while (*text != 0 && pos + 1 < size)
            buffer[pos++] = *text++;
        buffer[pos] = 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
