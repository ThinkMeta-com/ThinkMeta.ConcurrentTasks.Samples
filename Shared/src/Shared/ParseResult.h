// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// What reading the command line of a sample leads to (CommandLine::Parse of each sample). The messages are printed
    /// by then; the exit code follows from the result: 0 after Exit, 1 after Error.
    /// <para>Preliminary.</para>
    /// </summary>
    enum class ParseResult {
        Run,  // the options are complete: run the program
        Exit, // the help or version text has been printed: end with exit code 0
        Error // an error has been printed: end with exit code 1
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
