// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

// The entry point of the Windows programs built with /arch:AVX2 (linker /ENTRY:CpuCheckedWindowsStartup): checks the
// CPU (CpuCheck) before the C runtime starts and reports in a message box. A file of its own, so that only Windows
// programs pull in wWinMainCRTStartup and the message box.

#include <Windows.h>

#include "CpuCheck.h"

extern "C" unsigned long wWinMainCRTStartup(void* peb); // DWORD (LPVOID): the entry point of the C runtime

// No buffer security check: the security cookie is set up by the C runtime started below.
extern "C" __declspec(safebuffers) unsigned long CpuCheckedWindowsStartup(void* peb)
{
    using ThinkMeta::ConcurrentTasks::Samples::Shared::CpuCheck;

    wchar_t message[512];
    if (!CpuCheck::Supported(message, 512)) {
        MessageBoxW(nullptr, message, L"ThinkMeta.ConcurrentTasks", MB_OK | MB_ICONERROR);
        ExitProcess(CpuCheck::ExitCode);
    }
    return wWinMainCRTStartup(peb);
}
