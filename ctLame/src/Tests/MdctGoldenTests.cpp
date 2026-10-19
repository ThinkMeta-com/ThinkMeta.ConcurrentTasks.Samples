// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Mdct.h"
#include "SessionConfig.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Lame;

// Bit-exact regression test of the MDCT. The expected checksums were recorded with the kernels
// taken over unchanged from LAME's newmdct.c (API commit b8578bb); the hand-written kernels must
// produce exactly the same bits for every input, block type sequence and filter setting.

namespace LameTests {

    namespace {

        struct Random {
            std::uint64_t state;

            std::uint32_t Next()
            {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                return static_cast<std::uint32_t>(state >> 16);
            }

            // mostly audio-like values, sometimes tiny, huge, zero or -0
            sample_t Sample()
            {
                const std::uint32_t kind = Next() % 16;
                const float         unit = static_cast<float>(Next()) / 4294967296.0f * 2.0f - 1.0f;
                switch (kind) {
                    case 0: return 0.0f;
                    case 1: return -0.0f;
                    case 2: return unit * 1e-30f;
                    case 3: return unit * 1e9f;
                    default: return unit * 32768.0f;
                }
            }
        };

        struct Checksum {
            std::uint64_t value = 1469598103934665603ull;

            void Add(const void* data, std::size_t size)
            {
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                for (std::size_t i = 0; i < size; ++i) {
                    value ^= bytes[i];
                    value *= 1099511628211ull;
                }
            }
        };

        std::uint64_t RunMdct(void (*adjust)(EncoderOptions&), std::uint64_t seed)
        {
            EncoderOptions options;
            if (adjust != nullptr)
                adjust(options);
            SessionConfig config;
            Assert::AreEqual(0, InitParams(options, config));

            Random           random{seed};
            MdctChannelState state;
            static sample_t  input[1152 * 2 + 2048];
            FrameLines       xr;
            Checksum         checksum;

            for (sample_t& s : input)
                s = random.Sample();
            MdctPrime(config.amp_filter, state, input);
            checksum.Add(&state, sizeof(state));

            // every sequence of block types, as the psychoacoustic model could choose them
            const std::int32_t types[4] = {NORM_TYPE, START_TYPE, SHORT_TYPE, STOP_TYPE};
            for (std::int32_t frame = 0; frame < 3000; ++frame) {
                std::memmove(input, input + 1152, sizeof(input) - 1152 * sizeof(sample_t));
                for (std::int32_t i = static_cast<std::int32_t>(sizeof(input) / sizeof(sample_t)) - 1152;
                     i < static_cast<std::int32_t>(sizeof(input) / sizeof(sample_t)); ++i)
                    input[i] = random.Sample();
                const std::array<std::int32_t, 2> blockType = {types[random.Next() % 4], types[random.Next() % 4]};
                MdctChannel(config.amp_filter, state, input, blockType, xr);
                checksum.Add(&xr, sizeof(xr));
            }
            checksum.Add(&state, sizeof(state));
            return checksum.value;
        }

        void Check(const wchar_t* name, std::uint64_t expected, std::uint64_t actual)
        {
            wchar_t text[128];
            swprintf_s(text, L"%ls: 0x%016llX", name, actual);
            Logger::WriteMessage(text);
            Assert::AreEqual(expected, actual, name);
        }

    } // namespace

    TEST_CLASS(MdctGoldenTests){public: TEST_METHOD(Default44k){Check(L"Default44k", 0x9DAE9E0A19199F6Bull, RunMdct(nullptr, 0x9E3779B97F4A7C15ull));
} // namespace LameTests

TEST_METHOD(Lowpass32kV7)
{
    Check(L"Lowpass32kV7", 0xE98D7D5D9CC3A830ull,
          RunMdct(
              [](EncoderOptions& o) {
                  o.SetInSamplerate(32000);
                  o.SetVbrQuality(7.3f);
              },
              0x243F6A8885A308D3ull));
}

TEST_METHOD(HighpassAndLowpass48k)
{
    Check(L"HighpassAndLowpass48k", 0x8F4CA879B1CE0478ull,
          RunMdct(
              [](EncoderOptions& o) {
                  o.SetInSamplerate(48000);
                  o.SetHighpassFreq(120);
                  o.SetHighpassWidth(200);
                  o.SetLowpassFreq(9000);
                  o.SetLowpassWidth(1500);
              },
              0x13198A2E03707344ull));
}

TEST_METHOD(NoFilterV0)
{
    Check(L"NoFilterV0", 0x131AB18166E099C5ull,
          RunMdct(
              [](EncoderOptions& o) {
                  o.SetVbrQuality(0.0f);
                  o.SetLowpassFreq(-1);
              },
              0xA4093822299F31D0ull));
}
}
;

} // namespace LameTests
