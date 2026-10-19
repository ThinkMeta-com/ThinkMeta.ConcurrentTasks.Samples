// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include <cstdint>

#include "Application.h"

int wmain(int argc, wchar_t** argv)
{
    ThinkMeta::ConcurrentTasks::Samples::Lame::Application application;
    if (application.ParseArgs(argc, argv))
        return application.Execute();
    return application.ParseError();
}
