// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <cstdint>

#include "SessionConfig.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Lame;

// The command line comparison with lame.exe (Compare-LameCli.ps1) checks
// everything --verbose prints. These tests pin values it cannot see.

namespace LameTests {

    TEST_CLASS(InitParamsTests){public: TEST_METHOD(DefaultsFor44kStereo){EncoderOptions options;
    SessionConfig config;
    Assert::AreEqual(0, InitParams(options, config));

    Assert::AreEqual(44100, config.samplerate_out);
    Assert::AreEqual(0, config.samplerate_index);
    Assert::AreEqual(static_cast<std::int32_t>(JointStereo), static_cast<std::int32_t>(config.mode));
    Assert::AreEqual(static_cast<std::int32_t>(ShortBlockCoupled), static_cast<std::int32_t>(config.short_blocks));
    Assert::AreEqual(2, config.channels_out);
    Assert::AreEqual(36, config.sideinfo_len);
    Assert::AreEqual(0, config.options.quality); // -q 3 maps to 0 for VBR-new
    Assert::AreEqual(1, config.noise_shaping);
    Assert::AreEqual(1, config.use_best_huffman);
    Assert::AreEqual(1, config.vbr_min_bitrate_index);
    Assert::AreEqual(14, config.vbr_max_bitrate_index);
    Assert::AreEqual(7680 * 2, config.buffer_constraint); // MDB_MAXIMUM
    Assert::AreEqual(4, config.options.vbrQ);
    Assert::AreEqual(460, config.preset);
    Assert::AreEqual(5, config.ATHtype);
    Assert::AreEqual(0, config.sfb21_extra);
    Assert::AreEqual(576, config.scalefac_band.l[22]);
    Assert::AreEqual(192, config.scalefac_band.s[13]);
} // namespace LameTests

TEST_METHOD(MinimumBitrateIsRoundedButKeptForTheTag)
{
    EncoderOptions options;
    options.SetVbrMinBitrateKbps(100);
    options.SetVbrMaxBitrateKbps(999);
    SessionConfig config;
    Assert::AreEqual(0, InitParams(options, config));

    Assert::AreEqual(96, config.options.vbrMinBitrateKbps);
    Assert::AreEqual(7, config.vbr_min_bitrate_index);
    Assert::AreEqual(320, config.options.vbrMaxBitrateKbps);
    Assert::AreEqual(14, config.vbr_max_bitrate_index);
    Assert::AreEqual(100, config.avg_bitrate);
}

// Without resampling LAME remaps -V 6.5 ... 8 at 32 kHz to 5.2 ... 6.5 and disables the lowpass.
TEST_METHOD(QualityRemapAt32k)
{
    EncoderOptions options;
    options.SetInSamplerate(32000);
    options.SetVbrQuality(7.0f);
    SessionConfig config;
    Assert::AreEqual(0, InitParams(options, config));

    Assert::AreEqual(5, config.options.vbrQ);
    Assert::IsTrue(config.options.vbrQFrac > 0.63f && config.options.vbrQFrac < 0.64f);
    Assert::AreEqual(-1, config.lowpassfreq);
    Assert::AreEqual(0.0f, config.lowpass2);
}

TEST_METHOD(RejectsUnsupportedInput)
{
    EncoderOptions options;
    options.SetInSamplerate(22050);
    SessionConfig config;
    Assert::AreEqual(-1, InitParams(options, config));
}

TEST_METHOD(DownmixTransform)
{
    EncoderOptions options;
    options.SetMode(Mono);
    options.SetScale(0.5f);
    SessionConfig config;
    Assert::AreEqual(0, InitParams(options, config));

    Assert::AreEqual(1, config.channels_out);
    Assert::AreEqual(0.25f, config.pcm_transform[0][0]);
    Assert::AreEqual(0.25f, config.pcm_transform[0][1]);
    Assert::AreEqual(0.0f, config.pcm_transform[1][0]);
    Assert::AreEqual(0.0f, config.pcm_transform[1][1]);
}
}
;

} // namespace LameTests
