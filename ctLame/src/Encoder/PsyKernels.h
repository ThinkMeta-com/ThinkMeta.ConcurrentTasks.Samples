// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

#include "SimdFeatures.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    // Loops of the psychoacoustic model in a scalar and SIMD versions with the same bits
    // (PsySimd.inc). The scalar versions are in PsyPrepare.cpp and PsyResolve.cpp; the FFT has its
    // own (FftKernels.h).

    /// <summary>The length of the high pass of vbrpsy_attack_detection.</summary>
    inline constexpr std::int32_t NSFIRLEN = 21;

    /// <summary>The coefficients of that high pass (fircoef in psymodel.c).</summary>
    extern const std::array<float, 10> AttackFirCoef;

    /// <summary>The peak of each of the 9 sub-short blocks per psy channel (L, R, M, S).</summary>
    using SubShortPeaks = std::array<std::array<float, 9>, 4>;

    /// <summary>
    /// vbrpsy_attack_detection, the part without state: the high pass of fs/4 of each output
    /// channel, for joint stereo also mid and side of it, and the peak of |x| (at least 1) in each
    /// of the 9 sub-short blocks of every psy channel. buffer is as for PsyPrepare.
    /// </summary>
    using AttackPrepFunction = void (*)(const ChannelSamples& buffer, std::int32_t channels, std::int32_t psyChannels, SubShortPeaks& subShort) noexcept;

    void AttackPrepScalar(const ChannelSamples& buffer, std::int32_t channels, std::int32_t psyChannels, SubShortPeaks& subShort) noexcept;
    void AttackPrepSse41(const ChannelSamples& buffer, std::int32_t channels, std::int32_t psyChannels, SubShortPeaks& subShort) noexcept;
    void AttackPrepAvx2(const ChannelSamples& buffer, std::int32_t channels, std::int32_t psyChannels, SubShortPeaks& subShort) noexcept;

    /// <summary>The kernel of a SIMD level.</summary>
    AttackPrepFunction AttackPrepFor(SimdLevel level) noexcept;

    // the constants of vbrpsy_mask_add (psymodel.c), defined in PsyPrepare.cpp
    extern const std::array<float, 10>       MaskAddTable2;     // (1 + 10^(i/16 ...))^2 style factors, by 16 * log10 of the ratio
    extern const float                       MaskAddMaxI1;      // pow(10, (I1LIMIT + 1) / 16.0)
    extern const float                       MaskAddMaxI2;      // pow(10, (I2LIMIT + 1) / 16.0)
    extern const std::array<float, 9>        MaskTab;           // the masking table by mask index (tab in psymodel.c)
    extern const std::array<std::int32_t, 9> MaskAddDeltaTable; // tab_mask_add_delta

    /// <summary>
    /// The index (int) FastLog10X(ratio, 16) of vbrpsy_mask_add for ratios from 1 below
    /// MaskAddMaxI1 is the number of these 9 limits the ratio reaches (FastLog2 does not decrease
    /// there; a test checks every float). The SIMD kernels compare instead of taking the log.
    /// </summary>
    const float* MaskAddIndexLimits() noexcept;

    /// <summary>
    /// The spreading convolution of vbrpsy_compute_masking_l/_s without the average masking:
    /// for each partition b < npart the chain ecb = s3 * eb * tab[maskIdx] over its spreading
    /// range s3ind[b], combined with vbrpsy_mask_add in the order of the range. s3 holds the rows
    /// one after the other.
    /// </summary>
    using MaskChainsFunction = void (*)(const float* s3, const std::array<std::int32_t, 2>* s3ind, std::int32_t npart, const float* eb,
                                        const std::uint8_t* maskIdx, float* ecb) noexcept;

    void MaskChainsScalar(const float* s3, const std::array<std::int32_t, 2>* s3ind, std::int32_t npart, const float* eb, const std::uint8_t* maskIdx,
                          float* ecb) noexcept;
    void MaskChainsSse41(const float* s3, const std::array<std::int32_t, 2>* s3ind, std::int32_t npart, const float* eb, const std::uint8_t* maskIdx,
                         float* ecb) noexcept;
    void MaskChainsAvx2(const float* s3, const std::array<std::int32_t, 2>* s3ind, std::int32_t npart, const float* eb, const std::uint8_t* maskIdx,
                        float* ecb) noexcept;

    /// <summary>The kernel of a SIMD level.</summary>
    MaskChainsFunction MaskChainsFor(SimdLevel level) noexcept;

    /// <summary>What the long block pre-echo control of vbrpsy_compute_masking_l works on, for one channel.</summary>
    struct MaskingLongArgs {
        const float* maskingLower; // per partition
        float        maskingLowerGlobal;
        std::int32_t npart;
        std::int32_t blocktypeOld; // of the channel (L or R for M and S)
        const float* ecb;          // spread energy
        const float* limit;        // threshold limit for strongly tonal signals
        const float* eb;           // energy
        float*       nbL1;         // spread energy of the last granule, updated
        float*       nbL2;         // of the one before, updated
        float*       thr;          // the threshold
    };

    /// <summary>The pre-echo control of partition b: LAME's scalar code, shared by all versions.</summary>
    inline void MaskingLongOne(const MaskingLongArgs& a, std::int32_t b) noexcept
    {
        constexpr std::int32_t rpelev          = 2;
        constexpr std::int32_t rpelev2         = 16;
        constexpr double       NS_PREECHO_ATT2 = 0.3;

        float const maskingLower = a.maskingLower[b] * a.maskingLowerGlobal;
        float const ecb          = a.ecb[b];
        float       thr;

        // long block pre-echo control; not after a short block (all pre-echo was in there)
        if (a.blocktypeOld == SHORT_TYPE) {
            float const ecbLimit = rpelev * a.nbL1[b];
            if (ecbLimit > 0)
                thr = Min(ecb, ecbLimit);
            else {
                // LAME's Min() of a float and a double compares in double
                const double limit = a.eb[b] * NS_PREECHO_ATT2;
                thr                = static_cast<float>(ecb < limit ? ecb : limit);
            }
        } else {
            float ecbLimit2 = rpelev2 * a.nbL2[b];
            float ecbLimit1 = rpelev * a.nbL1[b];
            float ecbLimit;
            if (ecbLimit2 <= 0)
                ecbLimit2 = ecb;
            if (ecbLimit1 <= 0)
                ecbLimit1 = ecb;
            if (a.blocktypeOld == NORM_TYPE)
                ecbLimit = Min(ecbLimit1, ecbLimit2);
            else
                ecbLimit = ecbLimit1;
            thr = Min(ecb, ecbLimit);
        }
        a.nbL2[b] = a.nbL1[b];
        a.nbL1[b] = ecb;
        if (thr > a.limit[b])
            thr = a.limit[b];
        if (maskingLower > 1)
            thr *= maskingLower;
        if (thr > a.eb[b])
            thr = a.eb[b];
        if (maskingLower < 1)
            thr *= maskingLower;
        a.thr[b] = thr;
    }

    /// <summary>The pre-echo control of partitions 0..npart-1.</summary>
    using MaskingLongFunction = void (*)(const MaskingLongArgs& args) noexcept;

    void MaskingLongScalar(const MaskingLongArgs& args) noexcept;
    void MaskingLongSse41(const MaskingLongArgs& args) noexcept;
    void MaskingLongAvx2(const MaskingLongArgs& args) noexcept;

    /// <summary>The kernel of a SIMD level.</summary>
    MaskingLongFunction MaskingLongFor(SimdLevel level) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
