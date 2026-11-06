//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include <cstdint>

#include "Application.h"
#include "Text.h"

int CT_MAIN(int argc, ThinkMeta::ConcurrentTasks::Samples::Shared::Char** argv)
{
    ThinkMeta::ConcurrentTasks::Samples::Deflate::Application application;
    if (application.ParseArgs(argc, argv))
        return application.Execute();
    return application.ParseError();
}
