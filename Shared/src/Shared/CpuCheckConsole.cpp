// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

// The CPU check of the console programs built for AVX2, before any of their own code runs (CpuCheck):
//   Windows: the entry point (linker /ENTRY:CpuCheckedConsoleStartup), before the C runtime starts; a file of its own,
//            so that only console programs pull in wmainCRTStartup.
//   Linux:   a function in the program's .preinit_array, which the dynamic loader calls before the program's
//            constructors. The file is linked as an object file of its own (CMake: Samples.Shared.CpuCheck); in a
//            static library the linker would leave it out, as nothing refers to it.

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <unistd.h>
#endif

#include <cstdint>

#include "Console.h"
#include "CpuCheck.h"

#if defined(_WIN32)

extern "C" unsigned long wmainCRTStartup(void* peb); // DWORD (LPVOID): the entry point of the C runtime

// No buffer security check: the security cookie is set up by the C runtime started below.
extern "C" __declspec(safebuffers) unsigned long CpuCheckedConsoleStartup(void* peb)
{
    using ThinkMeta::ConcurrentTasks::Samples::Shared::Console;
    using ThinkMeta::ConcurrentTasks::Samples::Shared::CpuCheck;
    using ThinkMeta::ConcurrentTasks::Samples::Shared::Stream;

    wchar_t message[512];
    if (!CpuCheck::Supported(message, 512)) {
        std::int32_t length = 0;
        while (message[length] != 0)
            ++length;
        Console::Write(Stream::Error, message, length);
        ExitProcess(CpuCheck::ExitCode);
    }
    return wmainCRTStartup(peb);
}

#else

// The signature of the functions in .preinit_array: argc, argv and the environment.
static void CpuCheckedStartup([[maybe_unused]] int argc, [[maybe_unused]] char** argv, [[maybe_unused]] char** environment)
{
    using ThinkMeta::ConcurrentTasks::Samples::Shared::Console;
    using ThinkMeta::ConcurrentTasks::Samples::Shared::CpuCheck;
    using ThinkMeta::ConcurrentTasks::Samples::Shared::Stream;

    char message[512];
    if (!CpuCheck::Supported(message, 512)) {
        std::int32_t length = 0;
        while (message[length] != 0)
            ++length;
        Console::Write(Stream::Error, message, length);
        _exit(static_cast<int>(CpuCheck::ExitCode));
    }
}

[[gnu::used, gnu::section(".preinit_array")]] static void (*CpuCheckEntry)(int, char**, char**) = &CpuCheckedStartup;

#endif
