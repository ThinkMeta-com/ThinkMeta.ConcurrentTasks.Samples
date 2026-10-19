// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include "Version.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Lame;

namespace LameTests {

    TEST_CLASS(VersionTests){public:
                                 // The reference lame.exe writes exactly these bytes into the LAME tag and prints these
                                 // strings; any difference breaks the bit compare.
                                 TEST_METHOD(MatchesLame40){Assert::AreEqual("4.0", GetLameVersion());
    Assert::AreEqual("4.0", GetLameShortVersion());
    Assert::AreEqual("LAME4.0 ", GetLameVeryShortVersion());
    Assert::AreEqual("LAME4.0 ", GetLameTagEncoderShortVersion());
    Assert::AreEqual("https://lame.sourceforge.io", GetLameUrl());
    Assert::AreEqual("64bits", GetLameOsBitness());
} // namespace LameTests
}
;

} // namespace LameTests
