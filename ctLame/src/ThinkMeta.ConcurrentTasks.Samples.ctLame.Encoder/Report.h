// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdarg>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>printf-like report function, like LAME's lame_report_function.</summary>
    using ReportFunction = void (*)(const char* format, std::va_list args);

    /// <summary>Report functions of an encoder session (lame_global_flags::report).</summary>
    struct ReportFunctions {
        ReportFunction message = nullptr;
        ReportFunction debug   = nullptr;
        ReportFunction error   = nullptr;
    };

    /// <summary>Report function for messages with UTF-16 text, e.g. paths.</summary>
    using ReportFunctionW = void (*)(const wchar_t* format, std::va_list args);

    /// <summary>Calls a report function printf-style; does nothing if it is null.</summary>
    void Report(ReportFunction function, const char* format, ...);
    void Report(ReportFunctionW function, const wchar_t* format, ...);

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
