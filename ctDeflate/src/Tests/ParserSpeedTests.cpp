//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "pch.h"

#include "BlockRules.h"
#include "MultiParser.h"
#include "Parser.h"
#include "TestEnvironment.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Deflate;
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
            wchar_t    path[MAX_PATH];
            const bool set  = TestEnvironment::Get(L"CTDEFLATE_BENCH_FILE", path);
            HANDLE     file = set ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr) : INVALID_HANDLE_VALUE;
            if (file == INVALID_HANDLE_VALUE) {
                wchar_t text[200];
                Logger::WriteMessage(TestEnvironment::SkipText(text, L"CTDEFLATE_BENCH_FILE", L"a file of at least 16 MB to parse"));
                return;
            }
            std::vector<std::uint8_t> data(16u << 20);
            unsigned long             read = 0; // DWORD of ReadFile
            ::ReadFile(file, data.data(), static_cast<unsigned long>(data.size()), &read, nullptr);
            CloseHandle(file);
            data.resize(read);

            auto seconds = [](LARGE_INTEGER a, LARGE_INTEGER b) {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                return static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
            };

            for (const std::int32_t level : {6, 9}) {
                const LevelParams params = LevelParams::For(level);
                LARGE_INTEGER     t0, t1, t2, t3;

                auto parser = std::make_unique<Parser>();
                parser->Init(params, data.data(), data.size());
                StepLog log;
                log.Reset(0);
                QueryPerformanceCounter(&t0);
                parser->StartSpeculative(0);
                parser->Run(data.size(), log);
                QueryPerformanceCounter(&t1);

                auto multi = std::make_unique<MultiParser>();
                multi->Init(params, data.data(), data.size());
                PathLog paths;
                multi->Run(0, data.size(), MinLenBit(3), paths);
                QueryPerformanceCounter(&t2);
                multi->Run(0, data.size(), MinLenBit(3) | MinLenBit(4), paths);
                QueryPerformanceCounter(&t3);

                const double mb = static_cast<double>(data.size()) / 1e6;
                wchar_t      msg[256];
                swprintf_s(msg, L"level %d: Parser %.1f MB/s, MultiParser {3} %.1f MB/s, {3,4} %.1f MB/s (%zu steps in path 3)\n", level,
                           mb / seconds(t0, t1), mb / seconds(t1, t2), mb / seconds(t2, t3), paths.lanes[3].steps.size());
                Logger::WriteMessage(msg);
            }
        }
    };

} // namespace DeflateTests
