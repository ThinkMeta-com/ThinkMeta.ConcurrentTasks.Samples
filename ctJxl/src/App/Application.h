// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CjxlMain.h"
#include "ParseResult.h"
#include "Text.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    /// <summary>
    /// The program ctJxl: the command line (ParseArgs), then either the encoding (Execute) or the end after an error
    /// or the help text (ParseError). The same three steps in every sample.
    /// <para>ctJxl takes cjxl's command line (CjxlMain.cc) plus its own options:</para>
    /// <list type="bullet">
    /// <item>--pool ct|libjxl: libjxl's parallel regions on ConcurrentTasks (default) or on libjxl's own thread pool.</item>
    /// </list>
    /// </summary>
    class Application
    {
    public:
        /// <summary>Debug builds: CRT reports to stderr (DebugReports).</summary>
        Application();

        /// <summary>Takes ctJxl's own options off the command line and keeps the rest for cjxl. False on an error.</summary>
        bool ParseArgs(std::int32_t argc, Shared::Char** argv);

        /// <summary>Encodes with cjxl's frontend; returns its exit code.</summary>
        std::int32_t Execute();

        /// <summary>After a failed ParseArgs, which has printed the message: returns the exit code.</summary>
        std::int32_t ParseError();

    private:
        static std::string ToUtf8(const Shared::Char* text);
        static bool        TakeOption(const std::string& arg, const char* name, std::int32_t argc, Shared::Char** argv, std::int32_t& i,
                                      std::string& value);

        static CjxlRunner MakeTaskRunner(std::size_t workers);
        static CjxlRunner MakeLibjxlRunner(std::size_t workers);

        Shared::ParseResult      result_ = Shared::ParseResult::Error;
        std::vector<std::string> cjxlArgs_; // UTF-8, argv[0] first
        bool                     libjxlPool_ = false;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
