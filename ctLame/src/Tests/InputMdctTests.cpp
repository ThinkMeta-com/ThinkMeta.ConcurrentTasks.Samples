// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Fft.h"
#include "Input.h"
#include "Mdct.h"
#include "SessionConfig.h"
#include "StreamInput.h"
#include "TestData.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Lame;

// Input, FFT and MDCT. Bit-identity with LAME is proven by the hash compare of whole files;
// these tests pin the exactly specified parts (sample conversion, framing) and check
// the transforms for plausibility.

namespace LameTests {

    namespace {

        struct CollectingPcmSink : IPcmSink {
            std::int32_t left[8192];
            std::int32_t right[8192];
            std::int32_t count = 0;
            std::int32_t calls = 0;

            void AddSamples(const std::int32_t* l, const std::int32_t* r, std::int32_t n) override
            {
                for (std::int32_t i = 0; i < n; ++i) {
                    left[count + i]  = l[i];
                    right[count + i] = r[i];
                }
                count += n;
                ++calls;
            }
        };

        struct CountingFrameSink : IFrameSink {
            std::int32_t frames = 0;
            sample_t     first[2][2048];
            sample_t     last[2][2048];

            void EncodeFrame(const ChannelSamples& input) override
            {
                for (std::int32_t ch = 0; ch < 2 && input[ch] != nullptr; ++ch) {
                    if (frames == 0)
                        std::memcpy(first[ch], input[ch], FrameInputSize * sizeof(sample_t));
                    std::memcpy(last[ch], input[ch], FrameInputSize * sizeof(sample_t));
                }
                ++frames;
            }
        };

        SessionConfig MakeConfig(void (*adjust)(EncoderOptions&) = nullptr)
        {
            EncoderOptions options;
            if (adjust != nullptr)
                adjust(options);
            SessionConfig config;
            Assert::AreEqual(0, InitParams(options, config));
            return config;
        }

        // LAME's own estimate of the number of frames (lame_get_totalframes)
        std::int32_t EstimatedFrames(std::uint32_t samples)
        {
            const std::uint32_t perFrame = 1152;
            std::int32_t        frames   = static_cast<std::int32_t>(samples / perFrame);
            samples -= frames * perFrame;
            samples += 576;
            std::uint32_t endPadding = perFrame - (samples % perFrame);
            if (endPadding < 576)
                endPadding += perFrame;
            samples += endPadding;
            return frames + static_cast<std::int32_t>(samples / perFrame);
        }

    } // namespace

    TEST_CLASS(PcmReaderTests){public: TEST_METHOD(Converts16Bit){PcmReader reader;
    Assert::IsTrue(reader.Initialize({2, 16, false, false}));
    const auto        data = Bytes(0x01, 0x02, 0xFF, 0x7F, 0x00, 0x80, 0xFF, 0xFF);
    CollectingPcmSink sink;
    reader.Push(data.data(), data.size(), sink);
    reader.Finish(sink);
    Assert::AreEqual(2, sink.count);
    Assert::AreEqual(0x02010000, sink.left[0]);
    Assert::AreEqual(0x7FFF0000, sink.right[0]);
    Assert::AreEqual(static_cast<std::int32_t>(0x80000000u), sink.left[1]);
    Assert::AreEqual(static_cast<std::int32_t>(0xFFFF0000u), sink.right[1]);
} // namespace LameTests

TEST_METHOD(Converts8And24And32Bit)
{
    {
        PcmReader reader;
        Assert::IsTrue(reader.Initialize({1, 8, false, false}));
        const auto        data = Bytes(0x80, 0x00, 0xFF);
        CollectingPcmSink sink;
        reader.Push(data.data(), data.size(), sink);
        reader.Finish(sink);
        Assert::AreEqual(3, sink.count);
        Assert::AreEqual(0x007F0000, sink.left[0]); // unsigned 8 bit, 0x80 is silence
        Assert::AreEqual(static_cast<std::int32_t>(0x807F0000u), sink.left[1]);
        Assert::AreEqual(0x7F7F0000, sink.left[2]);
        Assert::AreEqual(0, sink.right[0]); // mono: the second channel is zero
    }
    {
        PcmReader reader;
        Assert::IsTrue(reader.Initialize({1, 24, false, false}));
        const auto        data = Bytes(0x01, 0x02, 0x03);
        CollectingPcmSink sink;
        reader.Push(data.data(), data.size(), sink);
        reader.Finish(sink);
        Assert::AreEqual(0x03020100, sink.left[0]);
    }
    {
        PcmReader reader;
        Assert::IsTrue(reader.Initialize({1, 32, false, false}));
        const auto        data = Bytes(0x01, 0x02, 0x03, 0x84);
        CollectingPcmSink sink;
        reader.Push(data.data(), data.size(), sink);
        reader.Finish(sink);
        Assert::AreEqual(static_cast<std::int32_t>(0x84030201u), sink.left[0]);
    }
}

TEST_METHOD(ConvertsFloat)
{
    PcmReader reader;
    Assert::IsTrue(reader.Initialize({1, 32, true, false}));
    const float       values[] = {0.5f, 1.0f, -1.0f, -0.25f, 2.0f, 0.0f};
    CollectingPcmSink sink;
    reader.Push(reinterpret_cast<const std::byte*>(values), sizeof(values), sink);
    reader.Finish(sink);
    Assert::AreEqual(6, sink.count);
    Assert::AreEqual(1073741824, sink.left[0]);
    Assert::AreEqual(INT_MAX, sink.left[1]);
    Assert::AreEqual(INT_MIN, sink.left[2]);
    Assert::AreEqual(-536870912, sink.left[3]);
    Assert::AreEqual(INT_MAX, sink.left[4]);
    Assert::AreEqual(0, sink.left[5]);
}

TEST_METHOD(RejectsOtherWidths)
{
    PcmReader reader;
    Assert::IsFalse(reader.Initialize({2, 12, false, false}));
}

// Blocks of 1152 frames like get_audio, no matter how the bytes arrive.
TEST_METHOD(DeliversBlocksOf1152Frames)
{
    PcmReader reader;
    Assert::IsTrue(reader.Initialize({2, 16, false, false}));
    static std::byte data[3000 * 4];
    for (std::int32_t i = 0; i < 3000 * 4; ++i)
        data[i] = static_cast<std::byte>(i);
    CollectingPcmSink sink;
    for (std::int32_t offset = 0; offset < 3000 * 4; offset += 1000)
        reader.Push(data + offset, 1000, sink);
    reader.Finish(sink);
    Assert::AreEqual(3000, sink.count);
    Assert::AreEqual(3, sink.calls); // 1152 + 1152 + 696
}

// LAME's last fread: whole samples only, and with an odd number of samples in a stereo
// file the channels of that block are shifted (the first sample is dropped).
TEST_METHOD(LastBlockQuirks)
{
    PcmReader reader;
    Assert::IsTrue(reader.Initialize({2, 16, false, false}));
    const auto        data = Bytes(0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0x77); // 5 samples and a stray byte
    CollectingPcmSink sink;
    reader.Push(data.data(), data.size(), sink);
    reader.Finish(sink);
    Assert::AreEqual(2, sink.count);
    Assert::AreEqual(0x02000000, sink.left[0]);
    Assert::AreEqual(0x03000000, sink.right[0]);
    Assert::AreEqual(0x04000000, sink.left[1]);
    Assert::AreEqual(0x05000000, sink.right[1]);
}

// --swap-channel on a mono file swaps in the zeroed second channel: LAME encodes silence.
TEST_METHOD(SwapOnMonoGivesSilence)
{
    PcmReader reader;
    Assert::IsTrue(reader.Initialize({1, 16, false, true}));
    const auto        data = Bytes(0x00, 0x40, 0x00, 0xC0);
    CollectingPcmSink sink;
    reader.Push(data.data(), data.size(), sink);
    reader.Finish(sink);
    Assert::AreEqual(0, sink.left[0]);
    Assert::AreEqual(0x40000000, sink.right[0]);
}
}
;

TEST_CLASS(SampleStreamTests){public: TEST_METHOD(FrameCountMatchesLame){const SessionConfig config = MakeConfig();
const std::int32_t  lengths[] = {0, 1, 575, 576, 577, 1151, 1152, 1153, 1375, 1376, 1377, 2000, 5000, 44100, 100001};
static std::int32_t zeros[100001];
for (std::int32_t length : lengths) {
    StreamInput input;
    Assert::IsTrue(input.Initialize(config));
    CountingFrameSink sink;
    for (std::int32_t done = 0; done < length;) {
        const std::int32_t n = length - done < 1152 ? length - done : 1152;
        input.AddInt(zeros, zeros, n, sink);
        done += n;
    }
    input.Flush(sink);
    Assert::AreEqual(EstimatedFrames(static_cast<std::uint32_t>(length)), sink.frames);
    Assert::AreEqual(sink.frames, input.Frames());
}
}

// The input starts after 528 zeros (ENCDELAY - MDCTDELAY); 16 bit samples come through
// unchanged (int << 16, then * 1/65536).
TEST_METHOD(FirstFrameLayout)
{
    const SessionConfig config = MakeConfig();
    StreamInput         input;
    Assert::IsTrue(input.Initialize(config));
    static std::int32_t left[2000];
    static std::int32_t right[2000];
    for (std::int32_t i = 0; i < 2000; ++i) {
        left[i]  = (i + 1) << 16;
        right[i] = -(i + 1) << 16;
    }
    CountingFrameSink sink;
    input.AddInt(left, right, 1152, sink);
    input.AddInt(left + 1152, right + 1152, 848, sink);
    Assert::AreEqual(1, sink.frames);
    for (std::int32_t i = 0; i < 528; ++i)
        Assert::AreEqual(0.0f, sink.first[0][i]);
    Assert::AreEqual(1.0f, sink.first[0][528]);
    Assert::AreEqual(-1.0f, sink.first[1][528]);
    Assert::AreEqual(1376.0f, sink.first[0][1903]);
}

TEST_METHOD(DownmixToMono)
{
    const SessionConfig config = MakeConfig([](EncoderOptions& o) { o.SetMode(Mono); });
    StreamInput         input;
    Assert::IsTrue(input.Initialize(config));
    static std::int32_t left[1904];
    static std::int32_t right[1904];
    for (std::int32_t i = 0; i < 1904; ++i) {
        left[i]  = 1000 << 16;
        right[i] = 3000 << 16;
    }
    CountingFrameSink sink;
    input.AddInt(left, right, 1904, sink);
    Assert::AreEqual(1, sink.frames);
    Assert::AreEqual(2000.0f, sink.first[0][528]);
}

// The padding goes through the transform like in LAME: a negative scale gives -0.0.
TEST_METHOD(NegativeScalePadsWithNegativeZero)
{
    const SessionConfig config = MakeConfig([](EncoderOptions& o) { o.SetScale(-1.0f); });
    StreamInput         input;
    Assert::IsTrue(input.Initialize(config));
    CountingFrameSink  sink;
    const std::int32_t one[1] = {1 << 16};
    input.AddInt(one, one, 1, sink);
    input.Flush(sink);
    Assert::IsTrue(sink.frames >= 1);
    Assert::AreEqual(-1.0f, sink.first[0][528]);
    Assert::IsTrue(std::signbit(sink.last[0][1903]));
    Assert::IsFalse(std::signbit(sink.first[0][0])); // the initial zeros stay +0.0
}
}
;

TEST_CLASS(FftTests){public:
                         // The energy of each line, as the psychoacoustic model computes it from the Hartley
                         // transform, matches a naive DFT of the windowed input.
                         TEST_METHOD(LongEnergyMatchesDft){FftWindows windows;
windows.Initialize();
static sample_t signal[BLKSIZE];
for (std::int32_t n = 0; n < BLKSIZE; ++n)
    signal[n] = static_cast<sample_t>(1000.0 * std::sin(2 * PI * 37.3 * n / BLKSIZE) + 300.0 * std::cos(2 * PI * 211.7 * n / BLKSIZE)
                                      + 50.0 * ((n * 7919) % 101 - 50));
std::array<float, BLKSIZE> x;
FftLong(windows, x, signal);

double maxEnergy = 0;
double worst     = 0;
for (std::int32_t k = 1; k < BLKSIZE / 2; ++k) {
    const double energy = (static_cast<double>(x[k]) * x[k] + static_cast<double>(x[BLKSIZE - k]) * x[BLKSIZE - k]) * 0.5;
    double       re = 0, im = 0;
    for (std::int32_t n = 0; n < BLKSIZE; ++n) {
        const double v = windows.window[n] * static_cast<double>(signal[n]);
        re += v * std::cos(2 * PI * n * k / BLKSIZE);
        im -= v * std::sin(2 * PI * n * k / BLKSIZE);
    }
    const double expected = re * re + im * im;
    maxEnergy             = expected > maxEnergy ? expected : maxEnergy;
    const double error    = std::fabs(energy - expected);
    worst                 = error > worst ? error : worst;
}
Assert::IsTrue(worst < 1e-4 * maxEnergy, L"FFT energy differs from the DFT");
}

TEST_METHOD(ShortEnergyMatchesDft)
{
    FftWindows windows;
    windows.Initialize();
    static sample_t signal[1024];
    for (std::int32_t n = 0; n < 1024; ++n)
        signal[n] = static_cast<sample_t>(800.0 * std::sin(2 * PI * 0.07 * n) + 40.0 * ((n * 31) % 17 - 8));
    std::array<std::array<float, BLKSIZE_s>, 3> x;
    FftShort(windows, x, signal);

    for (std::int32_t b = 0; b < 3; ++b) {
        const std::int32_t start     = 192 * (b + 1);
        double             maxEnergy = 0;
        double             worst     = 0;
        for (std::int32_t k = 1; k < BLKSIZE_s / 2; ++k) {
            const double energy = (static_cast<double>(x[b][k]) * x[b][k] + static_cast<double>(x[b][BLKSIZE_s - k]) * x[b][BLKSIZE_s - k]) * 0.5;
            double       re = 0, im = 0;
            for (std::int32_t n = 0; n < BLKSIZE_s; ++n) {
                const double w = n < BLKSIZE_s / 2 ? windows.window_s[n] : windows.window_s[BLKSIZE_s - 1 - n];
                const double v = w * signal[start + n];
                re += v * std::cos(2 * PI * n * k / BLKSIZE_s);
                im -= v * std::sin(2 * PI * n * k / BLKSIZE_s);
            }
            const double expected = re * re + im * im;
            maxEnergy             = expected > maxEnergy ? expected : maxEnergy;
            const double error    = std::fabs(energy - expected);
            worst                 = error > worst ? error : worst;
        }
        Assert::IsTrue(worst < 1e-4 * maxEnergy, L"short FFT energy differs from the DFT");
    }
}
}
;

TEST_CLASS(MdctTests){public: TEST_METHOD(SilenceGivesZeros){const SessionConfig config = MakeConfig();
MdctChannelState   state;
static sample_t    input[2048] = {};
const std::array<std::int32_t, 2> types = {NORM_TYPE, NORM_TYPE};
FrameLines                        xr;
MdctPrime(config.amp_filter, state, input);
MdctChannel(config.amp_filter, state, input, types, xr);
for (std::int32_t gr = 0; gr < 2; ++gr)
    for (std::int32_t i = 0; i < 576; ++i)
        Assert::AreEqual(0.0f, xr[gr][i]);
}

// A steady sine ends up in the MDCT line of its frequency (576 lines up to fs/2), and
// the lowpass (17.2 kHz at -V 4) empties the lines above it.
TEST_METHOD(SineLandsInItsLine)
{
    const SessionConfig config = MakeConfig();
    MdctChannelState    state;
    static sample_t     input[1152 * 8];
    const double        frequency = 3000.0;
    for (std::int32_t n = 0; n < 1152 * 8; ++n)
        input[n] = static_cast<sample_t>(10000.0 * std::sin(2 * PI * frequency * n / 44100.0));

    const std::array<std::int32_t, 2> types = {NORM_TYPE, NORM_TYPE};
    FrameLines                        xr;
    MdctPrime(config.amp_filter, state, input);
    for (std::int32_t frame = 0; frame < 5; ++frame)
        MdctChannel(config.amp_filter, state, input + frame * 1152, types, xr);

    std::int32_t peak    = 0;
    float        peakAbs = 0;
    for (std::int32_t i = 0; i < 576; ++i) {
        if (std::fabs(xr[1][i]) > peakAbs) {
            peakAbs = std::fabs(xr[1][i]);
            peak    = i;
        }
    }
    const std::int32_t expected = static_cast<std::int32_t>(frequency / 22050.0 * 576.0);
    Assert::IsTrue(peak >= expected - 2 && peak <= expected + 2, L"the sine is not in its line");
    for (std::int32_t i = 540; i < 576; ++i)
        Assert::AreEqual(0.0f, xr[1][i]);
}
}
;

} // namespace LameTests
