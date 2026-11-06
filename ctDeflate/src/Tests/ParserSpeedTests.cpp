//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "pch.h"

#include "BlockRules.h"
#include "MultiParser.h"
#include "Parser.h"
#include "File.h"
#include "TestEnvironment.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Deflate;
using ThinkMeta::ConcurrentTasks::Samples::Shared::Char;
using ThinkMeta::ConcurrentTasks::Samples::Shared::File;
using ThinkMeta::ConcurrentTasks::Samples::Shared::TestEnvironment;

namespace DeflateTests {

    /// <summary>
    /// Single-thread speed of the chunk parsers on the file in the environment variable CTDEFLATE_BENCH_FILE (not a gate;
    /// prints MB/s); without it nothing is measured.
    /// </summary>
    TEST_CLASS(ParserSpeedTests)
    {
    public:
        TEST_METHOD(ParserVersusMultiParser)
        {
            Char          path[File::MaxPath];
            std::uint64_t size = 0;
            if (!TestEnvironment::Get(CT_TEXT("CTDEFLATE_BENCH_FILE"), path) || !File::Size(path, size)) {
                Char text[200];
                Logger::WriteMessage(TestEnvironment::SkipText(text, CT_TEXT("CTDEFLATE_BENCH_FILE"), CT_TEXT("a file of at least 16 MB to parse")));
                return;
            }
            std::vector<std::uint8_t> data(static_cast<std::size_t>(size < (16u << 20) ? size : (16u << 20)));
            Assert::IsTrue(File::Read(path, data.data(), data.size()));

            using Clock  = std::chrono::steady_clock;
            auto seconds = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); };

            for (const std::int32_t level : {6, 9}) {
                const LevelParams params = LevelParams::For(level);
                Clock::time_point t0, t1, t2, t3;

                auto parser = std::make_unique<Parser>();
                parser->Init(params, data.data(), data.size());
                StepLog log;
                log.Reset(0);
                t0 = Clock::now();
                parser->StartSpeculative(0);
                parser->Run(data.size(), log);
                t1 = Clock::now();

                auto multi = std::make_unique<MultiParser>();
                multi->Init(params, data.data(), data.size());
                PathLog paths;
                multi->Run(0, data.size(), MinLenBit(3), paths);
                t2 = Clock::now();
                multi->Run(0, data.size(), MinLenBit(3) | MinLenBit(4), paths);
                t3 = Clock::now();

                const double mb = static_cast<double>(data.size()) / 1e6;
                wchar_t      msg[256];
                std::swprintf(msg, std::size(msg), L"level %d: Parser %.1f MB/s, MultiParser {3} %.1f MB/s, {3,4} %.1f MB/s (%zu steps in path 3)\n", level,
                           mb / seconds(t0, t1), mb / seconds(t1, t2), mb / seconds(t2, t3), paths.lanes[3].steps.size());
                Logger::WriteMessage(msg);
            }
        }
    };

} // namespace DeflateTests
