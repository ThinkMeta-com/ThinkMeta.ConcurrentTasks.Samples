//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

// The entry point of ctDeflate.exe (CT_PROGRAM_NAME): checks the instruction sets before anything else runs.
//
// The rest of the program is compiled with /arch:AVX2, so the compiler may use AVX2, FMA, BMI1/2,
// LZCNT, MOVBE and F16C anywhere, already in static initializers and in the C runtime start-up that
// runs them. This file is compiled without /arch (SSE2) and without whole program optimization, and
// its entry point runs before the C runtime: it uses only CPUID, XGETBV and kernel32, and either
// reports what is missing and exits with code 3, or hands over to the C runtime's wmainCRTStartup.

#include <Windows.h>

#include <intrin.h>

#include <cstddef>
#include <cstdint>

extern "C" DWORD wmainCRTStartup(LPVOID);

#ifndef CT_PROGRAM_NAME
#    define CT_PROGRAM_NAME "ctDeflate"
#endif

namespace {

    struct Feature {
        const char* name;
        bool        present;
    };

    /// <summary>Appends text to the buffer at pos (no C runtime yet).</summary>
    void Append(char* buffer, std::size_t size, std::size_t& pos, const char* text) noexcept
    {
        while (*text != 0 && pos + 1 < size)
            buffer[pos++] = *text++;
        buffer[pos] = 0;
    }

    /// <summary>True if the CPU and the OS support what /arch:AVX2 code and the program use; else writes the message.</summary>
    __declspec(noinline) __declspec(safebuffers) bool CpuSupported() noexcept
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
        // The OS must save the YMM registers (OSXSAVE, XCR0 bits 1 and 2).
        const bool osxsave = (ecx1 & (1u << 27)) != 0;
        const bool ymm     = osxsave && (_xgetbv(0) & 6) == 6;

        const Feature features[] = {
            {"SSSE3", (ecx1 & (1u << 9)) != 0},       {"SSE4.1", (ecx1 & (1u << 19)) != 0},     {"SSE4.2", (ecx1 & (1u << 20)) != 0},
            {"POPCNT", (ecx1 & (1u << 23)) != 0},     {"PCLMULQDQ", (ecx1 & (1u << 1)) != 0},   {"AVX", (ecx1 & (1u << 28)) != 0 && ymm},
            {"AVX2", (ebx7 & (1u << 5)) != 0 && ymm}, {"FMA", (ecx1 & (1u << 12)) != 0 && ymm}, {"F16C", (ecx1 & (1u << 29)) != 0},
            {"BMI1", (ebx7 & (1u << 3)) != 0},        {"BMI2", (ebx7 & (1u << 8)) != 0},        {"LZCNT", (ecxExt & (1u << 5)) != 0},
            {"MOVBE", (ecx1 & (1u << 22)) != 0},
        };

        char        message[512];
        std::size_t pos     = 0;
        bool        missing = false;
        message[0]          = 0;
        Append(message, sizeof(message), pos,
               CT_PROGRAM_NAME " needs a CPU with AVX2, FMA, BMI2 and PCLMUL (Intel Haswell / AMD Excavator or later)"
                               " and an operating system that saves the AVX registers; this CPU lacks:");
        for (const Feature& f : features) {
            if (!f.present) {
                Append(message, sizeof(message), pos, " ");
                Append(message, sizeof(message), pos, f.name);
                missing = true;
            }
        }
        if (!missing)
            return true;
        Append(message, sizeof(message), pos, "\r\n");
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, static_cast<DWORD>(pos), &written, nullptr);
        return false;
    }

} // namespace

extern "C" DWORD CpuCheckedStartup(LPVOID peb)
{
    if (!CpuSupported())
        ExitProcess(3);
    return wmainCRTStartup(peb);
}
