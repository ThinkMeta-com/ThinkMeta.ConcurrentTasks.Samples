// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "FftKernels.h"
#include "Mdct.h"
#include "MdctKernels.h"
#include "Psy.h"
#include "PsyKernels.h"
#include "PsyMath.h"
#include "Quantize.h"
#include "QuantizeKernels.h"
#include "SessionConfig.h"
#include "SimdFeatures.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctLame;

namespace ctLameTests {

    namespace {

        std::uint64_t NextRandom(std::uint64_t& state) noexcept
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        }

        // inputs of several kinds: audio-like, large, tiny, sparse, denormals and -0, random finite bits
        void Fill(float* x, std::int32_t size, std::int32_t kind, std::uint64_t& state) noexcept
        {
            for (std::int32_t i = 0; i < size; ++i) {
                const std::uint32_t bits    = static_cast<std::uint32_t>(NextRandom(state) >> 16);
                const float         uniform = static_cast<float>(bits & 0xffffff) / 16777216.0f * 2.0f - 1.0f;
                switch (kind % 6) {
                    case 0: x[i] = 32768.0f * uniform * static_cast<float>(0.5 - 0.5 * std::cos(6.283185307179586 * (i + 0.5) / size)); break;
                    case 1: x[i] = 1e6f * uniform; break;
                    case 2: x[i] = (bits & 0x7000000) == 0 ? uniform : 0.0f; break;
                    case 3: x[i] = (bits & 0x1000000) != 0 ? 1e-40f * uniform : -0.0f; break;
                    case 4: {
                        const std::uint32_t finite = (bits << 8 ^ static_cast<std::uint32_t>(NextRandom(state))) & 0xBFFFFFFFu;
                        std::memcpy(&x[i], &finite, sizeof(finite));
                        break;
                    }
                    default: x[i] = static_cast<float>(bits % 65536) - 32768.0f; break;
                }
            }
        }

        // the SIMD transform against the scalar one, bit for bit, for both FFT sizes of psymodel
        void CheckFht(SimdLevel level)
        {
            const CpuFeatures features = DetectCpuFeatures();
            if (ChooseSimdLevel(level, features) != level) {
                Logger::WriteMessage(L"SIMD level not supported by this CPU, skipped\n");
                return;
            }
            const FhtFunction fht   = FhtFor(level);
            std::uint64_t     state = 0x9E3779B97F4A7C15ull;
            alignas(32) float expected[BLKSIZE];
            alignas(32) float actual[BLKSIZE];
            for (const std::int32_t n : {BLKSIZE / 2, BLKSIZE_s / 2}) {
                for (std::int32_t round = 0; round < 3000; ++round) {
                    Fill(expected, 2 * n, round, state);
                    std::memcpy(actual, expected, sizeof(float) * 2 * n);
                    FhtScalar(expected, n);
                    fht(actual, n);
                    Assert::AreEqual(0, std::memcmp(expected, actual, sizeof(float) * 2 * n), L"FHT differs from the scalar version");
                }
            }
        }

        // the SIMD xrpow against the scalar one, bit for bit, for every line count
        void CheckXrpow(SimdLevel level)
        {
            if (ChooseSimdLevel(level, DetectCpuFeatures()) != level) {
                Logger::WriteMessage(L"SIMD level not supported by this CPU, skipped\n");
                return;
            }
            const XrpowFunction xrpow = QuantizeKernelsFor(level).xrpow;
            std::uint64_t       state = 0x2545F4914F6CDD1Dull;
            alignas(32) float   xr[576];
            alignas(32) float   expected[576];
            alignas(32) float   actual[576];
            for (std::int32_t round = 0; round < 60; ++round) {
                Fill(xr, 576, round, state);
                for (std::int32_t count = 1; count <= 576; ++count) {
                    float expectedSum, expectedMax, actualSum, actualMax;
                    std::memset(expected, 0x55, sizeof(expected));
                    std::memset(actual, 0x55, sizeof(actual));
                    XrpowScalar(xr, expected, count, expectedSum, expectedMax);
                    xrpow(xr, actual, count, actualSum, actualMax);
                    Assert::AreEqual(0, std::memcmp(expected, actual, sizeof(expected)), L"xrpow differs from the scalar version");
                    Assert::AreEqual(0, std::memcmp(&expectedSum, &actualSum, sizeof(float)), L"sum differs from the scalar version");
                    Assert::AreEqual(0, std::memcmp(&expectedMax, &actualMax, sizeof(float)), L"max differs from the scalar version");
                }
            }
        }

        // the quantizer's band kernels against the scalar ones on bands of 1..64 lines at steps
        // where the band stays quantizable, with the real tables
        void CheckQuantizeBands(SimdLevel level)
        {
            if (ChooseSimdLevel(level, DetectCpuFeatures()) != level) {
                Logger::WriteMessage(L"SIMD level not supported by this CPU, skipped\n");
                return;
            }
            EncoderOptions options;
            SessionConfig  config;
            Assert::AreEqual(0, InitParams(options, config));
            static QuantizeConstants constants; // large
            static AthTables         ath;
            QuantizeState            state;
            QuantizeInit(config, constants, ath, state);
            const QuantizeKernels kernels = QuantizeKernelsFor(level);

            std::uint64_t            randomState = 0xD1B54A32D192ED03ull;
            alignas(32) float        xr[576];
            alignas(32) float        xr34[576];
            alignas(32) std::int32_t expected[576];
            alignas(32) std::int32_t actual[576];
            for (std::int32_t round = 0; round < 24; ++round) {
                Fill(xr, 576, round, randomState);
                for (std::int32_t i = 0; i < 576; ++i) {
                    const float x = std::fabs(xr[i]);
                    xr34[i]       = std::sqrt(x * std::sqrt(x));
                }
                for (std::int32_t start = 0; start < 576; start += 1 + static_cast<std::int32_t>(NextRandom(randomState) % 7)) {
                    for (std::int32_t bw = 1; bw <= 64 && start + bw <= 576; ++bw) {
                        const float max = VecMaxScalar(xr34 + start, bw);
                        Assert::IsTrue(max == kernels.vecMax(xr34 + start, bw), L"VecMax differs from the scalar version");

                        // the smallest step that keeps the band quantizable, then any larger one
                        std::int32_t low = 255;
                        while (low > 0 && constants.ipow20[low - 1] * max <= IXMAX_VAL)
                            --low;
                        const std::int32_t sf     = low + static_cast<std::int32_t>(NextRandom(randomState) % (256 - low));
                        const float        step   = constants.pow20[sf + Q_MAX2];
                        const float        step34 = constants.ipow20[sf];

                        const float noise0 = SfbNoiseScalar(xr + start, xr34 + start, bw, step, step34, constants.pow43.data(), constants.adj43.data());
                        const float noise1 = kernels.sfbNoise(xr + start, xr34 + start, bw, step, step34, constants.pow43.data(), constants.adj43.data());
                        Assert::AreEqual(0, std::memcmp(&noise0, &noise1, sizeof(float)), L"SfbNoise differs from the scalar version");

                        std::memset(expected, 0x55, sizeof(expected));
                        std::memset(actual, 0x55, sizeof(actual));
                        QuantizeLinesScalar(xr34 + start, expected, bw, step34, constants.adj43.data());
                        kernels.quantizeLines(xr34 + start, actual, bw, step34, constants.adj43.data());
                        Assert::AreEqual(0, std::memcmp(expected, actual, sizeof(expected)), L"QuantizeLines differs from the scalar version");
                    }
                }
            }
        }

        // the SIMD attack preparation against the scalar one: stereo with mid/side, stereo, mono
        void CheckAttackPrep(SimdLevel level)
        {
            if (ChooseSimdLevel(level, DetectCpuFeatures()) != level) {
                Logger::WriteMessage(L"SIMD level not supported by this CPU, skipped\n");
                return;
            }
            const AttackPrepFunction attackPrep = AttackPrepFor(level);
            std::uint64_t            state      = 0x94D049BB133111EBull;
            alignas(32) float        left[1024];
            alignas(32) float        right[1024];
            const ChannelSamples     buffer       = {left, right};
            const std::int32_t       layouts[][2] = {{2, 4}, {2, 2}, {1, 1}};
            for (std::int32_t round = 0; round < 2000; ++round) {
                Fill(left, 1024, round, state);
                Fill(right, 1024, round / 3, state);
                for (const auto& layout : layouts) {
                    SubShortPeaks expected;
                    SubShortPeaks actual;
                    std::memset(&expected, 0x55, sizeof(expected));
                    std::memset(&actual, 0x55, sizeof(actual));
                    AttackPrepScalar(buffer, layout[0], layout[1], expected);
                    attackPrep(buffer, layout[0], layout[1], actual);
                    Assert::AreEqual(0, std::memcmp(&expected, &actual, sizeof(expected)), L"AttackPrep differs from the scalar version");
                }
            }
        }

        // the spreading convolution against the scalar one with the real partitions of long and
        // short blocks: energies over a wide range with zeros, all mask indices
        void CheckMaskChains(SimdLevel level)
        {
            if (ChooseSimdLevel(level, DetectCpuFeatures()) != level) {
                Logger::WriteMessage(L"SIMD level not supported by this CPU, skipped\n");
                return;
            }
            EncoderOptions options;
            SessionConfig  config;
            Assert::AreEqual(0, InitParams(options, config));
            static PsyConstants gd; // large
            static PsyState     psv;
            static AthTables    ath;
            static AthAdjust    adjust;
            Assert::AreEqual(0, PsyInit(config, gd, psv, ath, adjust));
            const MaskChainsFunction maskChains = MaskChainsFor(level);

            std::uint64_t state = 0xBF58476D1CE4E5B9ull;
            float         eb[CBANDS];
            std::uint8_t  maskIdx[CBANDS + 2];
            float         expected[CBANDS];
            float         actual[CBANDS];
            for (std::int32_t round = 0; round < 20000; ++round) {
                const PsyPartitions& parts = (round & 1) ? gd.s : gd.l;
                for (std::int32_t b = 0; b < CBANDS; ++b) {
                    const std::uint64_t r = NextRandom(state);
                    // energies from 1e-10 to 1e10, some zeros, some runs of equal values
                    eb[b]      = (r & 15) == 0 ? 0.0f : static_cast<float>(std::pow(10.0, static_cast<double>((r >> 8) % 2000) / 100.0 - 10.0));
                    maskIdx[b] = static_cast<std::uint8_t>((r >> 32) % 9);
                    if ((r >> 40) % 8 == 0 && b > 0)
                        eb[b] = eb[b - 1];
                }
                std::memset(expected, 0x55, sizeof(expected));
                std::memset(actual, 0x55, sizeof(actual));
                MaskChainsScalar(parts.s3.data(), parts.s3ind.data(), parts.npart, eb, maskIdx, expected);
                maskChains(parts.s3.data(), parts.s3ind.data(), parts.npart, eb, maskIdx, actual);
                Assert::AreEqual(0, std::memcmp(expected, actual, sizeof(float) * parts.npart), L"MaskChains differs from the scalar version");
            }
        }

        // the polyphase window against the scalar one with LAME's window table and random samples
        // over the whole range WindowSubband reads
        void CheckWindowSubband(SimdLevel level)
        {
            if (ChooseSimdLevel(level, DetectCpuFeatures()) != level) {
                Logger::WriteMessage(L"SIMD level not supported by this CPU, skipped\n");
                return;
            }
            const MdctKernels::WindowSubbandFunction windowSubband = MdctKernels::WindowSubbandFor(level);
            std::uint64_t                            state         = 0x8CB92BA72F3D8DD7ull;
            const float* const                       window        = MdctAnalysisWindow();
            alignas(32) float                        samples[1024];
            float                                    expected[SBLIMIT];
            float                                    actual[SBLIMIT];
            for (std::int32_t round = 0; round < 20000; ++round) {
                Fill(samples, 1024, round, state);
                const sample_t* const x1 = samples + 512; // reads x1[-271 + 1] .. x1[255]
                std::memset(expected, 0x55, sizeof(expected));
                std::memset(actual, 0x55, sizeof(actual));
                MdctKernels::WindowSubband(window, x1, expected);
                windowSubband(window, x1, actual);
                Assert::AreEqual(0, std::memcmp(expected, actual, sizeof(expected)), L"WindowSubband differs from the scalar version");
            }
        }

        // the long block pre-echo control against the scalar one: all block types, limits at and
        // below zero, masking lower below, at and above 1, every partition count
        void CheckMaskingLong(SimdLevel level)
        {
            if (ChooseSimdLevel(level, DetectCpuFeatures()) != level) {
                Logger::WriteMessage(L"SIMD level not supported by this CPU, skipped\n");
                return;
            }
            const MaskingLongFunction maskingLong = MaskingLongFor(level);
            std::uint64_t             state       = 0x9FB21C651E98DF25ull;
            float                     maskingLower[CBANDS], ecb[CBANDS], limit[CBANDS], eb[CBANDS];
            float                     nb1[2][CBANDS], nb2[2][CBANDS], thr[2][CBANDS];
            auto                      value = [&](std::int32_t zeroOneIn) {
                const std::uint64_t r = NextRandom(state);
                if (r % zeroOneIn == 0)
                    return 0.0f;
                return static_cast<float>(std::pow(10.0, static_cast<double>((r >> 8) % 1600) / 100.0 - 8.0));
            };
            for (std::int32_t round = 0; round < 20000; ++round) {
                const std::int32_t npart  = 1 + static_cast<std::int32_t>(NextRandom(state) % CBANDS);
                const std::int32_t type   = static_cast<std::int32_t>(NextRandom(state) % 4);
                const float        global = (round % 3 == 0) ? 1.0f : static_cast<float>(0.25 + static_cast<double>(NextRandom(state) % 1000) / 500.0);
                for (std::int32_t b = 0; b < CBANDS; ++b) {
                    maskingLower[b] = (NextRandom(state) % 4 == 0) ? 1.0f : static_cast<float>(static_cast<double>(NextRandom(state) % 2000) / 1000.0);
                    ecb[b]          = value(8);
                    limit[b]        = value(16);
                    eb[b]           = value(16);
                    nb1[0][b] = nb1[1][b] = value(4);
                    nb2[0][b] = nb2[1][b] = value(4);
                    thr[0][b] = thr[1][b] = -1.0f;
                }
                MaskingLongScalar({maskingLower, global, npart, type, ecb, limit, eb, nb1[0], nb2[0], thr[0]});
                maskingLong({maskingLower, global, npart, type, ecb, limit, eb, nb1[1], nb2[1], thr[1]});
                Assert::AreEqual(0, std::memcmp(thr[0], thr[1], sizeof(thr[0])), L"MaskingLong thresholds differ from the scalar version");
                Assert::AreEqual(0, std::memcmp(nb1[0], nb1[1], sizeof(nb1[0])), L"MaskingLong nb_l1 differs from the scalar version");
                Assert::AreEqual(0, std::memcmp(nb2[0], nb2[1], sizeof(nb2[0])), L"MaskingLong nb_l2 differs from the scalar version");
            }
        }

    } // namespace

    TEST_CLASS(SimdTests){public: TEST_METHOD(FhtSse41MatchesScalar){CheckFht(SimdLevel::Sse41);
} // namespace ctLameTests

TEST_METHOD(FhtAvx2MatchesScalar)
{
    CheckFht(SimdLevel::Avx2);
}

TEST_METHOD(XrpowSse41MatchesScalar)
{
    CheckXrpow(SimdLevel::Sse41);
}

TEST_METHOD(XrpowAvx2MatchesScalar)
{
    CheckXrpow(SimdLevel::Avx2);
}

TEST_METHOD(QuantizeBandsSse41MatchScalar)
{
    CheckQuantizeBands(SimdLevel::Sse41);
}

TEST_METHOD(QuantizeBandsAvx2MatchScalar)
{
    CheckQuantizeBands(SimdLevel::Avx2);
}

TEST_METHOD(AttackPrepSse41MatchesScalar)
{
    CheckAttackPrep(SimdLevel::Sse41);
}

TEST_METHOD(AttackPrepAvx2MatchesScalar)
{
    CheckAttackPrep(SimdLevel::Avx2);
}

TEST_METHOD(MaskChainsSse41MatchesScalar)
{
    CheckMaskChains(SimdLevel::Sse41);
}

TEST_METHOD(MaskChainsAvx2MatchesScalar)
{
    CheckMaskChains(SimdLevel::Avx2);
}

// the index limits of the SIMD vbrpsy_mask_add give the index of FastLog10X for every float ratio
// from 1 below MaskAddMaxI1
TEST_METHOD(MaskAddIndexLimitsMatchFastLog)
{
    const float*  limits = MaskAddIndexLimits();
    std::uint32_t bits;
    std::uint32_t high;
    const float   one = 1.0f;
    std::memcpy(&bits, &one, sizeof(bits));
    std::memcpy(&high, &MaskAddMaxI1, sizeof(high));
    for (; bits < high; ++bits) {
        float ratio;
        std::memcpy(&ratio, &bits, sizeof(ratio));
        std::int32_t count = 0;
        for (std::int32_t k = 0; k < 9; ++k)
            count += ratio >= limits[k] ? 1 : 0;
        const std::int32_t index = static_cast<std::int32_t>(FastLog10X(ratio, 16.0f));
        if (count != index)
            Assert::Fail(L"index limits differ from FastLog10X");
    }
}

TEST_METHOD(WindowSubbandSse41MatchesScalar)
{
    CheckWindowSubband(SimdLevel::Sse41);
}

TEST_METHOD(WindowSubbandAvx2MatchesScalar)
{
    CheckWindowSubband(SimdLevel::Avx2);
}

TEST_METHOD(MaskingLongSse41MatchesScalar)
{
    CheckMaskingLong(SimdLevel::Sse41);
}

TEST_METHOD(MaskingLongAvx2MatchesScalar)
{
    CheckMaskingLong(SimdLevel::Avx2);
}

TEST_METHOD(ChooseCapsAtTheCpu)
{
    CpuFeatures none;
    Assert::IsTrue(ChooseSimdLevel(SimdLevel::Avx2, none) == SimdLevel::Scalar);
    CpuFeatures sse;
    sse.sse41 = true;
    Assert::IsTrue(ChooseSimdLevel(SimdLevel::Avx2, sse) == SimdLevel::Sse41);
    CpuFeatures all;
    all.sse41 = all.avx2 = true;
    Assert::IsTrue(ChooseSimdLevel(SimdLevel::Sse41, all) == SimdLevel::Sse41);
    SimdLevel level = SimdLevel::Scalar;
    Assert::IsTrue(ParseSimdLevel(L"avx2", level) && level == SimdLevel::Avx2);
    Assert::IsFalse(ParseSimdLevel(L"avx512", level));
}
}
;

} // namespace ctLameTests
