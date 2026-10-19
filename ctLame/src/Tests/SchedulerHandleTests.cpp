// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include "SchedulerHandle.h"

#include <utility>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks;
using ThinkMeta::ConcurrentTasks::Samples::Shared::SchedulerHandle;

namespace LameTests {

    TEST_CLASS(SchedulerHandleTests){public: TEST_METHOD(CreatesAndDestroys){auto scheduler = SchedulerHandle::Create(SchedulerKind::Default, 1);
    Assert::IsTrue(static_cast<bool>(scheduler));
    scheduler->Start();
    scheduler->Stop();
    scheduler.Reset();
    Assert::IsFalse(static_cast<bool>(scheduler));
} // namespace LameTests

TEST_METHOD(MoveTransfersOwnership)
{
    auto            first = SchedulerHandle::Create(SchedulerKind::Default, 1);
    ITaskScheduler* raw   = first.Get();

    SchedulerHandle second = std::move(first);
    Assert::IsNull(first.Get());
    Assert::IsTrue(second.Get() == raw);
}

// TaskSwap needs a switch policy; without one the core refuses to create it.
TEST_METHOD(EmptyWhenRejected)
{
    auto scheduler = SchedulerHandle::Create(SchedulerKind::TaskSwap, 1);
    Assert::IsFalse(static_cast<bool>(scheduler));
}
}
;

} // namespace LameTests
