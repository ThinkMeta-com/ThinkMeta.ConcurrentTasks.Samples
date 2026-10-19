// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#ifdef _DEBUG
#    include <crtdbg.h>
#    include <cstdlib>
#endif

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// The reports of the C runtime's debug build. Header only: it calls the C runtime, which the library of Shared does
    /// not, so the calls are compiled into the program.
    /// <para>Preliminary.</para>
    /// </summary>
    class DebugReports
    {
    public:
        /// <summary>
        /// Debug builds run unattended in the test matrices: CRT assertions and errors (for instance a std::array index
        /// out of range) go to stderr instead of a dialog, and the process ends with an error instead of waiting for a
        /// button. Does nothing in a release build.
        /// </summary>
        static void ToStderr() noexcept
        {
#ifdef _DEBUG
            _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
            _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
            _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
