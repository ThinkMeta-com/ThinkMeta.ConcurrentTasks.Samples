// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "CjxlMain.h"
#include "DebugReports.h"
#include "File.h"
#include "TestEnvironment.h"

// headers with imports last
#include "TaskPool.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Jxl;
namespace Shared = ThinkMeta::ConcurrentTasks::Samples::Shared;

// A task graph is right if its output does not depend on the order in which its tasks run (plan section 4.4). ctJxl's
// graph is libjxl's encoder with its parallel regions on ConcurrentTasks: the DC groups as tasks with their turns and
// gate (GroupSync), the PNG rows decoded while encoding (InputBands), the nested regions of the tree learning, the
// histograms, the tiles of butteraugli, the frames of an animation encoded ahead. Here every region runs its worker
// tasks on ONE scheduler thread in other orders (ParallelRunner::StartOrder): the caller of a region runs no items
// itself, all workers are tasks and start reversed or shuffled, a worker yields after it claimed an item, and items get
// High or Normal priority (every second one, or at random), so a later item overtakes the one before; a DC group yields
// after each turn it ends, the PNG reader after each band. Every encoding must write the file of the serial encoder (no
// workers: every region on its caller, groups and frames one after another, the input read before), byte for byte. A
// missing dependency shows as a wrong file, a crash or a run that does not end (the test then hangs).
//
// Limit: on one thread a task loses the processor only where it waits, yields or ends; races inside such a stretch only
// show with several threads, which the verify gates against the oracle cover.
//
// Input: CTJXL_TEST_IMAGES, the folder of ctJxl's test images (the images of the verify matrices, written by
// New-TestImages.ps1). Without it the tests are skipped.
namespace JxlTests {

    namespace {

        constexpr std::uint32_t Workers = 16; // worker tasks per region, as on a machine with 16 logical CPUs

        std::string ToUtf8(const Shared::Char* text)
        {
            const int   length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            std::string result(length > 0 ? static_cast<std::size_t>(length - 1) : 0, '\0');
            if (length > 1)
                WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
            return result;
        }

        // the folder of the test images, or false (and the test is skipped)
        bool ImageFolder(Shared::Char (&folder)[MAX_PATH])
        {
            if (Shared::TestEnvironment::Folder(CT_TEXT("CTJXL_TEST_IMAGES"), folder))
                return true;
            Shared::Char skip[200];
            Logger::WriteMessage(Shared::TestEnvironment::SkipText(skip, CT_TEXT("CTJXL_TEST_IMAGES"),
                                                                   CT_TEXT("the folder of ctJxl's test images")));
            return false;
        }

        // One encoding of image with cjxl's options, workers worker tasks per region on a scheduler with one thread; the file it
        // wrote. No workers is the serial encoder: no scheduler, every region runs on its caller.
        std::vector<std::uint8_t> Encode(const Shared::Char* folder, const Shared::Char* image, const std::vector<std::string>& options,
                                         std::uint32_t workers, ParallelRunner::StartOrder order,
                                         std::uint32_t taskBudget = TaskPool::TaskQueueCapacity / 2)
        {
            Shared::Char input[MAX_PATH];
            Assert::IsTrue(swprintf_s(input, L"%s%s", folder, image) > 0);
            std::uint64_t inputSize = 0;
            Assert::IsTrue(Shared::File::Size(input, inputSize), image); // cjxl ends the process on a missing input
            Shared::Char      output[MAX_PATH];
            Shared::Char      tempFolder[MAX_PATH];
            const std::uint32_t tempLength = GetTempPathW(MAX_PATH, tempFolder);
            Assert::IsTrue(tempLength > 0 && tempLength < MAX_PATH);
            Assert::IsTrue(swprintf_s(output, L"%sctjxl-order-%u.jxl", tempFolder, GetCurrentProcessId()) > 0);

            std::vector<std::string> args = {"cjxl", ToUtf8(input), ToUtf8(output)};
            args.insert(args.end(), options.begin(), options.end());
            args.emplace_back("--quiet");
            std::vector<char*> argv;
            for (std::string& arg : args)
                argv.push_back(arg.data());
            argv.push_back(nullptr);

            const CjxlRunnerFactory makeRunner = [workers, order, taskBudget](std::size_t) {
                CjxlRunner result;
                if (const std::shared_ptr<TaskPool> pool = TaskPool::Create(workers, workers > 0 ? 1 : 0, order, taskBudget)) {
                    result.runner = ParallelRunner::Run;
                    result.opaque = std::shared_ptr<void>(pool, &pool->Runner());
                }
                return result;
            };
            const int result = CjxlMain(static_cast<int>(argv.size() - 1), argv.data(), makeRunner);

            // the file first, deleted before any assert
            std::uint64_t             size = 0;
            std::vector<std::uint8_t> encoded;
            bool                      read = Shared::File::Size(output, size);
            if (read) {
                encoded.resize(static_cast<std::size_t>(size));
                read = Shared::File::Read(output, encoded.data(), encoded.size());
            }
            Shared::File::Delete(output);
            Assert::AreEqual(0, result, image);
            Assert::IsTrue(read, image);
            return encoded;
        }

        // the serial encoder, then with workers worker tasks per region the normal order and the test orders 1 (reversed), 2 and 3
        // (shuffled), 3 also with priorities at random
        void RunOrders(const Shared::Char* image, const std::vector<std::string>& options, std::uint32_t workers = Workers)
        {
            Shared::Char folder[MAX_PATH];
            if (!ImageFolder(folder))
                return;
            const std::vector<std::uint8_t> reference = Encode(folder, image, options, 0, {});
            Assert::IsTrue(!reference.empty());
            const ParallelRunner::StartOrder orders[] = {{0, false}, {1, false}, {2, false}, {3, true}};
            for (const ParallelRunner::StartOrder& order : orders) {
                const std::vector<std::uint8_t> encoded = Encode(folder, image, options, workers, order);
                wchar_t                         text[200];
                Assert::IsTrue(swprintf_s(text, L"%s: %u workers, order %u%s", image, workers, order.variant,
                                          order.randomPriorities ? L", priorities at random" : L"") > 0);
                Assert::AreEqual(reference.size(), encoded.size(), text);
                Assert::IsTrue(std::memcmp(reference.data(), encoded.data(), reference.size()) == 0, text);
            }
        }

    } // namespace

    TEST_CLASS(GraphOrderTests)
    {
    public:
        TEST_CLASS_INITIALIZE(ReportsToStderr)
        {
            Shared::DebugReports::ToStderr(); // no dialogs from debug asserts in unattended runs
        }

        // DC groups as tasks with turns and gate (D1.5), PNG rows decoded while the groups encode (D1.6b)
        TEST_METHOD(DcGroupsAndPngBands)
        {
            RunOrders(CT_TEXT("png-idats-2560x2304.png"), {"-q", "90"});
        }

        // lossless DC groups with an extra channel (W2, W8)
        TEST_METHOD(LosslessGroupsWithExtraChannel)
        {
            RunOrders(CT_TEXT("crop-2560x2304-depth.pam"), {"-d", "0"});
        }

        // the output turn: in output mode 1 (sections as they come) and 2 (jxlp boxes out of order) each group writes its
        // sections in turn; buffered (mode 0) they go to their own slots
        TEST_METHOD(GroupsStreamingOutput)
        {
            RunOrders(CT_TEXT("crop-2560x2304.ppm"), {"-q", "90", "--output_mode", "1"});
            RunOrders(CT_TEXT("crop-2560x2304-depth.pam"), {"-d", "0", "--output_mode", "2"});
        }

        // two workers: two working states for four DC groups, each reused by a second group
        TEST_METHOD(GroupStatesReused)
        {
            RunOrders(CT_TEXT("crop-2560x2304.ppm"), {"-q", "90"}, 2);
        }

        // butteraugli in tiles (D3)
        TEST_METHOD(ButteraugliTiles)
        {
            RunOrders(CT_TEXT("crop-1024x768.ppm"), {"-q", "90", "-e", "9"});
        }

        // tree learning (nested regions, subtrees), tree data ahead, histograms (D4)
        TEST_METHOD(ModularTreesAndHistograms)
        {
            RunOrders(CT_TEXT("crop-640x480.ppm"), {"-d", "0", "-e", "9"});
            RunOrders(CT_TEXT("crop-640x480.ppm"), {"-m", "1", "-q", "80"});
        }

        // frames encoded ahead (D7)
        TEST_METHOD(AnimationFramesAhead)
        {
            RunOrders(CT_TEXT("anim-320x240.png"), {"-d", "0"});
        }

        // the runner's task budget used up (E7): regions fork fewer tasks than workers, down to none, also the DC group
        // region (four groups, four working states) and the nested regions of the frames ahead
        TEST_METHOD(TaskBudgetUsedUp)
        {
            Shared::Char folder[MAX_PATH];
            if (!ImageFolder(folder))
                return;
            const struct {
                const Shared::Char*      image;
                std::vector<std::string> options;
            } cases[] = {{CT_TEXT("png-idats-2560x2304.png"), {"-q", "90"}}, {CT_TEXT("anim-320x240.png"), {"-d", "0", "-e", "9"}}};
            for (const auto& c : cases) {
                const std::vector<std::uint8_t> reference = Encode(folder, c.image, c.options, 0, {});
                Assert::IsTrue(!reference.empty());
                // 2 workers: two working states for four DC groups, so a task encodes two groups
                for (const std::uint32_t workers : {2u, Workers}) {
                    for (const std::uint32_t budget : {0u, 1u, 2u}) {
                        const std::vector<std::uint8_t> encoded = Encode(folder, c.image, c.options, workers, {}, budget);
                        wchar_t                         text[200];
                        Assert::IsTrue(swprintf_s(text, L"%s: %u workers, task budget %u", c.image, workers, budget) > 0);
                        Assert::AreEqual(reference.size(), encoded.size(), text);
                        Assert::IsTrue(std::memcmp(reference.data(), encoded.data(), reference.size()) == 0, text);
                    }
                }
            }
        }
    };

} // namespace JxlTests
