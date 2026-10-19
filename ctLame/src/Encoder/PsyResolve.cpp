// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Psy.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "PsyInternal.h"
#include "PsyMath.h"
#include "SessionConfig.h"

// Port of psymodel.c, the part of L3psycho_anal_vbr with state (PsyResolve): attacks, block types,
// masking with pre-echo control, the short blocks where needed, M/S thresholds and perceptual
// entropy; also adjust_ATH (encoder.c). Every expression keeps LAME's precision: float (float)
// where LAME computes in float, double where one of its double constants or math functions turns
// the arithmetic into double before the result is rounded.

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        constexpr double NS_PREECHO_ATT0 = 0.8;
        constexpr double NS_PREECHO_ATT1 = 0.6;
        constexpr double NS_PREECHO_ATT2 = 0.3;

        // LAME: convert_partition2scalefac() (psymodel.c): partition bands to scalefactor bands; the band at the
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
                const float wCurr = gd.bo_weight[sb];
                const float wNext = 1.0f - wCurr;
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

        // LAME: convert_partition2scalefac_s() (psymodel.c)
        void PartitionsToShortBands(PsyState& psv, const PsyPartitions& gds, const float* eb, const float* thr, std::int32_t chn, std::int32_t sblock) noexcept
        {
            std::array<float, SBMAX_s> enn, thm;
            PartitionsToScalefactorBands(gds, eb, thr, enn.data(), thm.data());
            for (std::int32_t sb = 0; sb < SBMAX_s; ++sb) {
                psv.en[chn].s[sb][sblock]  = enn[sb];
                psv.thm[chn].s[sb][sblock] = thm[sb];
            }
        }

        // LAME: convert_partition2scalefac_l() (psymodel.c)
        void PartitionsToLongBands(PsyState& psv, const PsyPartitions& gdl, const float* eb, const float* thr, std::int32_t chn) noexcept
        {
            PartitionsToScalefactorBands(gdl, eb, thr, psv.en[chn].l.data(), psv.thm[chn].l.data());
        }

        // LAME: convert_partition2scalefac_l_to_s() (psymodel.c): the long block result also
        // serves the short blocks, until they are computed
        void LongPartitionsToShortBands(PsyState& psv, const PsyPartitions& gdLongToShort, const float* eb, const float* thr, std::int32_t chn) noexcept
        {
            std::array<float, SBMAX_s> enn, thm;
            PartitionsToScalefactorBands(gdLongToShort, eb, thr, enn.data(), thm.data());
            for (std::int32_t sb = 0; sb < SBMAX_s; ++sb) {
                const float scale  = static_cast<float>(1. / 64.f);
                const float tmpEnn = enn[sb];
                const float tmpThm = thm[sb] * scale;
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

        // LAME: pecalc_s() (psymodel.c): perceptual entropy of a short block granule
        float PerceptualEntropyShort(const PsyRatio& mr, float maskingLower) noexcept
        {
            // these values are tuned only for 44.1kHz...
            // constant table taken over from LAME (psymodel.c) as data
            static const auto regcoef_s = std::to_array<float>({LameFloat(11.8), LameFloat(13.6), LameFloat(17.2), 32.0f, LameFloat(46.5), LameFloat(51.3),
                                                                LameFloat(57.5), LameFloat(67.1), LameFloat(71.5), LameFloat(84.6), LameFloat(97.6), 130.0f});

            float peS = 1236.28f / 4;
            for (std::int32_t sb = 0; sb < SBMAX_s - 1; sb++) {
                for (std::int32_t sblock = 0; sblock < 3; sblock++) {
                    const float thm = mr.thm.s[sb][sblock];
                    if (thm > 0.0f) {
                        const float x  = thm * maskingLower;
                        const float en = mr.en.s[sb][sblock];
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

        // LAME: pecalc_l() (psymodel.c): perceptual entropy of a long block granule
        float PerceptualEntropyLong(const PsyRatio& mr, float maskingLower) noexcept
        {
            // these values are tuned only for 44.1kHz...
            // constant table taken over from LAME (psymodel.c) as data
            static const auto regcoef_l = std::to_array<float>(
                {LameFloat(6.8),  LameFloat(5.8),  LameFloat(5.8),  LameFloat(6.4),  LameFloat(6.5),  LameFloat(9.9),  LameFloat(12.1),
                 LameFloat(14.4), 15.0f,           LameFloat(18.9), LameFloat(21.6), LameFloat(26.9), LameFloat(34.2), LameFloat(40.2),
                 LameFloat(46.8), LameFloat(56.5), LameFloat(60.7), LameFloat(73.9), LameFloat(85.7), LameFloat(93.4), LameFloat(126.1)});

            float peL = 1124.23f / 4;
            for (std::int32_t sb = 0; sb < SBMAX_l - 1; sb++) {
                const float thm = mr.thm.l[sb];
                if (thm > 0.0f) {
                    const float x  = thm * maskingLower;
                    const float en = mr.en.l[sb];
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

        // vbrpsy_attack_detection, the part with state: attacks from the sub-short block peaks
        // against those of the last granule; also hands out the last call's masking and energy
        void AttackResolve(const SessionConfig& cfg, const PsyConstants& gd, PsyState& psv, const SubShortPeaks& subShort, std::int32_t grOut,
                           const PsyGranuleOutput& out, std::array<std::array<float, 3>, 4>& subShortFactor,
                           std::array<std::array<std::int32_t, 4>, 4>& nsAttacks, std::array<std::int32_t, 2>& useLongBlock) noexcept
        {
            const std::int32_t nChnOut = cfg.channels_out;
            const std::int32_t nChnPsy = PsyDetail::PsyChannels(cfg);

            for (std::int32_t chn = 0; chn < nChnOut; chn++) {
                // the maskings of the last call, returned with one granule delay
                out.maskingRatio[grOut][chn].en  = psv.en[chn];
                out.maskingRatio[grOut][chn].thm = psv.thm[chn];
                if (nChnPsy > 2) {
                    out.maskingMsRatio[grOut][chn].en  = psv.en[chn + 2];
                    out.maskingMsRatio[grOut][chn].thm = psv.thm[chn + 2];
                }
            }

            for (std::int32_t chn = 0; chn < nChnPsy; chn++) {
                std::array<float, 12> attackIntensity;
                std::array<float, 12> enSubshort;
                std::array<float, 4>  enShort        = {0, 0, 0, 0};
                std::int32_t          nsUseLongBlock = 1;

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
                    const float enn    = enSubshort[i * 3 + 3] + enSubshort[i * 3 + 4] + enSubshort[i * 3 + 5];
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
                    const float x = gd.attack_threshold[chn];
                    for (std::int32_t i = 0; i < 12; i++) {
                        if (nsAttacks[chn][i / 3] == 0 && attackIntensity[i] > x)
                            nsAttacks[chn][i / 3] = (i % 3) + 1;
                    }
                }
                // there should be an energy change between short blocks, to avoid periodic signals
                for (std::int32_t i = 1; i < 4; i++) {
                    const float u = enShort[i - 1];
                    const float v = enShort[i];
                    const float m = Max(u, v);
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
                out.energy[chn] = psv.tot_ener[chn];
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

        // vbrpsy_compute_masking_s, the part with state
        void MaskingShortResolve(PsyState& psv, const PsyPartitions& gds, float maskingLowerGlobal, const PsyBandPrep& in, float* eb, float* thr,
                                 std::int32_t chn) noexcept
        {
            std::memcpy(eb, in.eb.data(), sizeof(in.eb));
            std::int32_t b;
            for (b = 0; b < gds.npart; b++) {
                const float maskingLower = gds.masking_lower[b] * maskingLowerGlobal;
                const float ecb          = in.ecb[b];
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
            const float msfix2 = msfix * 2.f;
            for (std::int32_t b = 0; b < n; ++b) {
                const float ebM  = eb[2][b];
                const float ebS  = eb[3][b];
                const float thmL = thr[0][b];
                const float thmR = thr[1][b];
                float       thmM = thr[2][b];
                float       thmS = thr[3][b];
                float       rmid, rside;

                // use this fix if L & R masking differs by 2db or less
                if (thmL <= 1.58f * thmR && thmR <= 1.58f * thmL) {
                    const float mldM = cbMld[b] * ebS;
                    const float mldS = cbMld[b] * ebM;
                    const float tmpM = Min(thmS, mldM);
                    const float tmpS = Min(thmM, mldS);
                    rmid             = Max(thmM, tmpM);
                    rside            = Max(thmS, tmpS);
                } else {
                    rmid  = thmM;
                    rside = thmS;
                }
                if (msfix > 0.f) {
                    // adjust M/S maskings if the user set "msfix" (Naoki Shibata 2000)
                    const float ath   = athCb[b] * athLower;
                    const float tmpL  = Max(thmL, ath);
                    const float tmpR  = Max(thmR, ath);
                    const float thmLR = Min(tmpL, tmpR);
                    thmM              = Max(rmid, ath);
                    thmS              = Max(rside, ath);
                    const float thmMS = thmM + thmS;
                    if (thmMS > 0.f && (thmLR * msfix2) < thmMS) {
                        const float f = thmLR * msfix2 / thmMS;
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

    } // namespace

    // LAME: the pre-echo loop of vbrpsy_compute_masking_l() (psymodel.c)
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

    // LAME: L3psycho_anal_vbr() (psymodel.c), the part with state
    void PsyResolve(const SessionConfig& cfg, const PsyConstants& gd, PsyState& psv, const AthTables& ath, float athAdjustFactor, float maskingLowerGlobal,
                    PsyResult& result, const PsyPrep& prep, const ChannelSamples& buffer, std::int32_t grOut, const PsyGranuleOutput& out) noexcept
    {
        std::array<std::int32_t, 2>& blocktypeD = out.blocktype;
        std::array<PsyXmin, 4>       lastThm;

        std::array<std::array<float, HBLKSIZE_s>, 3>               fftenergyS;
        std::array<std::array<std::array<float, BLKSIZE_s>, 3>, 2> wsampS;
        std::array<std::array<float, CBANDS>, 4>                   eb, thr;
        std::array<std::array<float, 3>, 4>                        subShortFactor;

        const float pcfact    = 0.6f;
        const float athFactor = (cfg.msfix > 0.f) ? (cfg.ATH_offset_factor * athAdjustFactor) : 1.f;

        std::array<std::array<std::int32_t, 4>, 4> nsAttacks = {};
        std::array<std::int32_t, 2>                useLongBlock;

        const std::int32_t nChnPsy = PsyDetail::PsyChannels(cfg);

        lastThm = psv.thm;

        AttackResolve(cfg, gd, psv, prep.subShort, grOut, out, subShortFactor, nsAttacks, useLongBlock);

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
                    PsyDetail::ComputeFftShort(gd, buffer, chn, sblock, fftenergyS, wsampS.data() + ch01);
                    PsyDetail::SpreadShort(gd.s, gd.maskChains, fftenergyS[sblock].data(), spread);
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

        // short block pre-echo control; kept close to LAME on purpose: the interplay of t1, t2
        // and the previous thresholds is LAME's tuning, a cleaner form would hide what it compares
        const float interpAtt1 = static_cast<float>(NS_PREECHO_ATT1 * pcfact);
        const float interpAtt2 = static_cast<float>(NS_PREECHO_ATT2 * pcfact);
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
                ppe  = &out.peMs[chn - 2];
                type = NORM_TYPE;
                if (blocktypeD[0] == SHORT_TYPE || blocktypeD[1] == SHORT_TYPE)
                    type = SHORT_TYPE;
                mr = &out.maskingMsRatio[grOut][chn - 2];
            } else {
                ppe  = &out.pe[chn];
                type = blocktypeD[chn];
                mr   = &out.maskingRatio[grOut][chn];
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
        const PsyGranuleOutput out{maskingRatio, maskingMsRatio, percepEntropy, percepMsEntropy, energy, blocktypeD};
        PsyResolve(cfg, gd, psv, ath, athAdjustFactor, maskingLowerGlobal, result, prep, buffer, grOut, out);
    }

    // LAME: adjust_ATH() (encoder.c)
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
            const float adjLimNew = static_cast<float>(31.98 * maxPow + 0.000625);
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

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
