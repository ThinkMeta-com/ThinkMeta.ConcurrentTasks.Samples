// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

// ctOpus.exe: opusenc-compatible Opus encoder for WAV files.

#include <Windows.h>

#include <cstdint>

#include "Frontend.h"
#include "HeapArray.h"

using namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus;

int wmain(int argc, wchar_t** argv)
{
    // the arguments as UTF-8, like opusenc's init_commandline_arguments_utf8 (WIN_UNICODE)
    HeapArray<char*> argvUtf8;
    HeapArray<char>  storage;
    std::size_t      total = 0;
    for (int i = 0; i < argc; ++i)
        total += static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr));
    if (!argvUtf8.Allocate(static_cast<std::size_t>(argc) + 1) || !storage.Allocate(total + 1))
        return 1;
    std::size_t pos = 0;
    for (int i = 0; i < argc; ++i) {
        argvUtf8[i] = storage.Data() + pos;
        pos += static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, storage.Data() + pos, static_cast<int>(total - pos), nullptr, nullptr));
    }
    return RunFrontend(argc, argvUtf8.Data(), argv);
}
