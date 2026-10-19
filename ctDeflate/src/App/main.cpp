//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include <cstdint>

#include "Application.h"

int wmain(int argc, wchar_t** argv)
{
    ThinkMeta::ConcurrentTasks::Samples::Deflate::Application application;
    if (application.ParseArgs(argc, argv))
        return application.Execute();
    return application.ParseError();
}
