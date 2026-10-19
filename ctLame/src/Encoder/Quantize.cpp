// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Quantize.h"

#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "BitCount.h"
#include "PsyMath.h"
#include "SessionConfig.h"
#include "VbrQuantize.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    namespace {

        constexpr std::int32_t NSATHSCALE = 100; // assuming a dynamic range of 96 dB this value should be 92

        // ATHmdct: the ATH at a frequency, scaled to MDCT energy
        float AthMdct(const SessionConfig& config, float freq) noexcept
        {
            float ath = AthFormula(config, freq);
            if (config.ATHfixpoint > 0)
                ath -= config.ATHfixpoint;
            else
                ath -= NSATHSCALE;
            ath += config.ATH_offset_db;
            return powf(10.0f, ath * 0.1f);
        }

        // LAME: the band loops of compute_ath() (quantize_pvt.c)
        // the lowest ATH in each band; frequencies of the long and short block lines
        void BandAth(const SessionConfig& config, const std::int32_t* bounds, std::int32_t bands, float lines, float* out) noexcept
        {
            const float sampleRate = static_cast<float>(config.samplerate_out);
            for (std::int32_t sfb = 0; sfb < bands; ++sfb) {
                out[sfb] = FLT_MAX;
                for (std::int32_t i = bounds[sfb]; i < bounds[sfb + 1]; ++i) {
                    const float freq = i * sampleRate / lines;
                    out[sfb]         = Min(out[sfb], AthMdct(config, freq));
                }
            }
        }

        // compute_ath
        void ComputeAth(const SessionConfig& config, AthTables& ath) noexcept
        {
            const ScalefacStruct& bands = config.scalefac_band;

            BandAth(config, bands.l.data(), SBMAX_l, 2 * 576, ath.l.data());
            BandAth(config, bands.psfb21.data(), PSFB21, 2 * 576, ath.psfb21.data());
            BandAth(config, bands.s.data(), SBMAX_s, 2 * 192, ath.s.data());
            for (std::int32_t sfb = 0; sfb < SBMAX_s; ++sfb)
                ath.s[sfb] *= bands.s[sfb + 1] - bands.s[sfb];
            BandAth(config, bands.psfb12.data(), PSFB12, 2 * 192, ath.psfb12.data());
            for (std::int32_t sfb = 0; sfb < PSFB12; ++sfb)
                ath.psfb12[sfb] *= bands.s[13] - bands.s[12]; // not sure about this (LAME)

            // no ATH: -200 dB
            if (config.noATH) {
                for (float& v : ath.l)
                    v = static_cast<float>(1E-20);
                for (float& v : ath.psfb21)
                    v = static_cast<float>(1E-20);
                for (float& v : ath.s)
                    v = static_cast<float>(1E-20);
                for (float& v : ath.psfb12)
                    v = static_cast<float>(1E-20);
            }

            ath.floor = static_cast<float>(10. * std::log10(static_cast<double>(AthMdct(config, -1.f))));
        }

        // athAdjust: lowers the ATH by the adjust factor, keeping the noise floor
        float AdjustedAth(float a, float x, float athFloor, float athFixpoint) noexcept
        {
            const float o = 90.30873362f;
            const float p = (athFixpoint < 1.f) ? 94.82444863f : athFixpoint;
            float       u = static_cast<float>(FastLog10X(x, 10.0f));
            const float v = a * a;
            float       w = 0.0f;
            u -= athFloor; // undo scaling
            if (v > 1E-20f)
                w = static_cast<float>(1.f + FastLog10X(v, 10.0f / o));
            if (w < 0)
                w = 0.f;
            u *= w;
            u += athFloor + o - p; // redo scaling
            return powf(10.f, 0.1f * u);
        }

        // LAME: the band loop body of calc_xmin() (quantize_pvt.c)
        // the allowed distortion of one band (long, or one window of a short band)
        float BandXmin(const float*& xr, std::int32_t width, float ath, float thm, float en, float factor, std::int32_t& athOver,
                       std::uint8_t& aboveCutoff) noexcept
        {
            const float rh1 = ath / width;
            float       rh2 = static_cast<float>(DBL_EPSILON);
            float       en0 = 0.0f;
            for (std::int32_t l = 0; l < width; ++l) {
                const float xa = *xr++;
                const float x2 = xa * xa;
                en0 += x2;
                rh2 += (x2 < rh1) ? x2 : rh1;
            }
            if (en0 > ath)
                ++athOver;

            float xmin;
            if (en0 < ath)
                xmin = en0;
            else if (rh2 < ath)
                xmin = ath;
            else
                xmin = rh2;

            if (en > 1e-12f) {
                float x = en0 * thm / en;
                x *= factor;
                if (xmin < x)
                    xmin = x;
            }
            // LAME's Max() of a float and DBL_EPSILON compares in double
            xmin        = static_cast<float>(xmin > DBL_EPSILON ? static_cast<double>(xmin) : DBL_EPSILON);
            aboveCutoff = (en0 > xmin + 1e-14f) ? 1 : 0;
            return xmin;
        }

        // init_outer_loop: a fresh gr_info for the granule; short blocks are reordered so that
        // the three windows of a band follow each other
        void InitOuterLoop(const SessionConfig& config, GrInfo& gi) noexcept
        {
            const ScalefacStruct& bands = config.scalefac_band;
            const std::int32_t    sfb21 = config.sfb21_extra;

            gi.part2_3_length    = 0;
            gi.big_values        = 0;
            gi.count1            = 0;
            gi.global_gain       = 210;
            gi.scalefac_compress = 0;
            // mixed_block_flag and block_type come from the psychoacoustic model
            gi.table_select[0]    = 0;
            gi.table_select[1]    = 0;
            gi.table_select[2]    = 0;
            gi.subblock_gain[0]   = 0;
            gi.subblock_gain[1]   = 0;
            gi.subblock_gain[2]   = 0;
            gi.subblock_gain[3]   = 0; // this one is always 0
            gi.region0_count      = 0;
            gi.region1_count      = 0;
            gi.preflag            = 0;
            gi.scalefac_scale     = 0;
            gi.count1table_select = 0;
            gi.part2_length       = 0;
            gi.sfb_lmax           = SBPSY_l;
            gi.sfb_smin           = SBPSY_s;
            gi.psy_lmax           = sfb21 ? SBMAX_l : SBPSY_l;
            gi.psymax             = gi.psy_lmax;
            gi.sfbmax             = gi.sfb_lmax;
            gi.sfbdivide          = 11;
            for (std::int32_t sfb = 0; sfb < SBMAX_l; ++sfb) {
                gi.width[sfb]  = bands.l[sfb + 1] - bands.l[sfb];
                gi.window[sfb] = 3; // which is always 0
            }

            if (gi.block_type == SHORT_TYPE) {
                gi.sfb_smin = 0;
                gi.sfb_lmax = 0;
                if (gi.mixed_block_flag) {
                    // MPEG-1: sfbs 0-7 long block, 3-12 short blocks
                    gi.sfb_smin = 3;
                    gi.sfb_lmax = 8;
                }
                gi.psymax    = gi.sfb_lmax + 3 * ((sfb21 ? SBMAX_s : SBPSY_s) - gi.sfb_smin);
                gi.sfbmax    = gi.sfb_lmax + 3 * (SBPSY_s - gi.sfb_smin);
                gi.sfbdivide = gi.sfbmax - 18;
                gi.psy_lmax  = gi.sfb_lmax;

                // the MDCT gives the windows interleaved line by line; within each band the
                // bitstream wants window 0, then 1, then 2, each in increasing frequency
                std::array<float, 576> work;
                work       = gi.xr;
                float* out = gi.xr.data() + bands.l[gi.sfb_lmax]; // may be the end
                for (std::int32_t sfb = gi.sfb_smin; sfb < SBMAX_s; ++sfb) {
                    for (std::int32_t window = 0; window < 3; ++window) {
                        for (std::int32_t l = bands.s[sfb]; l < bands.s[sfb + 1]; ++l)
                            *out++ = work[3 * l + window];
                    }
                }

                std::int32_t j = gi.sfb_lmax;
                for (std::int32_t sfb = gi.sfb_smin; sfb < SBMAX_s; ++sfb) {
                    gi.width[j] = gi.width[j + 1] = gi.width[j + 2] = bands.s[sfb + 1] - bands.s[sfb];
                    gi.window[j]                                    = 0;
                    gi.window[j + 1]                                = 1;
                    gi.window[j + 2]                                = 2;
                    j += 3;
                }
            }

            gi.count1bits        = 0;
            gi.max_nonzero_coeff = 575;
            gi.scalefac.fill(0); // fresh scalefactors are all zero
        }

        // init_xrpow with LAME's SSE kernel (init_xrpow_core_sse, XrpowScalar and its SIMD
        // versions). Returns false for a silent granule.
        bool InitXrpow(const QuantizeKernels& kernels, GrInfo& gi, std::array<float, 576>& xrpow) noexcept
        {
            const std::int32_t upper = gi.max_nonzero_coeff;
            std::memset(&xrpow[upper], 0, (576 - upper) * sizeof(xrpow[0]));

            float sum;
            kernels.xrpow(gi.xr.data(), xrpow.data(), upper + 1, sum, gi.xrpow_max);

            if (sum > static_cast<float>(1E-20))
                return true;
            gi.l3_enc.fill(0);
            return false;
        }

        // ms_convert: L/R to mid/side
        void MsConvert(SideInfo& side, std::int32_t gr) noexcept
        {
            float* const left  = side.tt[gr][0].xr.data();
            float* const right = side.tt[gr][1].xr.data();
            for (std::int32_t i = 0; i < 576; ++i) {
                const float l = left[i];
                const float r = right[i];
                left[i]       = (l + r) * static_cast<float>(SQRT2 * 0.5);
                right[i]      = (l - r) * static_cast<float>(SQRT2 * 0.5);
            }
        }

        // on_pe: splits a granule's target bits between the channels by perceptual entropy
        void OnPe(const SessionConfig& config, QuantizeState& state, const GranuleChannel<float>& pe, std::array<std::int32_t, 2>& targetBits,
                  std::int32_t meanBits, std::int32_t gr) noexcept
        {
            const std::int32_t          channels = config.channels_out;
            std::array<std::int32_t, 2> addBits  = {0, 0};
            std::int32_t                target, extra;
            ReservoirMaxBits(config, state.reservoir, state.substepShaping, meanBits, target, extra);

            std::int32_t bits = 0;
            for (std::int32_t ch = 0; ch < channels; ++ch) {
                targetBits[ch] = Min(MAX_BITS_PER_CHANNEL, target / channels);

                addBits[ch] = static_cast<std::int32_t>(targetBits[ch] * pe[gr][ch] / 700.0 - targetBits[ch]);

                // at most increase bits by 1.5 * average
                if (addBits[ch] > meanBits * 3 / 4)
                    addBits[ch] = meanBits * 3 / 4;
                if (addBits[ch] < 0)
                    addBits[ch] = 0;
                if (addBits[ch] + targetBits[ch] > MAX_BITS_PER_CHANNEL)
                    addBits[ch] = Max(0, MAX_BITS_PER_CHANNEL - targetBits[ch]);
                bits += addBits[ch];
            }
            if (bits > extra && bits > 0) {
                for (std::int32_t ch = 0; ch < channels; ++ch)
                    addBits[ch] = extra * addBits[ch] / bits;
            }

            for (std::int32_t ch = 0; ch < channels; ++ch) {
                targetBits[ch] += addBits[ch];
                extra -= addBits[ch];
            }

            bits = 0;
            for (std::int32_t ch = 0; ch < channels; ++ch)
                bits += targetBits[ch];
            if (bits > MAX_BITS_PER_GRANULE) {
                for (std::int32_t ch = 0; ch < channels; ++ch) {
                    targetBits[ch] *= MAX_BITS_PER_GRANULE;
                    targetBits[ch] /= bits;
                }
            }
        }

        // VBR_new_prepare without what the search did (M/S conversion, init_outer_loop,
        // calc_xmin): the frame sizes of all bitrates and the bits per granule and channel.
        void VbrNewPrepare(const SessionConfig& config, QuantizeState& state, FrameData& frame, const GranuleChannel<float>& pe,
                           std::array<std::int32_t, 16>& frameBits, GranuleChannel<std::int32_t>& maxBits, std::int32_t& maxResv) noexcept
        {
            const std::int32_t maxIndex = config.vbr_max_bitrate_index;
            SideInfo&          side     = frame.side;

            std::int32_t meanBits;
            frame.bitrateIndex = maxIndex;
            (void)ReservoirFrameBegin(config, state.reservoir, side, maxIndex, meanBits);
            maxResv = state.reservoir.max;

            // get_framebits: what each bitrate can hold, with the reservoir
            for (std::int32_t i = 1; i <= maxIndex; ++i) {
                std::int32_t unused;
                frameBits[i] = ReservoirFrameBegin(config, state.reservoir, side, i, unused);
            }
            frame.bitrateIndex                  = maxIndex;
            const std::int32_t maximumFrameBits = frameBits[maxIndex];

            std::int32_t bits = 0;
            for (std::int32_t gr = 0; gr < 2; ++gr) {
                OnPe(config, state, pe, maxBits[gr], meanBits, gr);
                for (std::int32_t ch = 0; ch < config.channels_out; ++ch)
                    bits += maxBits[gr][ch];
            }
            for (std::int32_t gr = 0; gr < 2; ++gr) {
                for (std::int32_t ch = 0; ch < config.channels_out; ++ch) {
                    if (bits > maximumFrameBits && bits > 0) {
                        maxBits[gr][ch] *= maximumFrameBits;
                        maxBits[gr][ch] /= bits;
                    }
                }
            }
            if (frame.analogSilence)
                maxResv = 0;
        }

    } // namespace

    // init_xrpow_core_sse with scalar code: |xr|^(3/4), the sum in four lanes (line i in lane i % 4)
    // combined at the end, and the maximum
    void XrpowScalar(const float* xr, float* xrpow, std::int32_t count, float& sum, float& max) noexcept
    {
        std::array<float, 4> laneSum = {0, 0, 0, 0};
        std::array<float, 4> laneMax = {0, 0, 0, 0};
        for (std::int32_t i = 0; i < count; ++i) {
            const float x = std::fabs(xr[i]);
            const float y = std::sqrt(x * std::sqrt(x));
            laneSum[i & 3] += x;
            if (!(laneMax[i & 3] > y)) // _mm_max_ps(a, b) is a > b ? a : b
                laneMax[i & 3] = y;
            xrpow[i] = y;
        }
        sum            = laneSum[0] + laneSum[1] + laneSum[2] + laneSum[3];
        const float ma = laneMax[0] > laneMax[1] ? laneMax[0] : laneMax[1];
        const float mb = laneMax[2] > laneMax[3] ? laneMax[2] : laneMax[3];
        max            = ma > mb ? ma : mb;
    }

    QuantizeKernels QuantizeKernelsFor(SimdLevel level) noexcept
    {
        switch (level) {
            case SimdLevel::Sse41: return {XrpowSse41, QuantizeLinesSse41, SfbNoiseSse41, VecMaxSse41};
            case SimdLevel::Avx2: return {XrpowAvx2, QuantizeLinesSse41, SfbNoiseAvx2, VecMaxAvx2};
            default: return {XrpowScalar, QuantizeLinesScalar, SfbNoiseScalar, VecMaxScalar};
        }
    }

    void QuantizeInit(const SessionConfig& config, QuantizeConstants& constants, AthTables& ath, QuantizeState& state) noexcept
    {
        constants.kernels    = QuantizeKernelsFor(SimdLevel::Scalar);
        state                = QuantizeState{};
        state.substepShaping = config.substep_shaping;
        ComputeAth(config, ath);

        constants.pow43[0] = 0.0;
        for (std::int32_t i = 1; i < PRECALC_SIZE; ++i)
            constants.pow43[i] = static_cast<float>(std::pow(static_cast<double>(static_cast<float>(i)), 4.0 / 3.0));

        // the quantizer truncates; adj43 moves the rounding point to where the error of
        // i^(4/3) and (i + 1)^(4/3) is equal
        std::int32_t i = 0;
        for (; i < PRECALC_SIZE - 1; ++i)
            constants.adj43[i] = static_cast<float>((i + 1) - std::pow(0.5 * (constants.pow43[i] + constants.pow43[i + 1]), 0.75));
        constants.adj43[i] = 0.5;

        for (i = 0; i < Q_MAX; ++i)
            constants.ipow20[i] = static_cast<float>(std::pow(2.0, static_cast<double>(i - 210) * -0.1875));
        for (i = 0; i <= Q_MAX + Q_MAX2; ++i)
            constants.pow20[i] = static_cast<float>(std::pow(2.0, static_cast<double>(i - 210 - Q_MAX2) * 0.25));

        InitRegionSplit(config, constants.bv_scf);
        InitHuffmanPairLengths(constants.pairs);
    }

    std::int32_t CalcXmin(const SessionConfig& config, const AthTables& ath, float athAdjustFactor, float psyDecay, const PsyRatio& ratio, GrInfo& gi,
                          float* xmin) noexcept
    {
        const float* xr      = gi.xr.data();
        std::int32_t athOver = 0;
        std::int32_t gsfb    = 0;

        for (; gsfb < gi.psy_lmax; ++gsfb) {
            float bandAth = AdjustedAth(athAdjustFactor, ath.l[gsfb], ath.floor, config.ATHfixpoint);
            bandAth *= config.longfact[gsfb];
            *xmin++ = BandXmin(xr, gi.width[gsfb], bandAth, ratio.thm.l[gsfb], ratio.en.l[gsfb], config.longfact[gsfb], athOver, gi.energy_above_cutoff[gsfb]);
        }

        // the highest non-zero line; long blocks end on an odd line, short blocks on a full
        // triple of windows
        std::int32_t maxNonzero = 0;
        for (std::int32_t k = 575; k > 0; --k) {
            if (std::fabs(gi.xr[k]) > 1e-12f) {
                maxNonzero = k;
                break;
            }
        }
        if (gi.block_type != SHORT_TYPE) {
            maxNonzero |= 1;
        } else {
            maxNonzero /= 6;
            maxNonzero *= 6;
            maxNonzero += 5;
        }
        if (config.sfb21_extra == 0 && config.samplerate_out < 44000) {
            const std::int32_t limit = gi.block_type != SHORT_TYPE ? config.scalefac_band.l[21] - 1 : 3 * config.scalefac_band.s[12] - 1;
            if (maxNonzero > limit)
                maxNonzero = limit;
        }
        gi.max_nonzero_coeff = maxNonzero;

        for (std::int32_t sfb = gi.sfb_smin; gsfb < gi.psymax; ++sfb, gsfb += 3) {
            float bandAth = AdjustedAth(athAdjustFactor, ath.s[sfb], ath.floor, config.ATHfixpoint);
            bandAth *= config.shortfact[sfb];
            const std::int32_t width = gi.width[gsfb];
            for (std::int32_t b = 0; b < 3; ++b) {
                *xmin++
                    = BandXmin(xr, width, bandAth, ratio.thm.s[sfb][b], ratio.en.s[sfb][b], config.shortfact[sfb], athOver, gi.energy_above_cutoff[gsfb + b]);
            }
            if (config.use_temporal_masking_effect) {
                if (xmin[-3] > xmin[-3 + 1])
                    xmin[-3 + 1] += (xmin[-3] - xmin[-3 + 1]) * psyDecay;
                if (xmin[-3 + 1] > xmin[-3 + 2])
                    xmin[-3 + 2] += (xmin[-3 + 1] - xmin[-3 + 2]) * psyDecay;
            }
        }
        return athOver;
    }

    void QuantSearch(const SessionConfig& config, const QuantizeConstants& constants, const AthTables& ath, float psyDecay, FrameData& frame) noexcept
    {
        // M/S coding uses the M/S masking
        const bool                      ms    = frame.modeExt == MPG_MD_MS_LR;
        const GranuleChannel<PsyRatio>& ratio = ms ? frame.maskingMs : frame.maskingLr;
        SideInfo&                       side  = frame.side;

        // the part of VBR_new_prepare that does not depend on the reservoir
        bool analogSilence = true;
        for (std::int32_t gr = 0; gr < 2; ++gr) {
            if (ms)
                MsConvert(side, gr);
            for (std::int32_t ch = 0; ch < config.channels_out; ++ch) {
                GrInfo& gi = side.tt[gr][ch];

                InitOuterLoop(config, gi);
                if (CalcXmin(config, ath, frame.athAdjustFactor, psyDecay, ratio[gr][ch], gi, frame.xmin[gr][ch].data()) != 0)
                    analogSilence = false;
            }
        }
        frame.analogSilence = analogSilence;

        std::memset(&frame.xrpow, 0, sizeof(frame.xrpow));
        for (std::int32_t gr = 0; gr < 2; ++gr) {
            for (std::int32_t ch = 0; ch < config.channels_out; ++ch)
                frame.search[gr][ch].active = InitXrpow(constants.kernels, side.tt[gr][ch], frame.xrpow[gr][ch]); // a silent granule needs no bits
        }

        VbrSearchFrame(config, constants, side, frame.xrpow, frame.xmin, frame.search);
    }

    bool QuantCommit(const SessionConfig& config, const QuantizeConstants& constants, QuantizeState& state, FrameData& frame) noexcept
    {
        // M/S coding uses the M/S perceptual entropy
        const GranuleChannel<float>& pe   = frame.modeExt == MPG_MD_MS_LR ? frame.peMs : frame.pe;
        SideInfo&                    side = frame.side;

        std::array<std::int32_t, 16>               frameBits;
        std::array<std::array<std::int32_t, 2>, 2> maxBits;
        std::int32_t                               maxResv;

        VbrNewPrepare(config, state, frame, pe, frameBits, maxBits, maxResv);

        for (std::int32_t gr = 0; gr < 2; ++gr) {
            for (std::int32_t ch = 0; ch < config.channels_out; ++ch) {
                if (!frame.search[gr][ch].active)
                    maxBits[gr][ch] = 0; // a silent granule needs no bits
            }
        }

        // quantize the granules with the lowest possible number of bits
        bool               reworked;
        const std::int32_t usedBits = VbrFitFrame(config, constants, side, frame.xrpow, maxBits, frame.search, reworked);
        if (reworked)
            frame.mainDataValid = false; // the granules changed after PrepareMainData
        if (usedBits < 0)
            return false;

        // the lowest bitrate that holds the bits; analog silence may go down to the smallest
        // frame unless the user asked for a hard minimum
        const std::int32_t maxIndex = config.vbr_max_bitrate_index;
        std::int32_t       i        = (frame.analogSilence && !config.enforce_min_bitrate) ? 1 : config.vbr_min_bitrate_index;
        for (; i < maxIndex; ++i) {
            if (usedBits <= frameBits[i])
                break;
        }
        if (i > maxIndex)
            i = maxIndex;
        if (maxResv > 0) {
            // a larger frame if the smaller one would overflow the reservoir
            std::int32_t j = maxIndex;
            for (; j > i; --j) {
                const std::int32_t unused = frameBits[j] - usedBits;
                if (unused <= maxResv)
                    break;
            }
            frame.bitrateIndex = j;
        } else {
            frame.bitrateIndex = i;
        }

        if (usedBits > frameBits[frame.bitrateIndex])
            return false; // should not happen

        std::int32_t meanBits;
        (void)ReservoirFrameBegin(config, state.reservoir, side, frame.bitrateIndex, meanBits);
        for (std::int32_t gr = 0; gr < 2; ++gr) {
            for (std::int32_t ch = 0; ch < config.channels_out; ++ch)
                ReservoirAdjust(state.reservoir, side.tt[gr][ch]);
        }
        ReservoirFrameEnd(state.reservoir, side, meanBits);
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
