// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <cstdint>

#include "Application.h"

int wmain(int argc, wchar_t** argv)
{
    ThinkMeta::ConcurrentTasks::Samples::Jpeg::Application application;
    if (application.ParseArgs(argc, argv))
        return application.Execute();
    return application.ParseError();
}
