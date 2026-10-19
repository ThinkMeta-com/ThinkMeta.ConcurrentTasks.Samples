// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

// The entry point of the console programs built with /arch:AVX2 (linker /ENTRY:CpuCheckedConsoleStartup): checks the
// CPU (CpuCheck) before the C runtime starts. A file of its own, so that only console programs pull in wmainCRTStartup.

#include <Windows.h>

#include <cstdint>

#include "Console.h"
#include "CpuCheck.h"

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
