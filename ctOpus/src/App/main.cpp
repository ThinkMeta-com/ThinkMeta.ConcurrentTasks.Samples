// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

// ctOpus.exe: opusenc-compatible Opus encoder for WAV files.

#include "Application.h"

int wmain(int argc, wchar_t** argv)
{
    ThinkMeta::ConcurrentTasks::Samples::Opus::Application application;
    if (application.ParseArgs(argc, argv))
        return application.Execute();
    return application.ParseError();
}
