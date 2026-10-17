// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Psy.h"

#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "PsyMath.h"
#include "SessionConfig.h"

// Port of psymodel.c (the VBR psychoacoustic model, L3psycho_anal_vbr, and psymodel_init) and of
// adjust_ATH (encoder.c). Every expression keeps LAME's precision: float (float) where LAME
// computes in float, double where one of its double constants or math functions turns the
// arithmetic into double before the result is rounded.

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    namespace {

        constexpr double       LN_TO_LOG10            = 0.2302585093; // LAME's value without M_LN10
        constexpr double       DELBARK                = .34;          // size of each partition band, in barks
        constexpr double       VO_SCALE               = 1. / (14752 * 14752) / (BLKSIZE / 2);
        constexpr double       TemporalMaskSustainSec = 0.01;
        constexpr double       NS_PREECHO_ATT0        = 0.8;
        constexpr double       NS_PREECHO_ATT1        = 0.6;
        constexpr double       NS_PREECHO_ATT2        = 0.3;
        constexpr double       NS_MSFIX               = 3.5;
        constexpr double       NSATTACKTHRE           = 4.4;
        constexpr std::int32_t NSATTACKTHRE_S         = 25;
        constexpr std::int32_t rpelev                 = 2;
        constexpr std::int32_t rpelev2                = 16;

        constexpr std::int32_t LastTabEntry = static_cast<std::int32_t>(sizeof(MaskTab) / sizeof(MaskTab[0])) - 1;

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
            float const ratio = m2 > m1 ? m2 / m1 : m1 / m2;
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

        // convert_partition2scalefac: partition bands to scalefactor bands; the band at the
        // transition from sfb to sfb+1 is split by bo_weight
        void PartitionsToScalefactorBands(const PsyPartitions& gd, const float* eb, const float* thr, float* ennOut, float* thmOut) noexcept
        {
            float              enn  = 0.0f;
            float              thmm = 0.0f;
            std::int32_t       sb   = 0;
            std::int32_t       b    = 0;
            const std::int32_t n    = gd.n_sb;
            for (; sb < n; ++b, ++sb) {
                const std::int32_t boSb  = gd.bo[sb];
                const std::int32_t npart = gd.npart;
                const std::int32_t bLim  = boSb < npart ? boSb : npart;
                while (b < bLim) {
                    enn += eb[b];
                    thmm += thr[b];
                    b++;
                }
                if (b >= npart) {
                    ennOut[sb] = enn;
                    thmOut[sb] = thmm;
                    ++sb;
                    break;
                }
                // at transition sfb -> sfb+1
                float const wCurr = gd.bo_weight[sb];
                float const wNext = 1.0f - wCurr;
                enn += wCurr * eb[b];
                thmm += wCurr * thr[b];
                ennOut[sb] = enn;
                thmOut[sb] = thmm;
                enn        = wNext * eb[b];
                thmm       = wNext * thr[b];
            }
            for (; sb < n; ++sb) {
                ennOut[sb] = 0;
                thmOut[sb] = 0;
            }
        }

        void PartitionsToShortBands(PsyState& psv, const PsyPartitions& gds, const float* eb, const float* thr, std::int32_t chn, std::int32_t sblock) noexcept
        {
            std::array<float, SBMAX_s> enn, thm;
            PartitionsToScalefactorBands(gds, eb, thr, enn.data(), thm.data());
            for (std::int32_t sb = 0; sb < SBMAX_s; ++sb) {
                psv.en[chn].s[sb][sblock]  = enn[sb];
                psv.thm[chn].s[sb][sblock] = thm[sb];
            }
        }

        void PartitionsToLongBands(PsyState& psv, const PsyPartitions& gdl, const float* eb, const float* thr, std::int32_t chn) noexcept
        {
            PartitionsToScalefactorBands(gdl, eb, thr, psv.en[chn].l.data(), psv.thm[chn].l.data());
        }

        // the long block result also serves the short blocks, until they are computed
        void LongPartitionsToShortBands(PsyState& psv, const PsyPartitions& gdLongToShort, const float* eb, const float* thr, std::int32_t chn) noexcept
        {
            std::array<float, SBMAX_s> enn, thm;
            PartitionsToScalefactorBands(gdLongToShort, eb, thr, enn.data(), thm.data());
            for (std::int32_t sb = 0; sb < SBMAX_s; ++sb) {
                float const scale  = static_cast<float>(1. / 64.f);
                float const tmpEnn = enn[sb];
                float const tmpThm = thm[sb] * scale;
                for (std::int32_t sblock = 0; sblock < 3; ++sblock) {
                    psv.en[chn].s[sb][sblock]  = tmpEnn;
                    psv.thm[chn].s[sb][sblock] = tmpThm;
                }
            }
        }

        // NS_INTERP: was pow((x),(r))*pow((y),1-(r))
        float NsInterp(float x, float y, float r) noexcept
        {
            if (r >= 1.0f)
                return x; // 99.7% of the time
            if (r <= 0.0f)
                return y;
            if (y > 0.0f)
                return ::powf(x / y, r) * y; // rest of the time
            return 0.0f;                     // never happens
        }

        float PerceptualEntropyShort(const PsyRatio& mr, float maskingLower) noexcept
        {
            // these values are tuned only for 44.1kHz...
            // constant table taken over from LAME (psymodel.c) as data
            static const auto regcoef_s = std::to_array<float>(
                {static_cast<float>(11.8), static_cast<float>(13.6), static_cast<float>(17.2), 32.0f, static_cast<float>(46.5), static_cast<float>(51.3),
                 static_cast<float>(57.5), static_cast<float>(67.1), static_cast<float>(71.5), static_cast<float>(84.6), static_cast<float>(97.6), 130.0f});

            float peS = 1236.28f / 4;
            for (std::int32_t sb = 0; sb < SBMAX_s - 1; sb++) {
                for (std::int32_t sblock = 0; sblock < 3; sblock++) {
                    float const thm = mr.thm.s[sb][sblock];
                    if (thm > 0.0f) {
                        float const x  = thm * maskingLower;
                        float const en = mr.en.s[sb][sblock];
                        if (en > x) {
                            if (en > x * 1e10f)
                                peS = static_cast<float>(peS + regcoef_s[sb] * (10.0f * LOG10));
                            else
                                peS = static_cast<float>(peS + regcoef_s[sb] * FastLog10(en / x));
                        }
                    }
                }
            }
            return peS;
        }

        float PerceptualEntropyLong(const PsyRatio& mr, float maskingLower) noexcept
        {
            // these values are tuned only for 44.1kHz...
            // constant table taken over from LAME (psymodel.c) as data
            static const auto regcoef_l = std::to_array<float>({static_cast<float>(6.8),  static_cast<float>(5.8),  static_cast<float>(5.8),
                                                                static_cast<float>(6.4),  static_cast<float>(6.5),  static_cast<float>(9.9),
                                                                static_cast<float>(12.1), static_cast<float>(14.4), 15.0f,
                                                                static_cast<float>(18.9), static_cast<float>(21.6), static_cast<float>(26.9),
                                                                static_cast<float>(34.2), static_cast<float>(40.2), static_cast<float>(46.8),
                                                                static_cast<float>(56.5), static_cast<float>(60.7), static_cast<float>(73.9),
                                                                static_cast<float>(85.7), static_cast<float>(93.4), static_cast<float>(126.1)});

            float peL = 1124.23f / 4;
            for (std::int32_t sb = 0; sb < SBMAX_l - 1; sb++) {
                float const thm = mr.thm.l[sb];
                if (thm > 0.0f) {
                    float const x  = thm * maskingLower;
                    float const en = mr.en.l[sb];
                    if (en > x) {
                        if (en > x * 1e10f)
                            peL = static_cast<float>(peL + regcoef_l[sb] * (10.0f * LOG10));
                        else
                            peL = static_cast<float>(peL + regcoef_l[sb] * FastLog10(en / x));
                    }
                }
            }
            return peL;
        }

        // calc_energy: energy, maximum and average of each partition band
        void PartitionEnergy(const PsyPartitions& l, const float* fftenergy, float* eb, float* max, float* avg) noexcept
        {
            for (std::int32_t b = 0, j = 0; b < l.npart; ++b) {
                float ebb = 0;
                float m   = 0;
                for (std::int32_t i = 0; i < l.numlines[b]; ++i, ++j) {
                    float const el = fftenergy[j];
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
                float const sqrt2Half = static_cast<float>(SQRT2 * 0.5f);
                // FFT data for mid and side channel is derived from L & R
                for (std::int32_t j = BLKSIZE - 1; j >= 0; --j) {
                    float const l = wsampL[0][j];
                    float const r = wsampL[1][j];
                    wsampL[0][j]  = (l + r) * sqrt2Half;
                    wsampL[1][j]  = (l - r) * sqrt2Half;
                }
            }

            fftenergy[0] = wsampL[0][0];
            fftenergy[0] *= fftenergy[0];
            for (std::int32_t j = BLKSIZE / 2 - 1; j >= 0; --j) {
                float const re             = (*wsampL)[BLKSIZE / 2 - j];
                float const im             = (*wsampL)[BLKSIZE / 2 + j];
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

        // vbrpsy_compute_fft_s
        void ComputeFftShort(const PsyConstants& gd, const ChannelSamples& buffer, std::int32_t chn, std::int32_t sblock,
                             std::array<std::array<float, HBLKSIZE_s>, 3>& fftenergyS, std::array<std::array<float, BLKSIZE_s>, 3>* wsampS) noexcept
        {
            if (sblock == 0 && chn < 2)
                FftShort(gd.fft, *wsampS, buffer[chn]);
            if (chn == 2) {
                float const sqrt2Half = static_cast<float>(SQRT2 * 0.5f);
                // FFT data for mid and side channel is derived from L & R
                for (std::int32_t j = BLKSIZE_s - 1; j >= 0; --j) {
                    float const l        = wsampS[0][sblock][j];
                    float const r        = wsampS[1][sblock][j];
                    wsampS[0][sblock][j] = (l + r) * sqrt2Half;
                    wsampS[1][sblock][j] = (l - r) * sqrt2Half;
                }
            }

            fftenergyS[sblock][0] = (*wsampS)[sblock][0];
            fftenergyS[sblock][0] *= fftenergyS[sblock][0];
            for (std::int32_t j = BLKSIZE_s / 2 - 1; j >= 0; --j) {
                float const re                        = (*wsampS)[sblock][BLKSIZE_s / 2 - j];
                float const im                        = (*wsampS)[sblock][BLKSIZE_s / 2 + j];
                fftenergyS[sblock][BLKSIZE_s / 2 - j] = (re * re + im * im) * 0.5f;
            }
        }

        // vbrpsy_attack_detection, the part with state: attacks from the sub-short block peaks
        // against those of the last granule; also hands out the last call's masking and energy
        void AttackResolve(const SessionConfig& cfg, const PsyConstants& gd, PsyState& psv, const SubShortPeaks& subShort, std::int32_t grOut,
                           GranuleChannel<PsyRatio>& maskingRatio, GranuleChannel<PsyRatio>& maskingMsRatio, std::array<float, 4>& energy,
                           std::array<std::array<float, 3>, 4>& subShortFactor, std::array<std::array<std::int32_t, 4>, 4>& nsAttacks,
                           std::array<std::int32_t, 2>& useLongBlock) noexcept
        {
            const std::int32_t nChnOut = cfg.channels_out;
            const std::int32_t nChnPsy = (cfg.mode == JointStereo) ? 4 : nChnOut; // chn 2 and 3 = mid and side

            for (std::int32_t chn = 0; chn < nChnOut; chn++) {
                // the maskings of the last call, returned with one granule delay
                maskingRatio[grOut][chn].en  = psv.en[chn];
                maskingRatio[grOut][chn].thm = psv.thm[chn];
                if (nChnPsy > 2) {
                    maskingMsRatio[grOut][chn].en  = psv.en[chn + 2];
                    maskingMsRatio[grOut][chn].thm = psv.thm[chn + 2];
                }
            }

            for (std::int32_t chn = 0; chn < nChnPsy; chn++) {
                std::array<float, 12> attackIntensity;
                std::array<float, 12> enSubshort;
                std::array<float, 4>  enShort        = {0, 0, 0, 0};
                std::int32_t nsUseLongBlock = 1;

                // energies of the sub-short blocks
                for (std::int32_t i = 0; i < 3; i++) {
                    enSubshort[i]      = psv.last_en_subshort[chn][i + 6];
                    attackIntensity[i] = enSubshort[i] / psv.last_en_subshort[chn][i + 4];
                    enShort[0] += enSubshort[i];
                }
                for (std::int32_t i = 0; i < 9; i++) {
                    float p                      = subShort[chn][i];
                    psv.last_en_subshort[chn][i] = enSubshort[i + 3] = p;
                    enShort[1 + i / 3] += p;
                    if (p > enSubshort[i + 3 - 2])
                        p = p / enSubshort[i + 3 - 2];
                    else if (enSubshort[i + 3 - 2] > p * 10.0f)
                        p = enSubshort[i + 3 - 2] / (p * 10.0f);
                    else
                        p = 0.0f;
                    attackIntensity[i + 3] = p;
                }
                // pulse like signal detection for fatboy.wav and so on
                for (std::int32_t i = 0; i < 3; ++i) {
                    float const enn    = enSubshort[i * 3 + 3] + enSubshort[i * 3 + 4] + enSubshort[i * 3 + 5];
                    float       factor = 1.f;
                    if (enSubshort[i * 3 + 5] * 6 < enn) {
                        factor *= 0.5f;
                        if (enSubshort[i * 3 + 4] * 6 < enn)
                            factor *= 0.5f;
                    }
                    subShortFactor[chn][i] = factor;
                }

                // compare energies between sub-shortblocks
                {
                    float const x = gd.attack_threshold[chn];
                    for (std::int32_t i = 0; i < 12; i++) {
                        if (nsAttacks[chn][i / 3] == 0 && attackIntensity[i] > x)
                            nsAttacks[chn][i / 3] = (i % 3) + 1;
                    }
                }
                // there should be an energy change between short blocks, to avoid periodic signals
                for (std::int32_t i = 1; i < 4; i++) {
                    float const u = enShort[i - 1];
                    float const v = enShort[i];
                    float const m = Max(u, v);
                    if (m < 40000) {
                        if (u < 1.7f * v && v < 1.7f * u) {
                            if (i == 1 && nsAttacks[chn][0] <= nsAttacks[chn][i])
                                nsAttacks[chn][0] = 0;
                            nsAttacks[chn][i] = 0;
                        }
                    }
                }

                if (nsAttacks[chn][0] <= psv.last_attacks[chn])
                    nsAttacks[chn][0] = 0;

                if (psv.last_attacks[chn] == 3 || nsAttacks[chn][0] + nsAttacks[chn][1] + nsAttacks[chn][2] + nsAttacks[chn][3]) {
                    nsUseLongBlock = 0;
                    if (nsAttacks[chn][1] && nsAttacks[chn][0])
                        nsAttacks[chn][1] = 0;
                    if (nsAttacks[chn][2] && nsAttacks[chn][1])
                        nsAttacks[chn][2] = 0;
                    if (nsAttacks[chn][3] && nsAttacks[chn][2])
                        nsAttacks[chn][3] = 0;
                }

                if (chn < 2)
                    useLongBlock[chn] = nsUseLongBlock;
                else if (nsUseLongBlock == 0)
                    useLongBlock[0] = useLongBlock[1] = 0;

                // the energy of the last call, returned with one granule delay
                energy[chn] = psv.tot_ener[chn];
            }
        }

        // vbrpsy_skip_masking_s
        void SkipMaskingShort(PsyState& psv, const PsyPartitions& gds, std::int32_t chn, std::int32_t sblock) noexcept
        {
            if (sblock == 0) {
                for (std::int32_t b = 0; b < gds.npart; b++)
                    psv.nb_s2[chn][b] = psv.nb_s1[chn][b];
            }
        }

        // vbrpsy_compute_masking_s, the part without state: energy, tonality and the spreading
        // of one short block; limit is the threshold limit for strongly tonal signals
        void SpreadShort(const PsyPartitions& gds, MaskChainsFunction maskChains, const float* fftenergyS, PsyBandPrep& out) noexcept
        {
            std::array<float, CBANDS>        max, avg;
            std::array<std::uint8_t, CBANDS> maskIdxS;
            std::int32_t b, j;

            max.fill(0);
            avg.fill(0);
            for (b = j = 0; b < gds.npart; ++b) {
                float              ebb = 0;
                float              m   = 0;
                const std::int32_t n   = gds.numlines[b];
                for (std::int32_t i = 0; i < n; ++i, ++j) {
                    float const el = fftenergyS[j];
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
                float const avgMask    = MaskTab[dd] * 0.5f;
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

        // vbrpsy_compute_masking_s, the part with state
        void MaskingShortResolve(PsyState& psv, const PsyPartitions& gds, float maskingLowerGlobal, const PsyBandPrep& in, float* eb, float* thr,
                                 std::int32_t chn) noexcept
        {
            std::memcpy(eb, in.eb.data(), sizeof(in.eb));
            std::int32_t b;
            for (b = 0; b < gds.npart; b++) {
                float const maskingLower = gds.masking_lower[b] * maskingLowerGlobal;
                float const ecb          = in.ecb[b];
                thr[b]                   = ecb; // pre-echo control follows later

                psv.nb_s2[chn][b] = psv.nb_s1[chn][b];
                psv.nb_s1[chn][b] = ecb;
                if (thr[b] > in.limit[b])
                    thr[b] = in.limit[b];
                if (maskingLower > 1)
                    thr[b] *= maskingLower;
                if (thr[b] > eb[b])
                    thr[b] = eb[b];
                if (maskingLower < 1)
                    thr[b] *= maskingLower;
            }
            for (; b < CBANDS; ++b)
                thr[b] = 0;
        }

        // vbrpsy_compute_masking_l, the part without state: energy and tonality of each
        // partition and the spreading; limit is the threshold limit for strongly tonal signals
        void SpreadLong(const PsyPartitions& gdl, MaskChainsFunction maskChains, const float* fftenergy, PsyBandPrep& out) noexcept
        {
            std::array<float, CBANDS>            max, avg;
            std::array<std::uint8_t, CBANDS + 2> maskIdxL;
            std::int32_t b;

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
                float const avgMask    = MaskTab[dd] * 0.5f;
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

        // vbrpsy_compute_masking_l, the part with state: long block pre-echo control against
        // the spread energies of the last granules
        void MaskingLongResolve(PsyState& psv, const PsyPartitions& gdl, MaskingLongFunction maskingLong, float maskingLowerGlobal, const PsyBandPrep& in,
                                float* ebL, float* thr, std::int32_t chn) noexcept
        {
            std::memcpy(ebL, in.eb.data(), sizeof(in.eb));
            const MaskingLongArgs args{
                gdl.masking_lower.data(), maskingLowerGlobal,    gdl.npart, psv.blocktype_old[chn & 0x01], in.ecb.data(), in.limit.data(), in.eb.data(),
                psv.nb_l1[chn].data(),    psv.nb_l2[chn].data(), thr};
            maskingLong(args);
            for (std::int32_t b = gdl.npart; b < CBANDS; ++b)
                thr[b] = 0;
        }

        // vbrpsy_compute_block_type
        void ComputeBlockType(const SessionConfig& cfg, std::int32_t* useLongBlock) noexcept
        {
            // force both channels to use the same block type (needed for ms_stereo; FhG does it always)
            if (cfg.short_blocks == ShortBlockCoupled && !(useLongBlock[0] && useLongBlock[1]))
                useLongBlock[0] = useLongBlock[1] = 0;

            for (std::int32_t chn = 0; chn < cfg.channels_out; chn++) {
                if (cfg.short_blocks == ShortBlockDispensed)
                    useLongBlock[chn] = 1;
                if (cfg.short_blocks == ShortBlockForced)
                    useLongBlock[chn] = 0;
            }
        }

        // vbrpsy_apply_block_type: the block type of the previous granule depends on this one
        void ApplyBlockType(PsyState& psv, std::int32_t nch, const std::int32_t* useLongBlock, std::int32_t* blocktypeD) noexcept
        {
            for (std::int32_t chn = 0; chn < nch; chn++) {
                std::int32_t blocktype = NORM_TYPE;
                if (useLongBlock[chn]) {
                    // no attack: use long blocks
                    if (psv.blocktype_old[chn] == SHORT_TYPE)
                        blocktype = STOP_TYPE;
                } else {
                    // attack: use short blocks
                    blocktype = SHORT_TYPE;
                    if (psv.blocktype_old[chn] == NORM_TYPE)
                        psv.blocktype_old[chn] = START_TYPE;
                    if (psv.blocktype_old[chn] == STOP_TYPE)
                        psv.blocktype_old[chn] = SHORT_TYPE;
                }
                blocktypeD[chn]        = psv.blocktype_old[chn]; // value returned to calling program
                psv.blocktype_old[chn] = blocktype;              // save for next call
            }
        }

        // vbrpsy_compute_MS_thresholds: M/S thresholds (Johnston & Ferreira 1992 ICASSP)
        void ComputeMsThresholds(const std::array<std::array<float, CBANDS>, 4>& eb, std::array<std::array<float, CBANDS>, 4>& thr,
                                 const std::array<float, CBANDS>& cbMld, const std::array<float, CBANDS>& athCb, float athLower, float msfix,
                                 std::int32_t n) noexcept
        {
            float const msfix2 = msfix * 2.f;
            for (std::int32_t b = 0; b < n; ++b) {
                float const ebM  = eb[2][b];
                float const ebS  = eb[3][b];
                float const thmL = thr[0][b];
                float const thmR = thr[1][b];
                float       thmM = thr[2][b];
                float       thmS = thr[3][b];
                float       rmid, rside;

                // use this fix if L & R masking differs by 2db or less
                if (thmL <= 1.58f * thmR && thmR <= 1.58f * thmL) {
                    float const mldM = cbMld[b] * ebS;
                    float const mldS = cbMld[b] * ebM;
                    float const tmpM = Min(thmS, mldM);
                    float const tmpS = Min(thmM, mldS);
                    rmid             = Max(thmM, tmpM);
                    rside            = Max(thmS, tmpS);
                } else {
                    rmid  = thmM;
                    rside = thmS;
                }
                if (msfix > 0.f) {
                    // adjust M/S maskings if the user set "msfix" (Naoki Shibata 2000)
                    float const ath   = athCb[b] * athLower;
                    float const tmpL  = Max(thmL, ath);
                    float const tmpR  = Max(thmR, ath);
                    float const thmLR = Min(tmpL, tmpR);
                    thmM              = Max(rmid, ath);
                    thmS              = Max(rside, ath);
                    float const thmMS = thmM + thmS;
                    if (thmMS > 0.f && (thmLR * msfix2) < thmMS) {
                        float const f = thmLR * msfix2 / thmMS;
                        thmM *= f;
                        thmS *= f;
                    }
                    rmid  = Min(thmM, rmid);
                    rside = Min(thmS, rside);
                }
                if (rmid > ebM)
                    rmid = ebM;
                if (rside > ebS)
                    rside = ebS;
                thr[2][b] = rmid;
                thr[3][b] = rside;
            }
        }

        // --- psymodel_init ---------------------------------------------------------------

        // s3_func: the spreading function, in units of energy
        float SpreadingFunction(float bark) noexcept
        {
            float tempx, x, tempy;
            tempx = bark;
            if (tempx >= 0)
                tempx *= 3;
            else
                tempx = static_cast<float>(tempx * 1.5);

            if (tempx >= 0.5 && tempx <= 2.5) {
                float const temp = static_cast<float>(tempx - 0.5);
                x                = static_cast<float>(8.0 * (temp * temp - 2.0 * temp));
            } else {
                x = 0.0f;
            }
            tempx = static_cast<float>(tempx + 0.474);
            tempy = static_cast<float>(15.811389 + 7.5 * tempx - 17.5 * std::sqrt(1.0 + tempx * tempx));

            if (tempy <= -60.0)
                return 0.0f;

            tempx = static_cast<float>(std::exp((x + tempy) * LN_TO_LOG10));

            // normalization, so that the integral over all barks is 1
            tempx = static_cast<float>(tempx / .6609193);
            return tempx;
        }

        // stereo_demask: stereo demasking thresholds, formula reverse engineered from a plot
        float StereoDemask(double f) noexcept
        {
            double arg = FreqToBark(static_cast<float>(f));
            arg        = (Min(arg, 15.5) / 15.5);
            return static_cast<float>(std::pow(10.0, 1.25 * (1 - std::cos(PI * arg)) - 2.5));
        }

        // init_numline: partition bands of about DELBARK and their relation to scalefactor bands
        void InitNumline(PsyPartitions& gd, float sfreq, std::int32_t fftSize, std::int32_t mdctSize, std::int32_t sbmax, const std::int32_t* scalepos) noexcept
        {
            std::array<float, CBANDS + 1>      bFrq;
            float const  mdctFreqFrac        = sfreq / (2.0f * mdctSize);
            float const  deltafreq           = fftSize / (2.0f * mdctSize);
            std::array<std::int32_t, HBLKSIZE> partition           = {0};
            std::int32_t i, j, ni;

            sfreq /= fftSize;
            j  = 0;
            ni = 0;
            // numlines, the number of spectral lines in each partition band
            for (i = 0; i < CBANDS; i++) {
                float const bark1 = FreqToBark(sfreq * j);
                bFrq[i]           = sfreq * j;

                std::int32_t j2;
                for (j2 = j; FreqToBark(sfreq * j2) - bark1 < DELBARK && j2 <= fftSize / 2; j2++) {
                }

                const std::int32_t nl = j2 - j;
                gd.numlines[i]        = nl;
                gd.rnumlines[i]       = (nl > 0) ? (1.0f / nl) : 0;

                ni = i + 1;

                while (j < j2)
                    partition[j++] = i;
                if (j > fftSize / 2) {
                    j = fftSize / 2;
                    ++i;
                    break;
                }
            }
            bFrq[i] = sfreq * j;

            gd.n_sb  = sbmax;
            gd.npart = ni;

            j = 0;
            for (i = 0; i < gd.npart; i++) {
                const std::int32_t nl   = gd.numlines[i];
                float const        freq = sfreq * (j + nl / 2);
                gd.mld_cb[i]            = StereoDemask(freq);
                j += nl;
            }
            for (; i < CBANDS; ++i)
                gd.mld_cb[i] = 1;

            for (std::int32_t sfb = 0; sfb < sbmax; sfb++) {
                const std::int32_t start = scalepos[sfb];
                const std::int32_t end   = scalepos[sfb + 1];

                std::int32_t i1 = static_cast<std::int32_t>(std::floor(.5 + deltafreq * (start - .5)));
                if (i1 < 0)
                    i1 = 0;
                std::int32_t i2 = static_cast<std::int32_t>(std::floor(.5 + deltafreq * (end - .5)));
                if (i2 > fftSize / 2)
                    i2 = fftSize / 2;

                const std::int32_t bo = partition[i2];
                gd.bm[sfb]            = (partition[i1] + partition[i2]) / 2;
                gd.bo[sfb]            = bo;

                // how much of this band belongs to the current scalefactor band
                float const fTmp = mdctFreqFrac * end;
                float       boW  = (fTmp - bFrq[bo]) / (bFrq[bo + 1] - bFrq[bo]);
                if (boW < 0)
                    boW = 0;
                else if (boW > 1)
                    boW = 1;
                gd.bo_weight[sfb] = boW;
                gd.mld[sfb]       = StereoDemask(mdctFreqFrac * start);
            }
        }

        // compute_bark_values: bark value and width of each partition band
        void ComputeBarkValues(const PsyPartitions& gd, float sfreq, std::int32_t fftSize, float* bval, float* bvalWidth) noexcept
        {
            std::int32_t j = 0;
            sfreq /= fftSize;
            for (std::int32_t k = 0; k < gd.npart; k++) {
                const std::int32_t w = gd.numlines[k];

                float bark1 = FreqToBark(sfreq * (j));
                float bark2 = FreqToBark(sfreq * (j + w - 1));
                bval[k]     = static_cast<float>(.5 * (bark1 + bark2));

                bark1        = FreqToBark(static_cast<float>(sfreq * (j - .5)));
                bark2        = FreqToBark(static_cast<float>(sfreq * (j + w - .5)));
                bvalWidth[k] = bark2 - bark1;
                j += w;
            }
        }

        // init_s3_values: the spreading function between all partition bands, stored sparse
        void InitS3Values(PsyPartitions& gd, const float* bval, const float* bvalWidth, const float* norm) noexcept
        {
            std::array<std::array<float, CBANDS>, CBANDS> s3;
            const std::int32_t npart = gd.npart;

            std::memset(&s3, 0, sizeof(s3));

            // s3[i][j]: the spreading function centered at band j (masker), for band i (maskee)
            for (std::int32_t i = 0; i < npart; i++) {
                for (std::int32_t j = 0; j < npart; j++) {
                    float const v = SpreadingFunction(bval[i] - bval[j]) * bvalWidth[j];
                    s3[i][j]      = v * norm[i];
                }
            }
            for (std::int32_t i = 0; i < npart; i++) {
                std::int32_t j;
                for (j = 0; j < npart; j++) {
                    if (s3[i][j] > 0.0f)
                        break;
                }
                gd.s3ind[i][0] = j;

                for (j = npart - 1; j > 0; j--) {
                    if (s3[i][j] > 0.0f)
                        break;
                }
                gd.s3ind[i][1] = j;
            }

            std::int32_t k = 0;
            for (std::int32_t i = 0; i < npart; i++)
                for (std::int32_t j = gd.s3ind[i][0]; j <= gd.s3ind[i][1]; j++)
                    gd.s3[k++] = s3[i][j];
        }

    } // namespace

    // constant tables taken over from LAME (psymodel.c) as data
    const float MaskAddMaxI1      = static_cast<float>(3.6517412725483771); // pow(10, (I1LIMIT + 1) / 16.0)
    const float MaskAddMaxI2      = static_cast<float>(31.622776601683793); // pow(10, (I2LIMIT + 1) / 16.0)
    const std::array<float, 10> MaskAddTable2     = {static_cast<float>(1.33352 * 1.33352), static_cast<float>(1.35879 * 1.35879),
                                                     static_cast<float>(1.38454 * 1.38454), static_cast<float>(1.39497 * 1.39497),
                                                     static_cast<float>(1.40548 * 1.40548), static_cast<float>(1.3537 * 1.3537),
                                                     static_cast<float>(1.30382 * 1.30382), static_cast<float>(1.22321 * 1.22321),
                                                     static_cast<float>(1.14758 * 1.14758), 1.0f};

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
                                          static_cast<float>(0.79433),
                                          static_cast<float>(0.63096),
                                          static_cast<float>(0.63096),
                                          static_cast<float>(0.63096),
                                          static_cast<float>(0.63096),
                                          static_cast<float>(0.63096),
                                          static_cast<float>(0.25119),
                                          static_cast<float>(0.11749)};

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
                float const x = s3[k] * eb[kk] * MaskTab[maskIdx[kk]];
                ecb           = MaskAdd(ecb, x, kk - b, delta);
                ++k, ++kk;
            }
            ecbOut[b] = ecb;
        }
    }

    void MaskingLongScalar(const MaskingLongArgs& args) noexcept
    {
        for (std::int32_t b = 0; b < args.npart; b++)
            MaskingLongOne(args, b);
    }

    MaskingLongFunction MaskingLongFor(SimdLevel level) noexcept
    {
        switch (level) {
            case SimdLevel::Sse41: return MaskingLongSse41;
            case SimdLevel::Avx2: return MaskingLongAvx2;
            default: return MaskingLongScalar;
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
        = {static_cast<float>(-8.65163e-18 * 2), static_cast<float>(-0.00851586 * 2), static_cast<float>(-6.74764e-18 * 2), static_cast<float>(0.0209036 * 2),
           static_cast<float>(-3.36639e-17 * 2), static_cast<float>(-0.0438162 * 2),  static_cast<float>(-1.54175e-17 * 2), static_cast<float>(0.0931738 * 2),
           static_cast<float>(-5.52212e-17 * 2), static_cast<float>(-0.313819 * 2)};

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
                    float const l   = nsHpfsmpl[0][i];
                    float const r   = nsHpfsmpl[1][i];
                    nsHpfsmpl[0][i] = l + r;
                    nsHpfsmpl[1][i] = l - r;
                }
            }
            for (std::int32_t i = 0; i < 9; i++) {
                float const* const pfe = pf + 576 / 9;
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

    std::int32_t PsyInit(SessionConfig& cfg, PsyConstants& gd, PsyState& psv, AthTables& ath, AthAdjust& adjust)
    {
        float const bvlA      = 13;
        float const bvlB      = 24;
        float const snrLA     = 0;
        float const snrLB     = 0;
        float const snrSA     = static_cast<float>(-8.25);
        float const snrSB     = static_cast<float>(-4.5);
        float const xav       = 10;
        float const xbv       = 12;
        float const sfreq     = static_cast<float>(cfg.samplerate_out);
        float const minvalLow = (0.f - cfg.minval);

        std::array<float, CBANDS> bval;
        std::array<float, CBANDS> bvalWidth;
        std::array<float, CBANDS> norm;
        std::int32_t i, j, b, k;

        std::memset(&gd, 0, sizeof(gd));
        std::memset(&psv, 0, sizeof(psv));
        norm.fill(0);

        gd.force_short_block_calc = cfg.options.experimentalZ;

        // set by lame_init_params before psymodel_init runs
        ath.use_adjust       = cfg.ath_use_adjust;
        ath.aa_sensitivity_p = cfg.ath_aa_sensitivity_p;

        psv.blocktype_old[0] = psv.blocktype_old[1] = NORM_TYPE; // the vbr header is long blocks

        for (i = 0; i < 4; ++i) {
            for (j = 0; j < CBANDS; ++j) {
                psv.nb_l1[i][j] = 1e20f;
                psv.nb_l2[i][j] = 1e20f;
                psv.nb_s1[i][j] = psv.nb_s2[i][j] = 1.0f;
            }
            for (std::int32_t sb = 0; sb < SBMAX_l; sb++) {
                psv.en[i].l[sb]  = 1e20f;
                psv.thm[i].l[sb] = 1e20f;
            }
            for (j = 0; j < 3; ++j) {
                for (std::int32_t sb = 0; sb < SBMAX_s; sb++) {
                    psv.en[i].s[sb][j]  = 1e20f;
                    psv.thm[i].s[sb][j] = 1e20f;
                }
                psv.last_attacks[i] = 0;
            }
            for (j = 0; j < 9; j++)
                psv.last_en_subshort[i][j] = 10.0f;
        }

        // init. for loudness approx. -jd 2001 mar 27
        psv.loudness_sq_save[0] = psv.loudness_sq_save[1] = 0.0f;

        // long blocks: numlines, bo, bm, bval, bval_width, mld
        InitNumline(gd.l, sfreq, BLKSIZE, 576, SBMAX_l, cfg.scalefac_band.l.data());
        ComputeBarkValues(gd.l, sfreq, BLKSIZE, bval.data(), bvalWidth.data());

        // the spreading function
        for (i = 0; i < gd.l.npart; i++) {
            double snr = snrLA;
            if (bval[i] >= bvlA)
                snr = snrLB * (bval[i] - bvlA) / (bvlB - bvlA) + snrLA * (bvlB - bval[i]) / (bvlB - bvlA);
            norm[i] = static_cast<float>(std::pow(10.0, snr / 10.0));
        }
        InitS3Values(gd.l, bval.data(), bvalWidth.data(), norm.data());

        // long block ATH and MINVAL
        j = 0;
        for (i = 0; i < gd.l.npart; i++) {
            double x = FLT_MAX;
            for (k = 0; k < gd.l.numlines[i]; k++, j++) {
                float const freq  = static_cast<float>(sfreq * j / (1000.0 * BLKSIZE));
                float       level = AthFormula(cfg, freq * 1000) - 20;              // scale to FFT units; returned value is in dB
                level             = static_cast<float>(std::pow(10., 0.1 * level)); // convert from dB -> energy
                level *= gd.l.numlines[i];
                if (x > level)
                    x = level;
            }
            ath.cb_l[i] = static_cast<float>(x);

            // MINVAL: for low frequencies the strength of the masking is limited
            x = 20.0 * (bval[i] / xav - 1.0);
            if (x > 6)
                x = 30;
            if (x < minvalLow)
                x = minvalLow;
            if (cfg.samplerate_out < 44000)
                x = 30;
            x -= 8.;
            gd.l.minval[i] = static_cast<float>(std::pow(10.0, x / 10.) * gd.l.numlines[i]);
        }

        // the same for short blocks
        InitNumline(gd.s, sfreq, BLKSIZE_s, 192, SBMAX_s, cfg.scalefac_band.s.data());
        ComputeBarkValues(gd.s, sfreq, BLKSIZE_s, bval.data(), bvalWidth.data());

        j = 0;
        for (i = 0; i < gd.s.npart; i++) {
            double snr = snrSA;
            if (bval[i] >= bvlA)
                snr = snrSB * (bval[i] - bvlA) / (bvlB - bvlA) + snrSA * (bvlB - bval[i]) / (bvlB - bvlA);
            norm[i] = static_cast<float>(std::pow(10.0, snr / 10.0));

            double x = FLT_MAX;
            for (k = 0; k < gd.s.numlines[i]; k++, j++) {
                float const freq  = static_cast<float>(sfreq * j / (1000.0 * BLKSIZE_s));
                float       level = AthFormula(cfg, freq * 1000) - 20;
                level             = static_cast<float>(std::pow(10., 0.1 * level));
                level *= gd.s.numlines[i];
                if (x > level)
                    x = level;
            }
            ath.cb_s[i] = static_cast<float>(x);

            // MINVAL
            x = 7.0 * (bval[i] / xbv - 1.0);
            if (bval[i] > xbv)
                x *= 1 + std::log(1 + x) * 3.1;
            if (bval[i] < xbv)
                x *= 1 + std::log(1 - x) * 2.3;
            if (x > 6)
                x = 30;
            if (x < minvalLow)
                x = minvalLow;
            if (cfg.samplerate_out < 44000)
                x = 30;
            x -= 8;
            gd.s.minval[i] = static_cast<float>(std::pow(10.0, x / 10) * gd.s.numlines[i]);
        }
        InitS3Values(gd.s, bval.data(), bvalWidth.data(), norm.data());

        gd.fft.Initialize();
        gd.attackPrep  = AttackPrepScalar;
        gd.maskChains  = MaskChainsScalar;
        gd.maskingLong = MaskingLongScalar;

        // temporal masking
        gd.decay = static_cast<float>(std::exp(-1.0 * LOG10 / (TemporalMaskSustainSec * sfreq / 192.0)));

        {
            float msfix = static_cast<float>(NS_MSFIX);
            if (cfg.use_safe_joint_stereo)
                msfix = 1.0f;
            if (std::fabs(static_cast<double>(cfg.msfix)) > 0.0)
                msfix = cfg.msfix;
            cfg.msfix = msfix;

            // spread only from npart_l bands
            for (b = 0; b < gd.l.npart; b++)
                if (gd.l.s3ind[b][1] > gd.l.npart - 1)
                    gd.l.s3ind[b][1] = gd.l.npart - 1;
        }

        // ATH auto adjustment: decrease the ATH by 12 dB per second
        const double frameDuration = 576. * 2 / sfreq;
        ath.decay                  = static_cast<float>(std::pow(10., -12. / 10. * frameDuration));
        adjust.factor              = static_cast<float>(0.01); // minimum, for leading low loudness
        adjust.limit               = 1.0f;                     // on lead, allow adjust up to maximum

        if (cfg.ATHtype != -1) {
            // equal loudness weights (eql_w): ATH in dB to relative power
            float       freq       = 0.0f;
            float const freqInc    = static_cast<float>(cfg.samplerate_out) / static_cast<float>(BLKSIZE);
            float       eqlBalance = 0.0f;
            for (i = 0; i < BLKSIZE / 2; ++i) {
                freq += freqInc;
                ath.eql_w[i] = static_cast<float>(1. / std::pow(10.0, static_cast<double>(AthFormula(cfg, freq) / 10)));
                eqlBalance += ath.eql_w[i];
            }
            eqlBalance = static_cast<float>(1.0 / eqlBalance);
            for (i = BLKSIZE / 2; --i >= 0;)
                ath.eql_w[i] *= eqlBalance;
        }

        // short block attack threshold
        {
            float x = cfg.options.attackThre;
            float y = cfg.options.attackThreS;
            if (x < 0)
                x = static_cast<float>(NSATTACKTHRE);
            if (y < 0)
                y = NSATTACKTHRE_S;
            gd.attack_threshold[0] = gd.attack_threshold[1] = gd.attack_threshold[2] = x;
            gd.attack_threshold[3]                                                   = y;
        }
        {
            // constant table taken over from LAME (psymodel.c) as data
            static const auto sk = std::to_array<float>({static_cast<float>(-7.4), static_cast<float>(-7.4), static_cast<float>(-7.4), static_cast<float>(-9.5),
                                                         static_cast<float>(-7.4), static_cast<float>(-6.1), static_cast<float>(-5.5), static_cast<float>(-4.7),
                                                         static_cast<float>(-4.7), static_cast<float>(-4.7), static_cast<float>(-4.7)});
            float              skS, skL;
            const std::int32_t vbrQ = cfg.options.vbrQ;
            if (vbrQ < 4)
                skL = skS = sk[0];
            else
                skL = skS = sk[vbrQ] + cfg.options.vbrQFrac * (sk[vbrQ] - sk[vbrQ + 1]);

            for (b = 0; b < gd.s.npart; b++) {
                const float m         = static_cast<float>(gd.s.npart - b) / gd.s.npart;
                gd.s.masking_lower[b] = ::powf(10.f, skS * m * 0.1f);
            }
            for (; b < CBANDS; ++b)
                gd.s.masking_lower[b] = 1.f;
            for (b = 0; b < gd.l.npart; b++) {
                const float m         = static_cast<float>(gd.l.npart - b) / gd.l.npart;
                gd.l.masking_lower[b] = ::powf(10.f, skL * m * 0.1f);
            }
            for (; b < CBANDS; ++b)
                gd.l.masking_lower[b] = 1.f;
        }
        gd.l_to_s = gd.l;
        InitNumline(gd.l_to_s, sfreq, BLKSIZE, 192, SBMAX_s, cfg.scalefac_band.s.data());
        return 0;
    }

    void PsyPrepare(const SessionConfig& cfg, const PsyConstants& gd, const AthTables& ath, const ChannelSamples& buffer, PsyPrep& prep) noexcept
    {
        std::array<std::array<float, HBLKSIZE>, 4> fftenergy;
        std::array<std::array<float, BLKSIZE>, 2>  wsampL;

        const std::int32_t nChnPsy = (cfg.mode == JointStereo) ? 4 : cfg.channels_out; // chn 2 and 3 = mid and side

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

    void PsyResolve(const SessionConfig& cfg, const PsyConstants& gd, PsyState& psv, const AthTables& ath, float athAdjustFactor, float maskingLowerGlobal,
                    PsyResult& result, const PsyPrep& prep, const ChannelSamples& buffer, std::int32_t grOut, GranuleChannel<PsyRatio>& maskingRatio,
                    GranuleChannel<PsyRatio>& maskingMsRatio, std::array<float, 2>& percepEntropy, std::array<float, 2>& percepMsEntropy,
                    std::array<float, 4>& energy, std::array<std::int32_t, 2>& blocktypeD) noexcept
    {
        std::array<PsyXmin, 4> lastThm;

        std::array<std::array<float, HBLKSIZE_s>, 3>               fftenergyS;
        std::array<std::array<std::array<float, BLKSIZE_s>, 3>, 2> wsampS;
        std::array<std::array<float, CBANDS>, 4>                   eb, thr;
        std::array<std::array<float, 3>, 4>                        subShortFactor;

        float const pcfact    = 0.6f;
        float const athFactor = (cfg.msfix > 0.f) ? (cfg.ATH_offset_factor * athAdjustFactor) : 1.f;

        std::array<std::array<std::int32_t, 4>, 4> nsAttacks = {};
        std::array<std::int32_t, 2>                useLongBlock;

        const std::int32_t nChnPsy = (cfg.mode == JointStereo) ? 4 : cfg.channels_out; // chn 2 and 3 = mid and side

        lastThm = psv.thm;

        AttackResolve(cfg, gd, psv, prep.subShort, grOut, maskingRatio, maskingMsRatio, energy, subShortFactor, nsAttacks, useLongBlock);

        ComputeBlockType(cfg, useLongBlock.data());

        // long blocks
        for (std::int32_t chn = 0; chn < nChnPsy; chn++) {
            if (chn < 2) { // no loudness for mid/side
                result.loudness_sq[grOut][chn] = psv.loudness_sq_save[chn];
                psv.loudness_sq_save[chn]      = prep.loudness[chn];
            }
            psv.tot_ener[chn] = prep.totEnergy[chn];
            MaskingLongResolve(psv, gd.l, gd.maskingLong, maskingLowerGlobal, prep.l[chn], eb[chn].data(), thr[chn].data(), chn);
        }
        if (cfg.mode == JointStereo && (useLongBlock[0] + useLongBlock[1]) == 2)
            ComputeMsThresholds(eb, thr, gd.l.mld_cb, ath.cb_l, athFactor, cfg.msfix, gd.l.npart);
        for (std::int32_t chn = 0; chn < nChnPsy; chn++) {
            PartitionsToLongBands(psv, gd.l, eb[chn].data(), thr[chn].data(), chn);
            LongPartitionsToShortBands(psv, gd.l_to_s, eb[chn].data(), thr[chn].data(), chn);
        }

        // short blocks
        const std::int32_t forceShortBlockCalc = gd.force_short_block_calc;
        for (std::int32_t sblock = 0; sblock < 3; sblock++) {
            for (std::int32_t chn = 0; chn < nChnPsy; ++chn) {
                const std::int32_t ch01 = chn & 0x01;
                if (useLongBlock[ch01] && !forceShortBlockCalc) {
                    SkipMaskingShort(psv, gd.s, chn, sblock);
                } else {
                    PsyBandPrep spread;
                    ComputeFftShort(gd, buffer, chn, sblock, fftenergyS, wsampS.data() + ch01);
                    SpreadShort(gd.s, gd.maskChains, fftenergyS[sblock].data(), spread);
                    MaskingShortResolve(psv, gd.s, maskingLowerGlobal, spread, eb[chn].data(), thr[chn].data(), chn);
                }
            }
            if (cfg.mode == JointStereo && (useLongBlock[0] + useLongBlock[1]) == 0)
                ComputeMsThresholds(eb, thr, gd.s.mld_cb, ath.cb_s, athFactor, cfg.msfix, gd.s.npart);
            for (std::int32_t chn = 0; chn < nChnPsy; ++chn) {
                const std::int32_t ch01 = chn & 0x01;
                if (!useLongBlock[ch01] || forceShortBlockCalc)
                    PartitionsToShortBands(psv, gd.s, eb[chn].data(), thr[chn].data(), chn, sblock);
            }
        }

        // short block pre-echo control
        float const interpAtt1 = static_cast<float>(NS_PREECHO_ATT1 * pcfact);
        float const interpAtt2 = static_cast<float>(NS_PREECHO_ATT2 * pcfact);
        for (std::int32_t chn = 0; chn < nChnPsy; chn++) {
            for (std::int32_t sb = 0; sb < SBMAX_s; sb++) {
                std::array<float, 3> newThmm;
                for (std::int32_t sblock = 0; sblock < 3; sblock++) {
                    float thmm = psv.thm[chn].s[sb][sblock];
                    thmm       = static_cast<float>(thmm * NS_PREECHO_ATT0);

                    float t1 = thmm;
                    float t2 = thmm;
                    float prevThm;

                    if (sblock > 0)
                        prevThm = newThmm[sblock - 1];
                    else
                        prevThm = lastThm[chn].s[sb][2];
                    if (nsAttacks[chn][sblock] >= 2 || nsAttacks[chn][sblock + 1] == 1)
                        t1 = NsInterp(prevThm, thmm, interpAtt1);
                    thmm = Min(t1, thmm);
                    if (nsAttacks[chn][sblock] == 1) {
                        t2 = NsInterp(prevThm, thmm, interpAtt2);
                    } else if ((sblock == 0 && psv.last_attacks[chn] == 3) || (sblock > 0 && nsAttacks[chn][sblock - 1] == 3)) {
                        // 2nd preceding block
                        switch (sblock) {
                            case 0: prevThm = lastThm[chn].s[sb][1]; break;
                            case 1: prevThm = lastThm[chn].s[sb][2]; break;
                            default: prevThm = newThmm[0]; break;
                        }
                        t2 = NsInterp(prevThm, thmm, interpAtt2);
                    }

                    thmm = Min(t1, thmm);
                    thmm = Min(t2, thmm);

                    // pulse like signal detection for fatboy.wav and so on
                    thmm *= subShortFactor[chn][sblock];

                    newThmm[sblock] = thmm;
                }
                for (std::int32_t sblock = 0; sblock < 3; sblock++)
                    psv.thm[chn].s[sb][sblock] = newThmm[sblock];
            }
        }
        for (std::int32_t chn = 0; chn < nChnPsy; chn++)
            psv.last_attacks[chn] = nsAttacks[chn][2];

        // final block type
        ApplyBlockType(psv, cfg.channels_out, useLongBlock.data(), blocktypeD.data());

        // perceptual entropy to return, without delay
        for (std::int32_t chn = 0; chn < nChnPsy; chn++) {
            float*          ppe;
            std::int32_t    type;
            const PsyRatio* mr;
            if (chn > 1) {
                ppe  = &percepMsEntropy[chn - 2];
                type = NORM_TYPE;
                if (blocktypeD[0] == SHORT_TYPE || blocktypeD[1] == SHORT_TYPE)
                    type = SHORT_TYPE;
                mr = &maskingMsRatio[grOut][chn - 2];
            } else {
                ppe  = &percepEntropy[chn];
                type = blocktypeD[chn];
                mr   = &maskingRatio[grOut][chn];
            }
            if (type == SHORT_TYPE)
                *ppe = PerceptualEntropyShort(*mr, maskingLowerGlobal);
            else
                *ppe = PerceptualEntropyLong(*mr, maskingLowerGlobal);
        }
    }

    void PsyAnalyze(const SessionConfig& cfg, const PsyConstants& gd, PsyState& psv, const AthTables& ath, float athAdjustFactor, float maskingLowerGlobal,
                    PsyResult& result, const ChannelSamples& buffer, std::int32_t grOut, GranuleChannel<PsyRatio>& maskingRatio,
                    GranuleChannel<PsyRatio>& maskingMsRatio, std::array<float, 2>& percepEntropy, std::array<float, 2>& percepMsEntropy,
                    std::array<float, 4>& energy, std::array<std::int32_t, 2>& blocktypeD)
    {
        PsyPrep prep;
        PsyPrepare(cfg, gd, ath, buffer, prep);
        PsyResolve(cfg, gd, psv, ath, athAdjustFactor, maskingLowerGlobal, result, prep, buffer, grOut, maskingRatio, maskingMsRatio, percepEntropy,
                   percepMsEntropy, energy, blocktypeD);
    }

    void AdjustAth(const SessionConfig& cfg, const AthTables& ath, AthAdjust& adjust, const PsyResult& result) noexcept
    {
        if (ath.use_adjust == 0) {
            adjust.factor = 1.0f; // no adjustment
            return;
        }

        // loudness based on the equal loudness curve; use the granule with the maximum combined loudness
        float maxPow = result.loudness_sq[0][0];
        float gr2Max = result.loudness_sq[1][0];
        if (cfg.channels_out == 2) {
            maxPow += result.loudness_sq[0][1];
            gr2Max += result.loudness_sq[1][1];
        } else {
            maxPow += maxPow;
            gr2Max += gr2Max;
        }
        maxPow = Max(maxPow, gr2Max);
        maxPow = static_cast<float>(maxPow * 0.5); // approaches 1.0 for full band noise

        // user tuning of ATH adjustment region
        maxPow *= ath.aa_sensitivity_p;

        // for an increase in loudness, set the adjustment to adjust_limit immediately after a
        // delay of one frame; for a decrease, reduce it towards adjust_limit gradually
        if (maxPow > 0.03125) {
            if (adjust.factor >= 1.0) {
                adjust.factor = 1.0f;
            } else if (adjust.factor < adjust.limit) {
                // ascend only to the preceding adjust_limit in case there is leading low volume
                adjust.factor = adjust.limit;
            }
            adjust.limit = 1.0f;
        } else {
            // about 32 dB maximum adjust (0.000625)
            float const adjLimNew = static_cast<float>(31.98 * maxPow + 0.000625);
            if (adjust.factor >= adjLimNew) { // descend gradually
                adjust.factor = static_cast<float>(adjust.factor * (adjLimNew * 0.075 + 0.925));
                if (adjust.factor < adjLimNew) // stop descent
                    adjust.factor = adjLimNew;
            } else { // ascend
                if (adjust.limit >= adjLimNew)
                    adjust.factor = adjLimNew;
                else if (adjust.factor < adjust.limit)
                    adjust.factor = adjust.limit;
            }
            adjust.limit = adjLimNew;
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
