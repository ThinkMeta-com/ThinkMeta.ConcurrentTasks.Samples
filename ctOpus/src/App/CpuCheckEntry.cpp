// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

// The entry point of the ctOpus programs (linker /ENTRY:CtCpuCheckEntry): the encoder library is
// built with /arch:AVX2, so its code - and code inlined from it, static initializers included -
// may use AVX2, FMA, BMI1/BMI2, LZCNT, MOVBE and F16C anywhere. This file is compiled without
// /arch (it belongs to the program, not to the encoder library) and checks the CPU before the C
// runtime starts; without the instructions the program stops with a message instead of crashing.
//
// Defines of the including project:
//   CT_CPUCHECK_CRT_ENTRY  the entry point of the C runtime (wmainCRTStartup, wWinMainCRTStartup)
//   CT_CPUCHECK_GUI        report in a message box instead of on stderr

#include <Windows.h>

#include <intrin.h>

#include <cstdint>

// The CRT entry points take the PEB pointer and return the exit code (DWORD as in the CRT).
extern "C" unsigned long CT_CPUCHECK_CRT_ENTRY(void* peb);

namespace {

    constexpr wchar_t Message[] = L"ctOpus needs a CPU with AVX2 (and FMA, BMI1, BMI2, LZCNT, MOVBE, F16C), e.g. Intel from Haswell (2013) "
                                  L"or AMD from Excavator or Zen, and an operating system that saves the AVX registers.\r\n"
                                  L"This CPU does not provide them, so the program stops.\r\n";

    bool Bit(std::int32_t value, std::int32_t bit) noexcept { return ((static_cast<std::uint32_t>(value) >> bit) & 1u) != 0; }

    // Everything /arch:AVX2 lets MSVC use, and the operating system's support for the YMM state.
    bool CpuSupported() noexcept
    {
#ifdef CT_CPUCHECK_TEST_REFUSE
        return false; // test builds only: the refusal path on a CPU that has everything
#endif
        std::int32_t r[4];
        __cpuid(r, 0);
        if (r[0] < 7)
            return false;
        __cpuid(r, 1);
        const std::int32_t ecx1 = r[2];
        if (!Bit(ecx1, 0) || !Bit(ecx1, 9) || !Bit(ecx1, 12) || !Bit(ecx1, 19) || !Bit(ecx1, 20) || !Bit(ecx1, 22) || !Bit(ecx1, 23)
            || !Bit(ecx1, 27) || !Bit(ecx1, 28) || !Bit(ecx1, 29)) // SSE3 SSSE3 FMA SSE4.1 SSE4.2 MOVBE POPCNT OSXSAVE AVX F16C
            return false;
        if ((_xgetbv(0) & 6) != 6) // XMM and YMM state enabled by the operating system
            return false;
        __cpuidex(r, 7, 0);
        if (!Bit(r[1], 3) || !Bit(r[1], 5) || !Bit(r[1], 8)) // BMI1 AVX2 BMI2
            return false;
        __cpuid(r, static_cast<std::int32_t>(0x80000000u));
        if (static_cast<std::uint32_t>(r[0]) < 0x80000001u)
            return false;
        __cpuid(r, static_cast<std::int32_t>(0x80000001u));
        return Bit(r[2], 5); // LZCNT
    }

    [[noreturn]] void Refuse() noexcept
    {
#ifdef CT_CPUCHECK_GUI
        MessageBoxW(nullptr, Message, L"ctOpus", MB_OK | MB_ICONERROR);
#else
        char text[sizeof(Message) / sizeof(wchar_t)];
        std::int32_t n = 0;
        for (; Message[n] != 0; ++n)
            text[n] = static_cast<char>(Message[n]); // the message is plain ASCII
        DWORD written = 0; // DWORD: WriteFile out parameter
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(n), &written, nullptr);
#endif
        ExitProcess(1);
    }

} // namespace

// No buffer security check: the security cookie is set up by the C runtime started below.
extern "C" __declspec(safebuffers) unsigned long CtCpuCheckEntry(void* peb)
{
    if (!CpuSupported())
        Refuse();
    return CT_CPUCHECK_CRT_ENTRY(peb);
}
