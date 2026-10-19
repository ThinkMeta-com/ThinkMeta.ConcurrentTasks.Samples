// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "Encoder.h"
#include "HuffmanTables.h"
#include "Input.h"
#include "Reservoir.h"
#include "SessionConfig.h"
#include "StreamInput.h"
#include "TestData.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctLame;

// Quantization, reservoir, bit stream and LAME tag; with them the encoder is
// complete. The encoder runs in-process here (without the I/O pipeline) and must produce the
// file the reduced lame.exe writes, byte for byte: the expected values are FNV-1a checksums of
// lame.exe's output for the same synth files and options. A test is skipped when its file is
// missing. Whole files are checked with Verify-Lame.ps1.

namespace ctLameTests {

    namespace {

        // the file in memory: placeholder of the tag, frames, flush; the tag frame replaces the
        // placeholder at the end, like the writer does
        struct EncodeRun : IPcmSink, IFrameSink {
            static constexpr std::size_t Capacity = 8 * 1024 * 1024;

            SessionConfig config;
            Encoder       encoder;
            StreamInput   input;
            std::byte     frame[16384];
            std::byte*    out   = nullptr;
            std::size_t   used  = 0;
            bool          error = false;

            EncodeRun() { out = static_cast<std::byte*>(HeapAlloc(GetProcessHeap(), 0, Capacity)); }
            ~EncodeRun() { HeapFree(GetProcessHeap(), 0, out); }

            void Append(const std::byte* data, std::int32_t size)
            {
                if (size < 0 || used + static_cast<std::size_t>(size) > Capacity) {
                    error = true;
                    return;
                }
                std::memcpy(out + used, data, static_cast<std::size_t>(size));
                used += static_cast<std::size_t>(size);
            }

            void AddSamples(const std::int32_t* left, const std::int32_t* right, std::int32_t count) override { input.AddInt(left, right, count, *this); }

            void EncodeFrame(const ChannelSamples& in) override { Append(frame, encoder.RunAllStages(in, frame, sizeof(frame))); }
        };

        void Run(const wchar_t* name, const wchar_t* file, void (*adjust)(EncoderOptions&), std::size_t expectedSize, std::uint64_t expected)
        {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%ls%ls", SynthDir(), file);
            WavData wav;
            if (!wav.Load(path)) {
                wchar_t text[400];
                swprintf_s(text, L"%ls: skipped, %ls not found", name, path);
                Logger::WriteMessage(text);
                return;
            }

            EncoderOptions options;
            options.SetInSamplerate(wav.rate);
            options.SetNumChannels(wav.channels);
            options.SetNumSamples(static_cast<std::uint32_t>(wav.size / (static_cast<std::size_t>(wav.channels) * 2)));
            adjust(options);

            const auto run = std::make_unique<EncodeRun>(); // too large for the stack
            Assert::AreEqual(0, InitParams(options, run->config));
            run->encoder.Initialize(run->config);
            Assert::IsTrue(run->input.Initialize(run->config));
            run->Append(run->frame, run->encoder.TakeHeaderBytes(run->frame, sizeof(run->frame)));

            PcmReader reader;
            Assert::IsTrue(reader.Initialize({wav.channels, 16, false, false}));
            reader.Push(wav.data, wav.size, *run);
            reader.Finish(*run);
            run->input.Flush(*run);
            run->encoder.SetEncoderPadding(run->input.EncoderPadding());
            run->Append(run->frame, run->encoder.Flush(run->frame, sizeof(run->frame)));

            std::byte         tag[2880];
            const std::size_t tagSize = run->encoder.GetLameTagFrame(tag, sizeof(tag));
            if (tagSize > 0 && tagSize <= run->used)
                std::memcpy(run->out, tag, tagSize);

            Checksum checksum;
            checksum.Add(run->out, run->used);
            const bool        error = run->error;
            const std::size_t size  = run->used;

            wchar_t text[200];
            swprintf_s(text, L"%ls: %zu bytes 0x%016llX", name, size, checksum.value);
            Logger::WriteMessage(text);
            Assert::IsFalse(error, name);
            Assert::AreEqual(expectedSize, size, name);
            Assert::AreEqual(expected, checksum.value, name);
        }

        constexpr const wchar_t* Clipped = L"s16-stereo-44100-clipped.wav";

    } // namespace

    TEST_CLASS(EncoderGoldenTests){public: TEST_METHOD(V4){Run(L"V4", Clipped, [](EncoderOptions& o) { o.SetVbrQuality(4.0f); }, 385718, 0x5E783E2D1529CD12ull);
} // namespace ctLameTests

TEST_METHOD(V0)
{
    Run(L"V0", Clipped, [](EncoderOptions& o) { o.SetVbrQuality(0.0f); }, 718945, 0x8A5969A07E0A1BA2ull);
}

TEST_METHOD(V9999)
{
    Run(L"V9999", Clipped, [](EncoderOptions& o) { o.SetVbrQuality(9.999f); }, 160979, 0xAC4E7B562DBAA8C3ull);
}

TEST_METHOD(MonoInputV4)
{
    Run(L"MonoInputV4", L"s16-mono-44100.wav", [](EncoderOptions& o) { o.SetVbrQuality(4.0f); }, 204722, 0x5210300AF1773B59ull);
}

TEST_METHOD(Rate32kV73)
{
    Run(L"Rate32kV73", L"s16-stereo-32000.wav", [](EncoderOptions& o) { o.SetVbrQuality(7.3f); }, 304416, 0x4E2A5B8892FD249Bull);
}

TEST_METHOD(Rate48kStereoV15)
{
    Run(
        L"Rate48kStereoV15", L"s16-stereo-48000.wav",
        [](EncoderOptions& o) {
            o.SetMode(Stereo);
            o.SetVbrQuality(1.5f);
        },
        489216, 0x7274089E472C22DAull);
}

TEST_METHOD(MonoDownmixV5)
{
    Run(
        L"MonoDownmixV5", Clipped,
        [](EncoderOptions& o) {
            o.SetMode(Mono);
            o.SetVbrQuality(5.0f);
        },
        180180, 0x47E87EE8F175CBA5ull);
}

TEST_METHOD(ExperimentalZV3)
{
    Run(
        L"ExperimentalZV3", Clipped,
        [](EncoderOptions& o) {
            o.SetExperimentalZ(1);
            o.SetVbrQuality(3.0f);
        },
        433449, 0x13AA434D5877B3BCull);
}

TEST_METHOD(ErrorProtectionV2)
{
    Run(
        L"ErrorProtectionV2", Clipped,
        [](EncoderOptions& o) {
            o.SetErrorProtection(1);
            o.SetVbrQuality(2.0f);
        },
        494610, 0x2E1BD07BC5E58C51ull);
}

TEST_METHOD(HardMinMaxV4)
{
    Run(
        L"HardMinMaxV4", Clipped,
        [](EncoderOptions& o) {
            o.SetVbrMinBitrateKbps(64);
            o.SetVbrMaxBitrateKbps(160);
            o.SetVbrHardMin(1);
            o.SetVbrQuality(4.0f);
        },
        379690, 0xD4E560C478F2A192ull);
}

TEST_METHOD(NoReservoirV4)
{
    Run(
        L"NoReservoirV4", Clipped,
        [](EncoderOptions& o) {
            o.SetDisableReservoir(1);
            o.SetVbrQuality(4.0f);
        },
        423526, 0xC224A70A656037F9ull);
}

TEST_METHOD(NoTagV4)
{
    Run(
        L"NoTagV4", Clipped,
        [](EncoderOptions& o) {
            o.SetWriteVbrTag(0);
            o.SetVbrQuality(4.0f);
        },
        385301, 0xBD4ECAAC55016997ull);
}
}
;

TEST_CLASS(EncoderTableTests){public:
                                // the packed code lengths, computed from the tables, against values of LAME's largetbl,
                                // table23 and table56
                                TEST_METHOD(HuffmanPairLengths){ThinkMeta::ConcurrentTasks::Samples::ctLame::HuffmanPairLengths lengths;
InitHuffmanPairLengths(lengths);
Assert::AreEqual(0x010004u, lengths.largetbl[0]);
Assert::AreEqual(0x0d000cu, lengths.largetbl[11]);
Assert::AreEqual(0x0a0006u, lengths.largetbl[255]);
Assert::AreEqual(0x010002u, lengths.table23[0]);
Assert::AreEqual(0x080008u, lengths.table23[8]);
Assert::AreEqual(0x010003u, lengths.table56[0]);
Assert::AreEqual(0x0a0009u, lengths.table56[15]);
}

TEST_METHOD(FrameBits)
{
    auto bits = [](std::int32_t rate, std::int32_t index) {
        EncoderOptions options;
        options.SetInSamplerate(rate);
        SessionConfig config;
        Assert::AreEqual(0, InitParams(options, config));
        return ThinkMeta::ConcurrentTasks::Samples::ctLame::FrameBits(config, index);
    };
    Assert::AreEqual(8 * 417, bits(44100, 9));  // 128 kbps: 417.96 bytes, never padded in VBR
    Assert::AreEqual(8 * 960, bits(48000, 14)); // 320 kbps
    Assert::AreEqual(8 * 144, bits(32000, 1));  // 32 kbps
}
}
;

} // namespace ctLameTests
