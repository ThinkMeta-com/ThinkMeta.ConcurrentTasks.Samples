// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#if defined(_WIN32)
#    include <Windows.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Application.h"

#include <jxl/thread_parallel_runner.h>

#include <cstring>
#include <memory>


#include "CjxlMain.h"
#include "Console.h"
#include "DebugReports.h"
#include "TaskPool.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    Application::Application()
    {
        Shared::DebugReports::ToStderr();
    }

    bool Application::ParseArgs(std::int32_t argc, Shared::Char** argv)
    {
        bool options = true; // cjxl's parser takes no options after "--", nor does ctJxl
        for (std::int32_t i = 0; i < argc; ++i) {
            std::string arg = ToUtf8(argv[i]);
            if (i > 0 && options) {
                if (arg == "--")
                    options = false;
                std::string value;
                if (TakeOption(arg, "--pool", argc, argv, i, value)) {
                    if (value != "ct" && value != "libjxl") {
                        Shared::Console::Print(Shared::Stream::Error, CT_TEXT("ctJxl: --pool needs ct or libjxl\n"));
                        return false;
                    }
                    libjxlPool_ = value == "libjxl";
                    continue;
                }
            }
            cjxlArgs_.push_back(std::move(arg));
        }
        result_ = Shared::ParseResult::Run;
        return true;
    }

    std::int32_t Application::Execute()
    {
        std::vector<char*> args;
        for (std::string& arg : cjxlArgs_)
            args.push_back(arg.data());
        args.push_back(nullptr);
        return CjxlMain(static_cast<int>(args.size() - 1), args.data(), libjxlPool_ ? MakeLibjxlRunner : MakeTaskRunner);
    }

    std::int32_t Application::ParseError()
    {
        return result_ == Shared::ParseResult::Exit ? 0 : 1;
    }

    std::string Application::ToUtf8(const Shared::Char* text)
    {
#if defined(_WIN32)
        const int   length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string result(length > 0 ? static_cast<std::size_t>(length - 1) : 0, '\0');
        if (length > 1)
            WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
        return result;
#else
        return text;
#endif
    }

    // "--name value" or "--name=value" (as cjxl's long options); false if arg is not this option
    bool Application::TakeOption(const std::string& arg, const char* name, std::int32_t argc, Shared::Char** argv, std::int32_t& i,
                                 std::string& value)
    {
        const std::size_t length = std::strlen(name);
        if (arg.compare(0, length, name) != 0 || (arg.size() > length && arg[length] != '='))
            return false;
        if (arg.size() > length)
            value = arg.substr(length + 1);
        else
            value = i + 1 < argc ? ToUtf8(argv[++i]) : std::string();
        return true;
    }

    // the runner for cjxl: libjxl's parallel regions on ConcurrentTasks (TaskPool), created once cjxl knows its thread count
    CjxlRunner Application::MakeTaskRunner(std::size_t workers)
    {
        CjxlRunner result;
        if (const std::shared_ptr<TaskPool> pool = TaskPool::Create(static_cast<std::uint32_t>(workers))) {
            result.runner = ParallelRunner::Run;
            result.opaque = std::shared_ptr<void>(pool, &pool->Runner()); // keeps the pool, points at the runner
        }
        return result;
    }

    // --pool libjxl: libjxl's own thread pool, as in cjxl
    CjxlRunner Application::MakeLibjxlRunner(std::size_t workers)
    {
        CjxlRunner result;
        if (void* opaque = JxlThreadParallelRunnerCreate(nullptr, workers)) {
            result.runner = JxlThreadParallelRunner;
            result.opaque = std::shared_ptr<void>(opaque, JxlThreadParallelRunnerDestroy);
        }
        return result;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
