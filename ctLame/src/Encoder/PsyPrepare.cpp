// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Psy.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "PsyInternal.h"
#include "PsyMath.h"
#include "SessionConfig.h"

// Port of psymodel.c, the part of L3psycho_anal_vbr without state (PsyPrepare): attack peaks, FFTs,
// energies and the spreading; also the scalar kernels and the constant tables they use. Every
// expression keeps LAME's precision: float (float) where LAME computes in float, double where one
// of its double constants or math functions turns the arithmetic into double before the result is
// rounded. PsyInit.cpp has psymodel_init, PsyResolve.cpp the part with state.

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    namespace {

        constexpr double VO_SCALE = 1. / (14752 * 14752) / (BLKSIZE / 2);

        constexpr std::int32_t LastTabEntry = static_cast<std::int32_t>(sizeof(MaskTab) / sizeof(MaskTab[0])) - 1;

        // tab_mask_add_delta (psymodel.c)
        std::int32_t MaskAddDelta(std::int32_t i) noexcept
        {
            return MaskAddDeltaTable[i];
        }

        // vbrpsy_mask_add: addition of simultaneous masking (Naoki Shibata 2000/7)
        float MaskAdd(float m1, float m2, std::int32_t b, std::int32_t delta) noexcept
        {
            if (m1 < 0)
                m1 = 0;
            if (m2 < 0)
                m2 = 0;
            if (m1 <= 0)
                return m2;
            if (m2 <= 0)
                return m1;
            const float ratio = m2 > m1 ? m2 / m1 : m1 / m2;
            if (std::abs(b) <= delta) { // approximately, 1 bark = 3 partitions
                if (ratio >= MaskAddMaxI1)
                    return m1 + m2;
                const std::int32_t i = static_cast<std::int32_t>(FastLog10X(ratio, 16.0f));
                return (m1 + m2) * MaskAddTable2[i];
            }
            if (ratio < MaskAddMaxI2)
                return m1 + m2;
            return m1 < m2 ? m2 : m1;
        }

        // calc_energy: energy, maximum and average of each partition band
        void PartitionEnergy(const PsyPartitions& l, const float* fftenergy, float* eb, float* max, float* avg) noexcept
        {
            for (std::int32_t b = 0, j = 0; b < l.npart; ++b) {
                float ebb = 0;
                float m   = 0;
                for (std::int32_t i = 0; i < l.numlines[b]; ++i, ++j) {
                    const float el = fftenergy[j];
                    ebb += el;
                    if (m < el)
                        m = el;
                }
                eb[b]  = ebb;
                max[b] = m;
                avg[b] = ebb * l.rnumlines[b];
            }
        }

        // calc_mask_index_l / vbrpsy_calc_mask_index_s: tonality index of each partition band
        // from the peak-to-average ratio of it and its neighbours
        void MaskIndex(const PsyPartitions& gd, const float* max, const float* avg, std::uint8_t* maskIdx) noexcept
        {
            float        m, a;
            std::int32_t b = 0;
            std::int32_t k;

            a = avg[b] + avg[b + 1];
            if (a > 0.0f) {
                m = max[b];
                if (m < max[b + 1])
                    m = max[b + 1];
                a = 20.0f * (m * 2.0f - a) / (a * (gd.numlines[b] + gd.numlines[b + 1] - 1));
                k = static_cast<std::int32_t>(a);
                if (k > LastTabEntry)
                    k = LastTabEntry;
                maskIdx[b] = static_cast<std::uint8_t>(k);
            } else {
                maskIdx[b] = 0;
            }

            for (b = 1; b < gd.npart - 1; b++) {
                a = avg[b - 1] + avg[b] + avg[b + 1];
                if (a > 0.0f) {
                    m = max[b - 1];
                    if (m < max[b])
                        m = max[b];
                    if (m < max[b + 1])
                        m = max[b + 1];
                    a = 20.0f * (m * 3.0f - a) / (a * (gd.numlines[b - 1] + gd.numlines[b] + gd.numlines[b + 1] - 1));
                    k = static_cast<std::int32_t>(a);
                    if (k > LastTabEntry)
                        k = LastTabEntry;
                    maskIdx[b] = static_cast<std::uint8_t>(k);
                } else {
                    maskIdx[b] = 0;
                }
            }

            a = avg[b - 1] + avg[b];
            if (a > 0.0f) {
                m = max[b - 1];
                if (m < max[b])
                    m = max[b];
                a = 20.0f * (m * 2.0f - a) / (a * (gd.numlines[b - 1] + gd.numlines[b] - 1));
                k = static_cast<std::int32_t>(a);
                if (k > LastTabEntry)
                    k = LastTabEntry;
                maskIdx[b] = static_cast<std::uint8_t>(k);
            } else {
                maskIdx[b] = 0;
            }
        }

        // vbrpsy_compute_fft_l: FFT (or M/S from L/R) and line energies; the total energy is in EnergySums
        void ComputeFftLong(const PsyConstants& gd, const ChannelSamples& buffer, std::int32_t chn, std::array<float, HBLKSIZE>& fftenergy,
                            std::array<float, BLKSIZE>* wsampL) noexcept
        {
            if (chn < 2) {
                FftLong(gd.fft, *wsampL, buffer[chn]);
            } else if (chn == 2) {
                const float sqrt2Half = static_cast<float>(SQRT2 * 0.5f);
                // FFT data for mid and side channel is derived from L & R
                for (std::int32_t j = BLKSIZE - 1; j >= 0; --j) {
                    const float l = wsampL[0][j];
                    const float r = wsampL[1][j];
                    wsampL[0][j]  = (l + r) * sqrt2Half;
                    wsampL[1][j]  = (l - r) * sqrt2Half;
                }
            }

            fftenergy[0] = wsampL[0][0];
            fftenergy[0] *= fftenergy[0];
            for (std::int32_t j = BLKSIZE / 2 - 1; j >= 0; --j) {
                const float re             = (*wsampL)[BLKSIZE / 2 - j];
                const float im             = (*wsampL)[BLKSIZE / 2 + j];
                fftenergy[BLKSIZE / 2 - j] = (re * re + im * im) * 0.5f;
            }
        }

        // the sums over the long block energies of every psy channel: the total energy above
        // line 10 and, for L and R, psycho_loudness_approx (loudness^2, about 1.0 for signals near
        // clipping). Each is a serial sum in LAME's order; the channels' sums are independent, so
        // they run side by side instead of one after the other.
        template <std::int32_t Channels>
        void EnergySums(const std::array<std::array<float, HBLKSIZE>, 4>& fftenergy, const float* eqlW, std::array<float, 4>& totEnergy,
                        std::array<float, 2>& loudness) noexcept
        {
            constexpr std::int32_t LoudChannels = Channels < 2 ? Channels : 2;

            std::array<float, Channels> total;
            for (std::int32_t c = 0; c < Channels; ++c)
                total[c] = 0.0f;
            for (std::int32_t j = 11; j < HBLKSIZE; j++) {
                for (std::int32_t c = 0; c < Channels; ++c)
                    total[c] += fftenergy[c][j];
            }
            for (std::int32_t c = 0; c < Channels; ++c)
                totEnergy[c] = total[c];

            std::array<float, LoudChannels> loudnessPower;
            for (std::int32_t c = 0; c < LoudChannels; ++c)
                loudnessPower[c] = 0.0f;
            for (std::int32_t i = 0; i < BLKSIZE / 2; ++i) {
                for (std::int32_t c = 0; c < LoudChannels; ++c)
                    loudnessPower[c] += fftenergy[c][i] * eqlW[i];
            }
            for (std::int32_t c = 0; c < LoudChannels; ++c)
                loudness[c] = static_cast<float>(loudnessPower[c] * VO_SCALE);
        }

        // vbrpsy_compute_masking_l, the part without state: energy and tonality of each
        // partition and the spreading; limit is the threshold limit for strongly tonal signals
        void SpreadLong(const PsyPartitions& gdl, MaskChainsFunction maskChains, const float* fftenergy, PsyBandPrep& out) noexcept
        {
            std::array<float, CBANDS>            max, avg;
            std::array<std::uint8_t, CBANDS + 2> maskIdxL;
            std::int32_t                         b;

            PartitionEnergy(gdl, fftenergy, out.eb.data(), max.data(), avg.data());
            MaskIndex(gdl, max.data(), avg.data(), maskIdxL.data());

            // convolve the partitioned energy with the spreading function, then the average masking
            std::array<float, CBANDS> ecbRaw;
            maskChains(gdl.s3.data(), gdl.s3ind.data(), gdl.npart, out.eb.data(), maskIdxL.data(), ecbRaw.data());
            for (b = 0; b < gdl.npart; b++) {
                std::int32_t       kk   = gdl.s3ind[b][0];
                const std::int32_t last = gdl.s3ind[b][1];
                std::int32_t       dd   = 0;
                for (; kk <= last; ++kk)
                    dd += maskIdxL[kk];
                const std::int32_t ddN = last - gdl.s3ind[b][0] + 1;
                dd                     = (1 + 2 * dd) / (2 * ddN);
                const float avgMask    = MaskTab[dd] * 0.5f;
                float       ecb        = ecbRaw[b];
                ecb *= avgMask;
                out.ecb[b] = ecb;

                // limit THR for strongly tonal signals, where it would exceed EB
                float x = max[b];
                x *= gdl.minval[b];
                x *= avgMask;
                out.limit[b] = x;
            }
            for (; b < CBANDS; ++b) {
                out.eb[b]    = 0;
                out.ecb[b]   = 0;
                out.limit[b] = 0;
            }
        }

    } // namespace

    namespace PsyDetail {

        // vbrpsy_compute_fft_s
        void ComputeFftShort(const PsyConstants& gd, const ChannelSamples& buffer, std::int32_t chn, std::int32_t sblock,
                             std::array<std::array<float, HBLKSIZE_s>, 3>& fftenergyS, std::array<std::array<float, BLKSIZE_s>, 3>* wsampS) noexcept
        {
            if (sblock == 0 && chn < 2)
                FftShort(gd.fft, *wsampS, buffer[chn]);
            if (chn == 2) {
                const float sqrt2Half = static_cast<float>(SQRT2 * 0.5f);
                // FFT data for mid and side channel is derived from L & R
                for (std::int32_t j = BLKSIZE_s - 1; j >= 0; --j) {
                    const float l        = wsampS[0][sblock][j];
                    const float r        = wsampS[1][sblock][j];
                    wsampS[0][sblock][j] = (l + r) * sqrt2Half;
                    wsampS[1][sblock][j] = (l - r) * sqrt2Half;
                }
            }

            fftenergyS[sblock][0] = (*wsampS)[sblock][0];
            fftenergyS[sblock][0] *= fftenergyS[sblock][0];
            for (std::int32_t j = BLKSIZE_s / 2 - 1; j >= 0; --j) {
                const float re                        = (*wsampS)[sblock][BLKSIZE_s / 2 - j];
                const float im                        = (*wsampS)[sblock][BLKSIZE_s / 2 + j];
                fftenergyS[sblock][BLKSIZE_s / 2 - j] = (re * re + im * im) * 0.5f;
            }
        }

        // vbrpsy_compute_masking_s, the part without state: energy, tonality and the spreading
        // of one short block; limit is the threshold limit for strongly tonal signals
        void SpreadShort(const PsyPartitions& gds, MaskChainsFunction maskChains, const float* fftenergyS, PsyBandPrep& out) noexcept
        {
            std::array<float, CBANDS>        max, avg;
            std::array<std::uint8_t, CBANDS> maskIdxS;
            std::int32_t                     b, j;

            max.fill(0);
            avg.fill(0);
            for (b = j = 0; b < gds.npart; ++b) {
                float              ebb = 0;
                float              m   = 0;
                const std::int32_t n   = gds.numlines[b];
                for (std::int32_t i = 0; i < n; ++i, ++j) {
                    const float el = fftenergyS[j];
                    ebb += el;
                    if (m < el)
                        m = el;
                }
                out.eb[b] = ebb;
                max[b]    = m;
                avg[b]    = ebb * gds.rnumlines[b];
            }
            MaskIndex(gds, max.data(), avg.data(), maskIdxS.data());

            // convolve the partitioned energy with the spreading function, then the average masking
            std::array<float, CBANDS> ecbRaw;
            maskChains(gds.s3.data(), gds.s3ind.data(), gds.npart, out.eb.data(), maskIdxS.data(), ecbRaw.data());
            for (b = 0; b < gds.npart; b++) {
                std::int32_t       kk   = gds.s3ind[b][0];
                const std::int32_t last = gds.s3ind[b][1];
                std::int32_t       dd   = 0;
                for (; kk <= last; ++kk)
                    dd += maskIdxS[kk];
                const std::int32_t ddN = last - gds.s3ind[b][0] + 1;
                dd                     = (1 + 2 * dd) / (2 * ddN);
                const float avgMask    = MaskTab[dd] * 0.5f;
                float       ecb        = ecbRaw[b];
                ecb *= avgMask;
                out.ecb[b] = ecb;

                // limit THR for strongly tonal signals, where it would exceed EB
                float x = max[b];
                x *= gds.minval[b];
                x *= avgMask;
                out.limit[b] = x;
            }
            for (; b < CBANDS; ++b) {
                out.eb[b]    = 0;
                out.ecb[b]   = 0;
                out.limit[b] = 0;
            }
        }

    } // namespace PsyDetail

    // constant tables taken over from LAME (psymodel.c) as data
    const float                 MaskAddMaxI1 = LameFloat(3.6517412725483771); // pow(10, (I1LIMIT + 1) / 16.0)
    const float                 MaskAddMaxI2 = LameFloat(31.622776601683793); // pow(10, (I2LIMIT + 1) / 16.0)
    const std::array<float, 10> MaskAddTable2
        = {LameFloat(1.33352 * 1.33352), LameFloat(1.35879 * 1.35879), LameFloat(1.38454 * 1.38454), LameFloat(1.39497 * 1.39497), LameFloat(1.40548 * 1.40548),
           LameFloat(1.3537 * 1.3537),   LameFloat(1.30382 * 1.30382), LameFloat(1.22321 * 1.22321), LameFloat(1.14758 * 1.14758), 1.0f};

    const float* MaskAddIndexLimits() noexcept
    {
        struct Limits {
            std::array<float, 9> values;
        };
        static const Limits limits = [] {
            auto bitsOf = [](float x) {
                std::uint32_t bits;
                std::memcpy(&bits, &x, sizeof(bits));
                return bits;
            };
            auto index = [](std::uint32_t bits) {
                float x;
                std::memcpy(&x, &bits, sizeof(x));
                return static_cast<std::int32_t>(FastLog10X(x, 16.0f));
            };
            // the smallest ratio from 1 up to MaskAddMaxI1 with an index of at least k, by binary
            // search over the float bits (ordered like the values); MaskAddMaxI1 if there is none,
            // the index is not used from there on
            Limits              l{};
            const std::uint32_t high = bitsOf(MaskAddMaxI1);
            for (std::int32_t k = 1; k <= 9; ++k) {
                std::uint32_t lo = bitsOf(1.0f);
                std::uint32_t hi = high;
                if (index(hi) >= k) {
                    while (lo < hi) {
                        const std::uint32_t mid = lo + (hi - lo) / 2;
                        if (index(mid) >= k)
                            hi = mid;
                        else
                            lo = mid + 1;
                    }
                }
                std::memcpy(&l.values[k - 1], &hi, sizeof(float));
            }
            return l;
        }();
        return limits.values.data();
    }

    // the masking table: from 0 dB (tone masking noise) to 9.3 dB (noise masking tone)
    const std::array<float, 9> MaskTab = {1.0f,
                                          LameFloat(0.79433),
                                          LameFloat(0.63096),
                                          LameFloat(0.63096),
                                          LameFloat(0.63096),
                                          LameFloat(0.63096),
                                          LameFloat(0.63096),
                                          LameFloat(0.25119),
                                          LameFloat(0.11749)};

    const std::array<std::int32_t, 9> MaskAddDeltaTable = {2, 2, 2, 1, 1, 1, 0, 0, -1};

    // the spreading convolution as vbrpsy_compute_masking_l/_s compute it
    void MaskChainsScalar(const float* s3, const std::array<std::int32_t, 2>* s3ind, std::int32_t npart, const float* eb, const std::uint8_t* maskIdx,
                          float* ecbOut) noexcept
    {
        std::int32_t k = 0;
        for (std::int32_t b = 0; b < npart; b++) {
            std::int32_t       kk    = s3ind[b][0];
            const std::int32_t last  = s3ind[b][1];
            const std::int32_t delta = MaskAddDelta(maskIdx[b]);

            float ecb = s3[k] * eb[kk] * MaskTab[maskIdx[kk]];
            ++k, ++kk;
            while (kk <= last) {
                const float x = s3[k] * eb[kk] * MaskTab[maskIdx[kk]];
                ecb           = MaskAdd(ecb, x, kk - b, delta);
                ++k, ++kk;
            }
            ecbOut[b] = ecb;
        }
    }

    MaskChainsFunction MaskChainsFor(SimdLevel level) noexcept
    {
        switch (level) {
            case SimdLevel::Sse41: return MaskChainsSse41;
            case SimdLevel::Avx2: return MaskChainsAvx2;
            default: return MaskChainsScalar;
        }
    }

    // constant table taken over from LAME (psymodel.c) as data
    const std::array<float, 10> AttackFirCoef
        = {LameFloat(-8.65163e-18 * 2), LameFloat(-0.00851586 * 2),  LameFloat(-6.74764e-18 * 2), LameFloat(0.0209036 * 2),    LameFloat(-3.36639e-17 * 2),
           LameFloat(-0.0438162 * 2),   LameFloat(-1.54175e-17 * 2), LameFloat(0.0931738 * 2),    LameFloat(-5.52212e-17 * 2), LameFloat(-0.313819 * 2)};

    // vbrpsy_attack_detection, the part without state: high pass of fs/4 and the peak of each of
    // the 9 sub-short blocks, for L, R and, from them, M and S
    void AttackPrepScalar(const ChannelSamples& buffer, std::int32_t nChnOut, std::int32_t nChnPsy, SubShortPeaks& subShort) noexcept
    {
        std::array<std::array<float, 576>, 2> nsHpfsmpl;
        std::memset(&nsHpfsmpl, 0, sizeof(nsHpfsmpl));
        for (std::int32_t chn = 0; chn < nChnOut; chn++) {
            // apply high pass filter of fs/4
            const sample_t* const firbuf = &buffer[chn][576 - 350 - NSFIRLEN + 192];
            for (std::int32_t i = 0; i < 576; i++) {
                float sum1 = firbuf[i + 10];
                float sum2 = 0.0f;
                for (std::int32_t j = 0; j < ((NSFIRLEN - 1) / 2) - 1; j += 2) {
                    sum1 += AttackFirCoef[j] * (firbuf[i + j] + firbuf[i + NSFIRLEN - j]);
                    sum2 += AttackFirCoef[j + 1] * (firbuf[i + j + 1] + firbuf[i + NSFIRLEN - j - 1]);
                }
                nsHpfsmpl[chn][i] = sum1 + sum2;
            }
        }

        for (std::int32_t chn = 0; chn < nChnPsy; chn++) {
            const float* pf = nsHpfsmpl[chn & 1].data();
            if (chn == 2) {
                for (std::int32_t i = 0, j = 576; j > 0; ++i, --j) {
                    const float l   = nsHpfsmpl[0][i];
                    const float r   = nsHpfsmpl[1][i];
                    nsHpfsmpl[0][i] = l + r;
                    nsHpfsmpl[1][i] = l - r;
                }
            }
            for (std::int32_t i = 0; i < 9; i++) {
                const float* const pfe = pf + 576 / 9;
                float              p   = 1.0f;
                for (; pf < pfe; pf++)
                    if (p < std::fabs(*pf))
                        p = std::fabs(*pf);
                subShort[chn][i] = p;
            }
        }
    }

    AttackPrepFunction AttackPrepFor(SimdLevel level) noexcept
    {
        switch (level) {
            case SimdLevel::Sse41: return AttackPrepSse41;
            case SimdLevel::Avx2: return AttackPrepAvx2;
            default: return AttackPrepScalar;
        }
    }

    void PsyPrepare(const SessionConfig& cfg, const PsyConstants& gd, const AthTables& ath, const ChannelSamples& buffer, PsyPrep& prep) noexcept
    {
        std::array<std::array<float, HBLKSIZE>, 4> fftenergy;
        std::array<std::array<float, BLKSIZE>, 2>  wsampL;

        const std::int32_t nChnPsy = PsyDetail::PsyChannels(cfg);

        gd.attackPrep(buffer, cfg.channels_out, nChnPsy, prep.subShort);

        // long blocks: the energies of all channels, then their sums, then the spreading
        for (std::int32_t chn = 0; chn < nChnPsy; chn++)
            ComputeFftLong(gd, buffer, chn, fftenergy[chn], wsampL.data() + (chn & 0x01));
        switch (nChnPsy) { // no loudness for mid/side
            case 4: EnergySums<4>(fftenergy, ath.eql_w.data(), prep.totEnergy, prep.loudness); break;
            case 2: EnergySums<2>(fftenergy, ath.eql_w.data(), prep.totEnergy, prep.loudness); break;
            default: EnergySums<1>(fftenergy, ath.eql_w.data(), prep.totEnergy, prep.loudness); break;
        }
        for (std::int32_t chn = 0; chn < nChnPsy; chn++)
            SpreadLong(gd.l, gd.maskChains, fftenergy[chn].data(), prep.l[chn]);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
