// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "SessionConfig.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

#include "Console.h"
#include "Presets.h"

// Port of lame_init_params and its helpers (lame.c), of the masking factor part of
// iteration_init (quantize_pvt.c) and of get_max_frame_buffer_size_by_constraint (bitstream.c).
// The arithmetic keeps LAME's types on purpose (float vs. double, int truncation): the values
// end up in the bitstream, so every rounding has to match.

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    using Shared::Console;
    using Shared::Stream;

    namespace {

        // LAME: filter_coef() (lame.c)
        float FilterCoef(float x)
        {
            if (x > 1.0)
                return 0.0;
            if (x <= 0.0)
                return 1.0;

            return static_cast<float>(std::cos(PI / 2 * x));
        }

        // LAME: lame_init_params_ppflt() (lame.c)
        void InitParamsPolyphaseFilter(SessionConfig& cfg)
        {
            std::int32_t band, maxband, minband;
            float        freq;
            std::int32_t lowpass_band  = 32;
            std::int32_t highpass_band = -1;

            if (cfg.lowpass1 > 0) {
                minband = 999;
                for (band = 0; band <= 31; band++) {
                    freq = static_cast<float>(band / 31.0);
                    // this band and above will be zeroed:
                    if (freq >= cfg.lowpass2)
                        lowpass_band = Min(lowpass_band, band);
                    if (cfg.lowpass1 < freq && freq < cfg.lowpass2)
                        minband = Min(minband, band);
                }

                // compute the *actual* transition band implemented by the polyphase filter
                if (minband == 999)
                    cfg.lowpass1 = static_cast<float>((lowpass_band - .75) / 31.0);
                else
                    cfg.lowpass1 = static_cast<float>((minband - .75) / 31.0);
                cfg.lowpass2 = static_cast<float>(lowpass_band / 31.0);
            }

            // make sure highpass filter is within 90% of what the effective highpass frequency will be
            if (cfg.highpass2 > 0) {
                if (cfg.highpass2 < .9 * (.75 / 31.0)) {
                    cfg.highpass1 = 0;
                    cfg.highpass2 = 0;
                    Console::Print(Stream::Error, L"Warning: highpass filter disabled.  highpass frequency too small\n");
                }
            }

            if (cfg.highpass2 > 0) {
                maxband = -1;
                for (band = 0; band <= 31; band++) {
                    freq = static_cast<float>(band / 31.0);
                    // this band and below will be zereod
                    if (freq <= cfg.highpass1)
                        highpass_band = Max(highpass_band, band);
                    if (cfg.highpass1 < freq && freq < cfg.highpass2)
                        maxband = Max(maxband, band);
                }
                // compute the *actual* transition band implemented by the polyphase filter
                cfg.highpass1 = static_cast<float>(highpass_band / 31.0);
                if (maxband == -1)
                    cfg.highpass2 = static_cast<float>((highpass_band + .75) / 31.0);
                else
                    cfg.highpass2 = static_cast<float>((maxband + .75) / 31.0);
            }

            for (band = 0; band < 32; band++) {
                float fc1, fc2;
                freq = band / 31.0f;
                if (cfg.highpass2 > cfg.highpass1)
                    fc1 = FilterCoef(static_cast<float>((cfg.highpass2 - freq) / (cfg.highpass2 - cfg.highpass1 + 1e-20)));
                else
                    fc1 = 1.0f;
                if (cfg.lowpass2 > cfg.lowpass1)
                    fc2 = FilterCoef(static_cast<float>((freq - cfg.lowpass1) / (cfg.lowpass2 - cfg.lowpass1 + 1e-20)));
                else
                    fc2 = 1.0f;
                cfg.amp_filter[band] = fc1 * fc2;
            }
        }

        // lame_init_qval: internal feature flags from the quality setting.
        void InitQval(EncoderOptions& gfp, SessionConfig& cfg)
        {
            switch (gfp.quality) {
                default:
                case 9: // no psymodel, no noise shaping
                    cfg.noise_shaping    = 0;
                    cfg.use_best_huffman = 0;
                    cfg.full_outer_loop  = 0;
                    break;

                case 8: gfp.quality = 7; [[fallthrough]];
                case 7: // use psymodel (for short block and m/s switching), but no noise shapping
                    cfg.noise_shaping    = 0;
                    cfg.use_best_huffman = 0;
                    cfg.full_outer_loop  = -1;
                    break;

                case 6:
                case 5:
                    if (cfg.noise_shaping == 0)
                        cfg.noise_shaping = 1;
                    if (cfg.subblock_gain == -1)
                        cfg.subblock_gain = 1;
                    cfg.use_best_huffman = 0;
                    cfg.full_outer_loop  = 0;
                    break;

                case 4:
                case 3:
                    if (cfg.noise_shaping == 0)
                        cfg.noise_shaping = 1;
                    if (cfg.subblock_gain == -1)
                        cfg.subblock_gain = 1;
                    cfg.use_best_huffman = 1;
                    cfg.full_outer_loop  = 0;
                    break;

                case 2:
                case 1:
                    if (cfg.noise_shaping == 0)
                        cfg.noise_shaping = 1;
                    if (cfg.substep_shaping == 0)
                        cfg.substep_shaping = 2;
                    if (cfg.subblock_gain == -1)
                        cfg.subblock_gain = 1;
                    cfg.use_best_huffman = 1; // inner loop
                    cfg.full_outer_loop  = 0;
                    break;

                case 0:
                    if (cfg.noise_shaping == 0)
                        cfg.noise_shaping = 1;
                    if (cfg.substep_shaping == 0)
                        cfg.substep_shaping = 2;
                    if (cfg.subblock_gain == -1)
                        cfg.subblock_gain = 1;
                    cfg.use_best_huffman = 1; // type 2 disabled because of it slowness, in favor of full outer loop search
                    cfg.full_outer_loop  = 1;
                    break;
            }
        }

        // LAME: linear_int() (lame.c)
        double LinearInt(double a, double b, double m)
        {
            return a + m * (b - a);
        }

        const std::array<std::array<float, 4>, 2> PayloadLong  = {{{-0.000f, -0.000f, -0.000f, +0.000f}, {-0.500f, -0.250f, -0.025f, +0.500f}}};
        const std::array<std::array<float, 4>, 2> PayloadShort = {{{-0.000f, -0.000f, -0.000f, +0.000f}, {-2.000f, -1.000f, -0.050f, +0.500f}}};

        // The masking factor part of iteration_init.
        void InitMaskingFactors(SessionConfig& cfg)
        {
            float              adjust, db;
            std::int32_t       i;
            const std::int32_t sel = 1; // all modes like vbr-new

            // long
            db     = cfg.adjust_bass_db + PayloadLong[sel][0];
            adjust = powf(10.f, db * 0.1f);
            for (i = 0; i <= 6; ++i)
                cfg.longfact[i] = adjust;
            db     = cfg.adjust_alto_db + PayloadLong[sel][1];
            adjust = powf(10.f, db * 0.1f);
            for (; i <= 13; ++i)
                cfg.longfact[i] = adjust;
            db     = cfg.adjust_treble_db + PayloadLong[sel][2];
            adjust = powf(10.f, db * 0.1f);
            for (; i <= 20; ++i)
                cfg.longfact[i] = adjust;
            db     = cfg.adjust_sfb21_db + PayloadLong[sel][3];
            adjust = powf(10.f, db * 0.1f);
            for (; i < SBMAX_l; ++i)
                cfg.longfact[i] = adjust;

            // short
            db     = cfg.adjust_bass_db + PayloadShort[sel][0];
            adjust = powf(10.f, db * 0.1f);
            for (i = 0; i <= 2; ++i)
                cfg.shortfact[i] = adjust;
            db     = cfg.adjust_alto_db + PayloadShort[sel][1];
            adjust = powf(10.f, db * 0.1f);
            for (; i <= 6; ++i)
                cfg.shortfact[i] = adjust;
            db     = cfg.adjust_treble_db + PayloadShort[sel][2];
            adjust = powf(10.f, db * 0.1f);
            for (; i <= 11; ++i)
                cfg.shortfact[i] = adjust;
            db     = cfg.adjust_sfb21_db + PayloadShort[sel][3];
            adjust = powf(10.f, db * 0.1f);
            for (; i < SBMAX_s; ++i)
                cfg.shortfact[i] = adjust;
        }

    } // namespace

    std::int32_t InitParams(EncoderOptions& gfp, SessionConfig& cfg)
    {
        std::int32_t i;
        std::int32_t j;

        cfg = SessionConfig{};

        if (gfp.samplerateIn < 1)
            return -1; // input sample rate makes no sense
        if (gfp.numChannels < 1 || 2 < gfp.numChannels)
            return -1; // number of input channels makes no sense
        // The input is never resampled, so it has to come at an MPEG-1 sample rate, which is
        // also the output sample rate.
        if (SmpFrqIndex(gfp.samplerateIn) < 0)
            return -1;
        gfp.samplerateOut = gfp.samplerateIn;

        cfg.enforce_min_bitrate = gfp.vbrHardMin;

        // the -b value as given, before it is rounded to a table bitrate below
        cfg.avg_bitrate = gfp.vbrMinBitrateKbps;

        cfg.error_protection = gfp.errorProtection;
        cfg.copyright        = gfp.copyright;
        cfg.original         = gfp.original;
        cfg.extension        = gfp.extension;
        cfg.emphasis         = gfp.emphasis;

        cfg.channels_in = gfp.numChannels;
        if (cfg.channels_in == 1)
            gfp.mode = Mono;
        cfg.channels_out = (gfp.mode == Mono) ? 1 : 2;
        if (gfp.mode != JointStereo)
            gfp.forceMs = 0; // forced mid/side stereo for j-stereo only
        cfg.force_ms = gfp.forceMs;

        // mapping VBR scale to internal VBR quality settings; only the 32 kHz row still applies
        {
            const float qval = gfp.vbrQ + gfp.vbrQFrac;
            struct QMap {
                std::int32_t sr_a;
                float        qa, qb, ta, tb;
                std::int32_t lp;
            };
            const std::array<QMap, 3> m = {{
                {48000, 0.0f, 6.5f, 0.0f, 6.5f, 23700},
                {44100, 0.0f, 6.5f, 0.0f, 6.5f, 21780},
                {32000, 6.5f, 8.0f, 5.2f, 6.5f, 15800},
            }};
            for (i = 2; i < 3; ++i) {
                if (gfp.samplerateIn == m[i].sr_a) {
                    if (qval < m[i].qa) {
                        double d     = qval / m[i].qa;
                        d            = d * m[i].ta;
                        gfp.vbrQ     = static_cast<std::int32_t>(d);
                        gfp.vbrQFrac = static_cast<float>(d - gfp.vbrQ);
                    }
                }
                if (gfp.samplerateIn >= m[i].sr_a) {
                    if (m[i].qa <= qval && qval < m[i].qb) {
                        if (gfp.samplerateIn == m[i].sr_a) {
                            const float q_ = m[i].qb - m[i].qa;
                            const float t_ = m[i].tb - m[i].ta;
                            double      d  = m[i].ta + t_ * (qval - m[i].qa) / q_;
                            gfp.vbrQ       = static_cast<std::int32_t>(d);
                            gfp.vbrQFrac   = static_cast<float>(d - gfp.vbrQ);
                            if (gfp.lowpassFreq == 0)
                                gfp.lowpassFreq = -1;
                        }
                        break;
                    }
                }
            }
        }

        // if a filter has not been enabled, see if we should add one
        if (gfp.lowpassFreq == 0) {
            double                             lowpass = 16000;
            const std::array<std::int32_t, 11> x       = {24000, 19500, 18500, 18000, 17500, 17000, 16500, 15600, 15200, 7230, 3950};
            if (0 <= gfp.vbrQ && gfp.vbrQ <= 9) {
                double a = x[gfp.vbrQ], b = x[gfp.vbrQ + 1], m = gfp.vbrQFrac;
                lowpass = LinearInt(a, b, m);
            } else {
                lowpass = 21500;
            }
            gfp.lowpassFreq = static_cast<std::int32_t>(lowpass);
        }

        gfp.lowpassFreq = Min(24000, gfp.lowpassFreq);
        gfp.lowpassFreq = Min(gfp.samplerateOut / 2, gfp.lowpassFreq);

        cfg.disable_reservoir = gfp.disableReservoir;
        cfg.lowpassfreq       = gfp.lowpassFreq;
        cfg.highpassfreq      = gfp.highpassFreq;
        cfg.samplerate_in     = gfp.samplerateIn;
        cfg.samplerate_out    = gfp.samplerateOut;

        // mode not set by the user and more than one input channel: joint stereo
        if (gfp.mode == NotSet)
            gfp.mode = JointStereo;

        cfg.mode = gfp.mode;

        // apply user driven high pass filter
        if (cfg.highpassfreq > 0) {
            cfg.highpass1 = static_cast<float>(2. * cfg.highpassfreq);

            if (gfp.highpassWidth >= 0)
                cfg.highpass2 = static_cast<float>(2. * (cfg.highpassfreq + gfp.highpassWidth));
            else // 0% above on default
                cfg.highpass2 = static_cast<float>((1 + 0.00) * 2. * cfg.highpassfreq);

            cfg.highpass1 /= cfg.samplerate_out;
            cfg.highpass2 /= cfg.samplerate_out;
        } else {
            cfg.highpass1 = 0;
            cfg.highpass2 = 0;
        }
        // apply user driven low pass filter
        cfg.lowpass1 = 0;
        cfg.lowpass2 = 0;
        if (cfg.lowpassfreq > 0 && cfg.lowpassfreq < (cfg.samplerate_out / 2)) {
            cfg.lowpass2 = static_cast<float>(2. * cfg.lowpassfreq);
            if (gfp.lowpassWidth >= 0) {
                cfg.lowpass1 = static_cast<float>(2. * (cfg.lowpassfreq - gfp.lowpassWidth));
                if (cfg.lowpass1 < 0) // has to be >= 0
                    cfg.lowpass1 = 0;
            } else { // 0% below on default
                cfg.lowpass1 = static_cast<float>((1 - 0.00) * 2. * cfg.lowpassfreq);
            }
            cfg.lowpass1 /= cfg.samplerate_out;
            cfg.lowpass2 /= cfg.samplerate_out;
        }

        InitParamsPolyphaseFilter(cfg);

        // samplerate and scalefactor bands
        cfg.samplerate_index = SmpFrqIndex(cfg.samplerate_out);
        assert(cfg.samplerate_index >= 0);

        j = cfg.samplerate_index;
        for (i = 0; i < SBMAX_l + 1; i++)
            cfg.scalefac_band.l[i] = SfBandIndex[j].l[i];

        for (i = 0; i < PSFB21 + 1; i++) {
            const std::int32_t size     = (cfg.scalefac_band.l[22] - cfg.scalefac_band.l[21]) / PSFB21;
            const std::int32_t start    = cfg.scalefac_band.l[21] + i * size;
            cfg.scalefac_band.psfb21[i] = start;
        }
        cfg.scalefac_band.psfb21[PSFB21] = 576;

        for (i = 0; i < SBMAX_s + 1; i++)
            cfg.scalefac_band.s[i] = SfBandIndex[j].s[i];

        for (i = 0; i < PSFB12 + 1; i++) {
            const std::int32_t size     = (cfg.scalefac_band.s[13] - cfg.scalefac_band.s[12]) / PSFB12;
            const std::int32_t start    = cfg.scalefac_band.s[12] + i * size;
            cfg.scalefac_band.psfb12[i] = start;
        }
        cfg.scalefac_band.psfb12[PSFB12] = 192;

        // determine the mean bitrate for main data
        cfg.sideinfo_len = (cfg.channels_out == 1) ? 4 + 17 : 4 + 32;

        if (cfg.error_protection)
            cfg.sideinfo_len += 2;

        if (gfp.athType == -1)
            gfp.athType = 4;

        assert(gfp.vbrQ <= 9);
        assert(gfp.vbrQ >= 0);

        {
            if (gfp.strictIso < 0)
                gfp.strictIso = MdbMaximum;
            if (gfp.useTemporal < 0)
                gfp.useTemporal = 0; // off by default for this VBR mode

            (void)ApplyPreset(gfp, 500 - (gfp.vbrQ * 10), 0);
            cfg.minval      = gfp.presetMinval;
            cfg.ATHfixpoint = gfp.presetAthFixpoint;
            // The newer VBR code supports only a limited subset of quality levels:
            // 9-5=5 are the same, uses x^3/4 quantization
            // 4-0=0 are the same  5 plus best huffman divide code
            if (gfp.quality < 0)
                gfp.quality = LameDefaultQuality;
            if (gfp.quality < 5)
                gfp.quality = 0;
            if (gfp.quality > 7)
                gfp.quality = 7;

            // sfb21 extra only with MPEG-1 at higher sampling rates
            if (gfp.experimentalY)
                cfg.sfb21_extra = 0;
            else
                cfg.sfb21_extra = (cfg.samplerate_out > 44000);
        }

        // initialize default values common for all modes
        cfg.mask_adjust       = gfp.maskingAdjust;
        cfg.mask_adjust_short = gfp.maskingAdjustShort;

        {                                   // choose a min/max bitrate for VBR
            cfg.vbr_min_bitrate_index = 1;  // default: allow  32 kbps
            cfg.vbr_max_bitrate_index = 14; // default: allow 320 kbps
            if (gfp.vbrMinBitrateKbps) {
                gfp.vbrMinBitrateKbps     = FindNearestBitrate(gfp.vbrMinBitrateKbps);
                cfg.vbr_min_bitrate_index = BitrateIndex(gfp.vbrMinBitrateKbps);
                if (cfg.vbr_min_bitrate_index < 0) {
                    assert(false);
                    cfg.vbr_min_bitrate_index = 1;
                }
            }
            if (gfp.vbrMaxBitrateKbps) {
                gfp.vbrMaxBitrateKbps     = FindNearestBitrate(gfp.vbrMaxBitrateKbps);
                cfg.vbr_max_bitrate_index = BitrateIndex(gfp.vbrMaxBitrateKbps);
                if (cfg.vbr_max_bitrate_index < 0) {
                    assert(false);
                    cfg.vbr_max_bitrate_index = 14;
                }
            }
            gfp.vbrMinBitrateKbps = BitrateTable[cfg.vbr_min_bitrate_index];
            gfp.vbrMaxBitrateKbps = BitrateTable[cfg.vbr_max_bitrate_index];
        }

        cfg.preset           = gfp.preset;
        cfg.write_lame_tag   = gfp.writeLameTag;
        cfg.substep_shaping  = gfp.substepShaping;
        cfg.noise_shaping    = gfp.noiseShaping;
        cfg.subblock_gain    = gfp.subblockGain;
        cfg.use_best_huffman = gfp.useBestHuffman;

        // initialize internal qval settings
        InitQval(gfp, cfg);

        // automatic ATH adjustment on
        if (gfp.athaaType < 0)
            cfg.ath_use_adjust = 3;
        else
            cfg.ath_use_adjust = gfp.athaaType;

        // initialize internal adaptive ATH settings  -jd
        cfg.ath_aa_sensitivity_p = static_cast<float>(std::pow(10.0, gfp.athaaSensitivity / -10.0));

        if (gfp.shortBlocks == ShortBlockNotSet)
            gfp.shortBlocks = ShortBlockAllowed;

        // Many hardware decoders cannot handle uncoupled short blocks in regular stereo mode;
        // coupling makes no sense for mono and dual-mono streams.
        if (gfp.shortBlocks == ShortBlockAllowed && (cfg.mode == JointStereo || cfg.mode == Stereo))
            gfp.shortBlocks = ShortBlockCoupled;

        cfg.short_blocks = gfp.shortBlocks;

        if (gfp.msfix < 0)
            gfp.SetMsfix(0);

        // select psychoacoustic model
        (void)gfp.SetExpNspsytune(gfp.expNspsytune | 1);

        if (gfp.athType < 0)
            gfp.athType = 4;

        if (gfp.athCurve < 0)
            gfp.athCurve = 4;

        if (gfp.interChRatio < 0)
            gfp.interChRatio = 0;

        if (gfp.useTemporal < 0)
            gfp.useTemporal = 1; // on by default

        cfg.interChRatio      = gfp.interChRatio;
        cfg.msfix             = gfp.msfix;
        cfg.ATH_offset_db     = 0 - gfp.athLowerDb;
        cfg.ATH_offset_factor = powf(10.f, cfg.ATH_offset_db * 0.1f);
        cfg.ATHcurve          = gfp.athCurve;
        cfg.ATHtype           = gfp.athType;
        cfg.ATHonly           = gfp.athOnly;
        cfg.ATHshort          = gfp.athShort;
        cfg.noATH             = gfp.noAth;

        cfg.use_temporal_masking_effect = gfp.useTemporal;
        if (cfg.mode == JointStereo)
            cfg.use_safe_joint_stereo = gfp.expNspsytune & 2;
        else
            cfg.use_safe_joint_stereo = 0;
        {
            cfg.adjust_bass_db = static_cast<float>((gfp.expNspsytune >> 2) & 63);
            if (cfg.adjust_bass_db >= 32.f)
                cfg.adjust_bass_db -= 64.f;
            cfg.adjust_bass_db *= 0.25f;

            cfg.adjust_alto_db = static_cast<float>((gfp.expNspsytune >> 8) & 63);
            if (cfg.adjust_alto_db >= 32.f)
                cfg.adjust_alto_db -= 64.f;
            cfg.adjust_alto_db *= 0.25f;

            cfg.adjust_treble_db = static_cast<float>((gfp.expNspsytune >> 14) & 63);
            if (cfg.adjust_treble_db >= 32.f)
                cfg.adjust_treble_db -= 64.f;
            cfg.adjust_treble_db *= 0.25f;

            // to be compatible with Naoki's original code, the next 6 bits define only the
            // amount of changing treble for sfb21
            cfg.adjust_sfb21_db = static_cast<float>((gfp.expNspsytune >> 20) & 63);
            if (cfg.adjust_sfb21_db >= 32.f)
                cfg.adjust_sfb21_db -= 64.f;
            cfg.adjust_sfb21_db *= 0.25f;
            cfg.adjust_sfb21_db += cfg.adjust_treble_db;
        }

        // PCM input transform matrix: user defined re-scaling and two-to-one channel downmix
        {
            std::array<std::array<float, 2>, 2> m = {{{1.0f, 0.0f}, {0.0f, 1.0f}}};

            // user selected scaling of the samples
            m[0][0] *= gfp.scale;
            m[0][1] *= gfp.scale;
            m[1][0] *= gfp.scale;
            m[1][1] *= gfp.scale;
            // user selected scaling of the channel 0 (left) samples
            m[0][0] *= gfp.scaleLeft;
            m[0][1] *= gfp.scaleLeft;
            // user selected scaling of the channel 1 (right) samples
            m[1][0] *= gfp.scaleRight;
            m[1][1] *= gfp.scaleRight;
            // Downsample to Mono if 2 channels in and 1 channel out
            if (cfg.channels_in == 2 && cfg.channels_out == 1) {
                m[0][0] = 0.5f * (m[0][0] + m[1][0]);
                m[0][1] = 0.5f * (m[0][1] + m[1][1]);
                m[1][0] = 0;
                m[1][1] = 0;
            }
            cfg.pcm_transform[0][0] = m[0][0];
            cfg.pcm_transform[0][1] = m[0][1];
            cfg.pcm_transform[1][0] = m[1][0];
            cfg.pcm_transform[1][1] = m[1][1];
        }

        InitMaskingFactors(cfg);

        cfg.buffer_constraint = GetMaxFrameBufferSizeByConstraint(cfg, gfp.strictIso);

        cfg.options = gfp;
        return 0;
    }

    std::int32_t CalcFrameLength(const SessionConfig& config, std::int32_t kbps, std::int32_t pad) noexcept
    {
        return 8 * (2 * 72000 * kbps / config.samplerate_out + pad);
    }

    std::int32_t GetMaxFrameBufferSizeByConstraint(const SessionConfig& config, std::int32_t constraint) noexcept
    {
        std::int32_t maxmp3buf = 0;
        if (config.avg_bitrate > 320) {
            // in freeformat the buffer is constant
            if (constraint == MdbStrictIso)
                maxmp3buf = CalcFrameLength(config, config.avg_bitrate, 0);
            else
                maxmp3buf = 7680 * 2; // maximum allowed bits per granule are 7680
        } else {
            const std::int32_t maxKbps = BitrateTable[14];
            switch (constraint) {
                default:
                case MdbDefault:
                    // All mp3 decoders should have enough buffer to handle this value: size of a 320kbps 32kHz frame
                    maxmp3buf = 8 * 1440;
                    break;
                case MdbStrictIso: maxmp3buf = CalcFrameLength(config, maxKbps, 0); break;
                case MdbMaximum: maxmp3buf = 7680 * 2; break;
            }
        }
        return maxmp3buf;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
