// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "Input.h"
#include "Psy.h"
#include "SessionConfig.h"
#include "StreamInput.h"
#include "TestData.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Lame;

// The psychoacoustic model. The expected checksums were recorded with an oracle that runs
// LAME's own L3psycho_anal_vbr and adjust_ATH (reduced libmp3lame) on the same frames; the port
// must return exactly the same bits. The inputs are a corpus file and the synth files named by
// CTLAME_TEST_CORPUS and CTLAME_TEST_SYNTH (TestData.h); a test is skipped when its
// file is missing.

namespace LameTests {

    namespace {

        constexpr std::int32_t MaxFrames = 3000;

        struct PsyRun : IPcmSink, IFrameSink {
            SessionConfig config;
            PsyConstants  constants;
            PsyState      state;
            AthTables     ath;
            AthAdjust     athAdjust;
            PsyResult     result;
            StreamInput   input;
            Checksum      checksum;
            float         maskingLower = 1;
            std::int32_t  frames       = 0;

            void AddSamples(const std::int32_t* left, const std::int32_t* right, std::int32_t count) override { input.AddInt(left, right, count, *this); }

            void EncodeFrame(const ChannelSamples& frame) override
            {
                if (frames >= MaxFrames)
                    return;
                GranuleChannel<PsyRatio>            maskingLr, maskingMs;
                std::array<std::array<float, 4>, 2> energy;
                GranuleChannel<float>               pe   = {};
                GranuleChannel<float>               peMs = {};
                const std::int32_t channels    = config.channels_out;
                const std::int32_t psyChannels = config.mode == JointStereo ? 4 : channels;
                std::memset(&maskingLr, 0, sizeof(maskingLr));
                std::memset(&maskingMs, 0, sizeof(maskingMs));
                std::memset(&energy, 0, sizeof(energy));

                for (std::int32_t gr = 0; gr < 2; ++gr) {
                    ChannelSamples buffer;
                    for (std::int32_t ch = 0; ch < channels; ++ch)
                        buffer[ch] = frame[ch] + 576 + gr * 576 - FFTOFFSET;
                    if (channels == 1)
                        buffer[1] = buffer[0];
                    std::array<std::int32_t, 2> blocktype;
                    PsyAnalyze(config, constants, state, ath, athAdjust.factor, maskingLower, result, buffer, gr, maskingLr, maskingMs, pe[gr], peMs[gr],
                               energy[gr], blocktype);
                    for (std::int32_t ch = 0; ch < channels; ++ch) {
                        checksum.Add(&maskingLr[gr][ch], sizeof(PsyRatio));
                        if (psyChannels > 2)
                            checksum.Add(&maskingMs[gr][ch], sizeof(PsyRatio));
                        checksum.Add(&pe[gr][ch], sizeof(float));
                        if (psyChannels > 2)
                            checksum.Add(&peMs[gr][ch], sizeof(float));
                        checksum.Add(&blocktype[ch], sizeof(std::int32_t));
                    }
                    checksum.Add(energy[gr].data(), sizeof(float) * psyChannels);
                }
                AdjustAth(config, ath, athAdjust, result);
                checksum.Add(&athAdjust.factor, sizeof(float));
                checksum.Add(&athAdjust.limit, sizeof(float));
                for (std::int32_t gr = 0; gr < 2; ++gr)
                    for (std::int32_t ch = 0; ch < channels; ++ch)
                        checksum.Add(&result.loudness_sq[gr][ch], sizeof(float));
                // VBR_new_prepare of the quantizer
                maskingLower = static_cast<float>(std::pow(10.0, config.mask_adjust * 0.1));
                ++frames;
            }
        };

        void Run(const wchar_t* name, const wchar_t* path, void (*adjust)(EncoderOptions&), std::int32_t expectedFrames, std::uint64_t expected)
        {
            WavData wav;
            if (!wav.Load(path)) {
                wchar_t text[400];
                Logger::WriteMessage(MissingInputText(text, name, path));
                return;
            }

            EncoderOptions options;
            options.SetInSamplerate(wav.rate);
            options.SetNumChannels(wav.channels);
            if (adjust != nullptr)
                adjust(options);

            const auto run = std::make_unique<PsyRun>(); // too large for the stack
            Assert::AreEqual(0, InitParams(options, run->config));
            Assert::AreEqual(0, PsyInit(run->config, run->constants, run->state, run->ath, run->athAdjust));
            Assert::IsTrue(run->input.Initialize(run->config));

            PcmReader reader;
            Assert::IsTrue(reader.Initialize({wav.channels, 16, false, false}));
            const std::size_t chunk = 65536;
            for (std::size_t pos = 0; pos < wav.size && run->frames < MaxFrames; pos += chunk)
                reader.Push(wav.data + pos, Min(chunk, wav.size - pos), *run);
            if (run->frames < MaxFrames) {
                reader.Finish(*run);
                run->input.Flush(*run);
            }

            const std::int32_t  frames = run->frames;
            const std::uint64_t actual = run->checksum.value;

            wchar_t text[200];
            swprintf_s(text, L"%ls: %d frames 0x%016llX", name, frames, actual);
            Logger::WriteMessage(text);
            Assert::AreEqual(expectedFrames, frames, name);
            Assert::AreEqual(expected, actual, name);
        }

        const wchar_t* const Corpus = CorpusFile();

        void RunSynth(const wchar_t* name, const wchar_t* file, void (*adjust)(EncoderOptions&), std::int32_t expectedFrames, std::uint64_t expected)
        {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%ls%ls", SynthDir(), file);
            Run(name, path, adjust, expectedFrames, expected);
        }

    } // namespace

    TEST_CLASS(PsyGoldenTests){public: TEST_METHOD(V4){Run(L"V4", Corpus, [](EncoderOptions& o) { o.SetVbrQuality(4.0f); }, 3000, 0x868D5F2CD3D92041ull);
} // namespace LameTests

TEST_METHOD(V0)
{
    Run(L"V0", Corpus, [](EncoderOptions& o) { o.SetVbrQuality(0.0f); }, 3000, 0x043AFCE30D1CD063ull);
}

TEST_METHOD(V9999)
{
    Run(L"V9999", Corpus, [](EncoderOptions& o) { o.SetVbrQuality(9.999f); }, 3000, 0xA3356836B5F88500ull);
}

TEST_METHOD(SimpleStereoV2)
{
    Run(
        L"SimpleStereoV2", Corpus,
        [](EncoderOptions& o) {
            o.SetVbrQuality(2.0f);
            o.SetMode(Stereo);
        },
        3000, 0x6F8D3106E52151A2ull);
}

TEST_METHOD(MonoDownmixV5)
{
    Run(
        L"MonoDownmixV5", Corpus,
        [](EncoderOptions& o) {
            o.SetVbrQuality(5.0f);
            o.SetMode(Mono);
        },
        3000, 0x2E4CE0BF19C886F8ull);
}

TEST_METHOD(ExperimentalZV3)
{
    Run(
        L"ExperimentalZV3", Corpus,
        [](EncoderOptions& o) {
            o.SetVbrQuality(3.0f);
            o.SetExperimentalZ(1);
        },
        3000, 0xEB7ACB566A313EACull);
}

TEST_METHOD(AthaaSensitivityV4)
{
    Run(
        L"AthaaSensitivityV4", Corpus,
        [](EncoderOptions& o) {
            o.SetVbrQuality(4.0f);
            o.SetAthaaSensitivity(6.0f);
        },
        3000, 0x1A7B1CBF280BD8D6ull);
}

TEST_METHOD(Rate32kV73)
{
    RunSynth(L"Rate32kV73", L"s16-stereo-32000.wav", [](EncoderOptions& o) { o.SetVbrQuality(7.3f); }, 767, 0x0E0FB2A117DD5B3Aull);
}

TEST_METHOD(Rate48kV15)
{
    RunSynth(L"Rate48kV15", L"s16-stereo-48000.wav", [](EncoderOptions& o) { o.SetVbrQuality(1.5f); }, 767, 0xABDDC233C3B953FEull);
}

TEST_METHOD(MonoInputV4)
{
    RunSynth(L"MonoInputV4", L"s16-mono-44100.wav", [](EncoderOptions& o) { o.SetVbrQuality(4.0f); }, 767, 0xA3F7E83F43B04B3Aull);
}

TEST_METHOD(ClippedV2)
{
    RunSynth(L"ClippedV2", L"s16-stereo-44100-clipped.wav", [](EncoderOptions& o) { o.SetVbrQuality(2.0f); }, 767, 0x04809653B7024C30ull);
}
}
;

} // namespace LameTests
