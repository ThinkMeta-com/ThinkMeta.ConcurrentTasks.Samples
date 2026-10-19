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
#include <cstring>

#include "PsyMath.h"
#include "SessionConfig.h"

// Port of psymodel_init (psymodel.c): the partition bands, the spreading function, the ATH and
// minimum masking per partition, the attack thresholds and the masking adjustment of the VBR
// psychoacoustic model. Every expression keeps LAME's precision: float (float) where LAME
// computes in float, double where one of its double constants or math functions turns the
// arithmetic into double before the result is rounded.

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    namespace {

        constexpr double       LN_TO_LOG10            = 0.2302585093; // LAME's value without M_LN10
        constexpr double       DELBARK                = .34;          // size of each partition band, in barks
        constexpr double       TemporalMaskSustainSec = 0.01;
        constexpr double       NS_MSFIX               = 3.5;
        constexpr double       NSATTACKTHRE           = 4.4;
        constexpr std::int32_t NSATTACKTHRE_S         = 25;

        // LAME: s3_func() (psymodel.c): the spreading function, in units of energy
        float SpreadingFunction(float bark) noexcept
        {
            float tempx, x, tempy;
            tempx = bark;
            if (tempx >= 0)
                tempx *= 3;
            else
                tempx = static_cast<float>(tempx * 1.5);

            if (tempx >= 0.5 && tempx <= 2.5) {
                const float temp = static_cast<float>(tempx - 0.5);
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

        // LAME: stereo_demask() (psymodel.c): stereo demasking thresholds, formula reverse engineered from a plot
        float StereoDemask(double f) noexcept
        {
            double arg = FreqToBark(static_cast<float>(f));
            arg        = (Min(arg, 15.5) / 15.5);
            return static_cast<float>(std::pow(10.0, 1.25 * (1 - std::cos(PI * arg)) - 2.5));
        }

        // LAME: init_numline() (psymodel.c): partition bands of about DELBARK and their relation to scalefactor bands
        void InitNumline(PsyPartitions& gd, float sfreq, std::int32_t fftSize, std::int32_t mdctSize, std::int32_t sbmax, const std::int32_t* scalepos) noexcept
        {
            std::array<float, CBANDS + 1>      bFrq;
            const float                        mdctFreqFrac = sfreq / (2.0f * mdctSize);
            const float                        deltafreq    = fftSize / (2.0f * mdctSize);
            std::array<std::int32_t, HBLKSIZE> partition    = {0};
            std::int32_t                       i, j, ni;

            sfreq /= fftSize;
            j  = 0;
            ni = 0;
            // numlines, the number of spectral lines in each partition band
            for (i = 0; i < CBANDS; i++) {
                const float bark1 = FreqToBark(sfreq * j);
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
                const float        freq = sfreq * (j + nl / 2);
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
                const float fTmp = mdctFreqFrac * end;
                float       boW  = (fTmp - bFrq[bo]) / (bFrq[bo + 1] - bFrq[bo]);
                if (boW < 0)
                    boW = 0;
                else if (boW > 1)
                    boW = 1;
                gd.bo_weight[sfb] = boW;
                gd.mld[sfb]       = StereoDemask(mdctFreqFrac * start);
            }
        }

        // LAME: compute_bark_values() (psymodel.c): bark value and width of each partition band
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

        // LAME: init_s3_values() (psymodel.c): the spreading function between all partition bands, stored sparse
        void InitS3Values(PsyPartitions& gd, const float* bval, const float* bvalWidth, const float* norm) noexcept
        {
            std::array<std::array<float, CBANDS>, CBANDS> s3;
            const std::int32_t                            npart = gd.npart;

            std::memset(&s3, 0, sizeof(s3));

            // s3[i][j]: the spreading function centered at band j (masker), for band i (maskee)
            for (std::int32_t i = 0; i < npart; i++) {
                for (std::int32_t j = 0; j < npart; j++) {
                    const float v = SpreadingFunction(bval[i] - bval[j]) * bvalWidth[j];
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

    // LAME: psymodel_init() (psymodel.c)
    std::int32_t PsyInit(SessionConfig& cfg, PsyConstants& gd, PsyState& psv, AthTables& ath, AthAdjust& adjust)
    {
        const float bvlA      = 13;
        const float bvlB      = 24;
        const float snrLA     = 0;
        const float snrLB     = 0;
        const float snrSA     = LameFloat(-8.25);
        const float snrSB     = LameFloat(-4.5);
        const float xav       = 10;
        const float xbv       = 12;
        const float sfreq     = static_cast<float>(cfg.samplerate_out);
        const float minvalLow = (0.f - cfg.minval);

        std::array<float, CBANDS> bval;
        std::array<float, CBANDS> bvalWidth;
        std::array<float, CBANDS> norm;
        std::int32_t              i, j, b, k;

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
                const float freq  = static_cast<float>(sfreq * j / (1000.0 * BLKSIZE));
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
                const float freq  = static_cast<float>(sfreq * j / (1000.0 * BLKSIZE_s));
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
        adjust.factor              = LameFloat(0.01); // minimum, for leading low loudness
        adjust.limit               = 1.0f;            // on lead, allow adjust up to maximum

        if (cfg.ATHtype != -1) {
            // equal loudness weights (eql_w): ATH in dB to relative power
            float       freq       = 0.0f;
            const float freqInc    = static_cast<float>(cfg.samplerate_out) / static_cast<float>(BLKSIZE);
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
            // constant table taken over from LAME (psymodel.c) as data: the masking adjustment
            // by VBR quality, a FLOAT table of double literals (LameFloat rounds them alike)
            static const auto  sk = std::to_array<float>({LameFloat(-7.4), LameFloat(-7.4), LameFloat(-7.4), LameFloat(-9.5), LameFloat(-7.4), LameFloat(-6.1),
                                                          LameFloat(-5.5), LameFloat(-4.7), LameFloat(-4.7), LameFloat(-4.7), LameFloat(-4.7)});
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

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
