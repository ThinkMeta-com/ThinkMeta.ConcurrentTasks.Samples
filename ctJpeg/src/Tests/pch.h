// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#ifndef PCH_H
#define PCH_H

#if defined(_WIN32)
#    include <Windows.h>
#endif

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <string>

#include "CppUnitTest.h"
#include "Text.h"

// The samples' text (Shared::Char) as the wide text of the test framework's messages (ASCII).
inline std::wstring Wide(const ThinkMeta::ConcurrentTasks::Samples::Shared::Char* text)
{
    std::wstring result;
    for (; *text != 0; ++text)
        result += static_cast<wchar_t>(*text);
    return result;
}
#endif // PCH_H
