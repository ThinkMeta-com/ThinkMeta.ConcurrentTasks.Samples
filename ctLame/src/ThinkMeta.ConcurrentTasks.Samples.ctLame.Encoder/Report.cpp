// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Report.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    void Report(ReportFunction function, const char* format, ...)
    {
        if (function == nullptr)
            return;

        std::va_list args;
        va_start(args, format);
        function(format, args);
        va_end(args);
    }

    void Report(ReportFunctionW function, const wchar_t* format, ...)
    {
        if (function == nullptr)
            return;

        std::va_list args;
        va_start(args, format);
        function(format, args);
        va_end(args);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
