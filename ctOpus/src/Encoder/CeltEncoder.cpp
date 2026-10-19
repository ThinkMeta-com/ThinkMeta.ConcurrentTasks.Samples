// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "CeltEncoder.h"
#include "OpusDefines.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "CeltBands.h"
#include "CeltEnergy.h"
#include "CeltMath.h"
#include "CeltMdct.h"
#include "CeltPitch.h"
#include "CeltRate.h"
#include "CeltSimd.h"
#include "IntMath.h"
#include "RangeEncoder.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    namespace {

        // celt_encoder.c does not see the M_PI of math.h (no _USE_MATH_DEFINES) and uses its own
        constexpr double Pi = 3.141592653;

        constexpr std::int32_t LeakBandsCelt = 19;
        constexpr std::int32_t PacketSizeCap = 1275;

        constexpr std::int8_t TfSelectTable[4][8] = {
            // isTransient=0  isTransient=1
            {0, -1, 0, -1, 0, -1, 0, -1}, // 2.5 ms
            {0, -1, 0, -2, 1, 0, 1, -1},  // 5 ms
            {0, -2, 0, -3, 2, 0, 1, -1},  // 10 ms
            {0, -2, 0, -3, 3, 0, 1, -1},  // 20 ms
        };

        constexpr std::uint8_t TrimIcdf[11]  = {126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};
        constexpr std::uint8_t SpreadIcdf[4] = {25, 23, 2, 0};
        constexpr std::uint8_t TapsetIcdf[3] = {2, 1, 0};

        constexpr float IntensityThresholds[21] = {1, 2, 3, 4, 5, 6, 7, 8, 16, 24, 36, 44, 50, 56, 62, 67, 72, 79, 88, 106, 134};
        constexpr float IntensityHysteresis[21] = {1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 4, 5, 6, 8, 8};

        // libopus: bitrate_to_bits() (celt/celt.h)
        constexpr std::int32_t BitrateToBits(std::int32_t bitrate, std::int32_t fs, std::int32_t frameSize) noexcept
        {
            return bitrate * 6 / (6 * fs / frameSize);
        }

        // libopus: celt_sqrt(), celt_log2(), celt_exp2() (celt/mathops.h) of the float build without
        // FLOAT_APPROX: double libm
        inline float SqrtD(double x) noexcept
        {
            return static_cast<float>(std::sqrt(x));
        }
        inline float Log2D(float x) noexcept
        {
            return static_cast<float>(1.442695040888963387 * std::log(static_cast<double>(x)));
        }
        inline float Exp2D(float x) noexcept
        {
            return static_cast<float>(std::exp(0.6931471805599453094 * static_cast<double>(x)));
        }

        // libopus: resampling_factor() (celt/celt.c)
        std::int32_t ResamplingFactor(std::int32_t rate) noexcept
        {
            switch (rate) {
                case 48000: return 1;
                case 24000: return 2;
                case 16000: return 3;
                case 12000: return 4;
                case 8000: return 6;
                default: return 0;
            }
        }

        // libopus: transient_analysis() (celt/celt_encoder.c)
        bool TransientAnalysis(const float* in, std::int32_t len, std::int32_t c, float& tfEstimate, std::int32_t& tfChan, bool allowWeakTransients,
                               bool& weakTransient, float toneFreq, float toneishness) noexcept
        {
            // 6*64/x, trained on real data to minimise the average error
            static constexpr std::uint8_t invTable[128] = {
                255, 255, 156, 110, 86, 70, 59, 51, 45, 40, 37, 33, 31, 28, 26, 25, 23, 22, 21, 20, 19, 18, 17, 16, 16, 15, 15, 14, 13, 13, 12, 12,
                12,  12,  11,  11,  11, 10, 10, 10, 9,  9,  9,  9,  9,  9,  8,  8,  8,  8,  8,  7,  7,  7,  7,  7,  7,  6,  6,  6,  6,  6,  6,  6,
                6,   6,   6,   6,   6,  6,  6,  6,  6,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
                4,   4,   4,   4,   4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  2,
            };
            float        tmp[960 + CeltEncoder::Overlap];
            std::int32_t maskMetric = 0;
            // forward masking: 6.7 dB/ms; at low rates 3.3 dB/ms, against coding transients that
            // could make the energy unstable or collapse partially
            const float forwardDecay = allowWeakTransients ? .03125f : .0625f;

            weakTransient           = false;
            const std::int32_t len2 = len / 2;
            for (std::int32_t ch = 0; ch < c; ++ch) {
                float mem0 = 0;
                float mem1 = 0;
                // high-pass filter (1 - 2*z^-1 + z^-2) / (1 - z^-1 + .5*z^-2), with short dependency chains
                for (std::int32_t i = 0; i < len; ++i) {
                    const float x     = in[i + ch * len];
                    const float y     = mem0 + x;
                    const float mem00 = mem0;
                    mem0              = mem0 - x + .5f * mem1;
                    mem1              = x - mem00;
                    tmp[i]            = y;
                }
                // the first samples are bad because the memory is not propagated
                std::memset(tmp, 0, 12 * sizeof(float));

                float mean = 0;
                mem0       = 0;
                // grouped by two; forward pass for the post-echo threshold
                for (std::int32_t i = 0; i < len2; ++i) {
                    const float x2 = tmp[2 * i] * tmp[2 * i] + tmp[2 * i + 1] * tmp[2 * i + 1];
                    mean += x2;
                    mem0   = x2 + (1.f - forwardDecay) * mem0;
                    tmp[i] = forwardDecay * mem0;
                }

                mem0       = 0;
                float maxE = 0;
                // backward pass for the pre-echo threshold (backward masking 13.9 dB/ms)
                for (std::int32_t i = len2 - 1; i >= 0; --i) {
                    mem0   = tmp[i] + 0.875f * mem0;
                    tmp[i] = 0.125f * mem0;
                    maxE   = Max32(maxE, 0.125f * mem0);
                }

                // frame energy: the geometric mean of the energy and half the max
                mean = SqrtD(mean * maxE * .5 * len2);
                // the inverse of the mean energy
                const float norm = len2 / (1e-15f + mean);
                // harmonic mean, without the unreliable boundaries; the data is smooth, so 1/4 of
                // the samples suffices
                std::int32_t unmask = 0;
                for (std::int32_t i = 12; i < len2 - 5; i += 4) {
                    const double v  = std::floor(64 * norm * (tmp[i] + 1e-15f)); // do not round to nearest
                    const double v1 = 127 < v ? 127 : v;
                    const auto   id = static_cast<std::int32_t>(0 > v1 ? 0 : v1);
                    unmask += invTable[id];
                }
                // normalise for the 1/4 of the samples and the factor 6 of the table
                unmask = 64 * unmask * 4 / (6 * (len2 - 17));
                if (unmask > maskMetric) {
                    tfChan     = ch;
                    maskMetric = unmask;
                }
            }
            bool isTransient = maskMetric > 200;
            // the partial cycle of a very low tone is no transient
            if (toneishness > .98f && toneFreq < 0.026f) {
                isTransient = false;
                maskMetric  = 0;
            }
            // weak transients at low rates, handled differently against partial collapse
            if (allowWeakTransients && isTransient && maskMetric < 600) {
                isTransient   = false;
                weakTransient = true;
            }
            // an arbitrary metric for the VBR boost
            const float tfMax = Max32(0, SqrtD(27 * maskMetric) - 42);
            // tf_estimate = 1 + MIN16(1, sqrt(MAX16(0, tf_max-30))/20)
            const double t = static_cast<double>(static_cast<float>(0.0069) * Min32(163, tfMax)) - 0.139;
            tfEstimate     = SqrtD(0 > t ? 0 : t);
            return isTransient;
        }

        // libopus: patch_transient_decision() (celt/celt_encoder.c)
        // sudden increases of energy that patch the transient decision
        bool PatchTransientDecision(const float* newE, const float* oldE, std::int32_t nbEBands, std::int32_t start, std::int32_t end, std::int32_t c) noexcept
        {
            float spreadOld[26];
            // an aggressive (-6 dB/Bark) spreading function on the old frame against false
            // detections from irrelevant bands
            if (c == 1) {
                spreadOld[start] = oldE[start];
                for (std::int32_t i = start + 1; i < end; ++i)
                    spreadOld[i] = Max32(spreadOld[i - 1] - 1.0f, oldE[i]);
            } else {
                spreadOld[start] = Max32(oldE[start], oldE[start + nbEBands]);
                for (std::int32_t i = start + 1; i < end; ++i)
                    spreadOld[i] = Max32(spreadOld[i - 1] - 1.0f, Max32(oldE[i], oldE[i + nbEBands]));
            }
            for (std::int32_t i = end - 2; i >= start; --i)
                spreadOld[i] = Max32(spreadOld[i], spreadOld[i + 1] - 1.0f);
            // the mean increase
            float meanDiff = 0;
            for (std::int32_t ch = 0; ch < c; ++ch) {
                for (std::int32_t i = Imax(2, start); i < end - 1; ++i) {
                    const float x1 = Max32(0, newE[i + ch * nbEBands]);
                    const float x2 = Max32(0, spreadOld[i]);
                    meanDiff       = meanDiff + Max32(0, x1 - x2);
                }
            }
            meanDiff = meanDiff / static_cast<float>(c * (end - 1 - Imax(2, start)));
            return meanDiff > 1.f;
        }

        // libopus: compute_mdcts() (celt/celt_encoder.c)
        // the window and the MDCTs of all sub-frames and channels
        void ComputeMdcts(const CeltMode::View& mode, std::int32_t shortBlocks, const float* in, float* out, std::int32_t c, std::int32_t cc, std::int32_t lm,
                          std::int32_t upsample) noexcept
        {
            const std::int32_t overlap = mode.overlap;
            std::int32_t       b       = 1;
            std::int32_t       n       = mode.shortMdctSize << lm;
            std::int32_t       shift   = mode.maxLM - lm;
            if (shortBlocks != 0) {
                b     = shortBlocks;
                n     = mode.shortMdctSize;
                shift = mode.maxLM;
            }
            for (std::int32_t ch = 0; ch < cc; ++ch) {
                // the sub-frames interleaved while doing the MDCTs
                for (std::int32_t k = 0; k < b; ++k)
                    MdctForward(in + ch * (b * n + overlap) + k * n, &out[k + ch * n * b], shift, b);
            }
            if (cc == 2 && c == 1) {
                for (std::int32_t i = 0; i < b * n; ++i)
                    out[i] = .5f * out[i] + .5f * out[b * n + i];
            }
            if (upsample != 1) {
                for (std::int32_t ch = 0; ch < c; ++ch) {
                    const std::int32_t bound = b * n / upsample;
                    for (std::int32_t i = 0; i < bound; ++i)
                        out[ch * b * n + i] *= upsample;
                    std::memset(&out[ch * b * n + bound], 0, static_cast<std::size_t>(b * n - bound) * sizeof(float));
                }
            }
        }

        // libopus: celt_preemphasis() (celt/celt_encoder.c) for the static mode (coef[1] == 0)
        void Preemphasis(const float* pcmp, float* inp, std::int32_t n, std::int32_t cc, std::int32_t upsample, const float* coef, float* mem,
                         bool clip) noexcept
        {
            const float coef0 = coef[0];
            float       m     = *mem;
            // the fast path for 48 kHz without clipping
            if (coef[1] == 0 && upsample == 1 && !clip) {
                for (std::int32_t i = 0; i < n; ++i) {
                    const float x = 32768.f * pcmp[cc * i];
                    inp[i]        = x - m;
                    m             = coef0 * x;
                }
                *mem = m;
                return;
            }
            const std::int32_t nu = n / upsample;
            if (upsample != 1)
                std::memset(inp, 0, static_cast<std::size_t>(n) * sizeof(float));
            for (std::int32_t i = 0; i < nu; ++i)
                inp[i * upsample] = 32768.f * pcmp[cc * i];
            if (clip) {
                // against non-portable files
                for (std::int32_t i = 0; i < nu; ++i)
                    inp[i * upsample] = Max32(-65536.f, Min32(65536.f, inp[i * upsample]));
            }
            for (std::int32_t i = 0; i < n; ++i) {
                const float x = inp[i];
                inp[i]        = x - m;
                m             = coef0 * x;
            }
            *mem = m;
        }

        // libopus: l1_metric() (celt/celt_encoder.c)
        float L1Metric(const float* tmp, std::int32_t n, std::int32_t lm, float bias) noexcept
        {
            float l1 = 0;
            for (std::int32_t i = 0; i < n; ++i)
                l1 += std::fabs(tmp[i]);
            // when in doubt, prefer a good frequency resolution
            return l1 + lm * bias * l1;
        }

        // libopus: tf_analysis() (celt/celt_encoder.c)
        std::int32_t TfAnalysis(const CeltMode::View& m, std::int32_t len, bool isTransient, std::int32_t* tfRes, std::int32_t lambda, const float* x,
                                std::int32_t n0, std::int32_t lm, float tfEstimate, std::int32_t tfChan, const std::int32_t* importance) noexcept
        {
            std::int32_t metric[CeltEncoder::NbEBands];
            std::int32_t path0[CeltEncoder::NbEBands];
            std::int32_t path1[CeltEncoder::NbEBands];
            float        tmp[176];
            float        tmp1[176];
            const float  bias = .04f * Max32(-.25f, .5f - tfEstimate);
            const auto   tr   = static_cast<std::int32_t>(isTransient);

            for (std::int32_t i = 0; i < len; ++i) {
                const std::int32_t n = (m.eBands[i + 1] - m.eBands[i]) << lm;
                // too narrow to be split down to LM=-1
                const bool narrow = (m.eBands[i + 1] - m.eBands[i]) == 1;
                std::memcpy(tmp, &x[tfChan * n0 + (m.eBands[i] << lm)], static_cast<std::size_t>(n) * sizeof(float));
                float        l1        = L1Metric(tmp, n, isTransient ? lm : 0, bias);
                float        bestL1    = l1;
                std::int32_t bestLevel = 0;
                // the -1 case for transients
                if (isTransient && !narrow) {
                    std::memcpy(tmp1, tmp, static_cast<std::size_t>(n) * sizeof(float));
                    Haar1(tmp1, n >> lm, 1 << lm);
                    l1 = L1Metric(tmp1, n, lm + 1, bias);
                    if (l1 < bestL1) {
                        bestL1    = l1;
                        bestLevel = -1;
                    }
                }
                for (std::int32_t k = 0; k < lm + static_cast<std::int32_t>(!(isTransient || narrow)); ++k) {
                    const std::int32_t b = isTransient ? (lm - k - 1) : k + 1;
                    Haar1(tmp, n >> k, 1 << k);
                    l1 = L1Metric(tmp, n, b, bias);
                    if (l1 < bestL1) {
                        bestL1    = l1;
                        bestLevel = k + 1;
                    }
                }
                // in Q1, to select the mid-point (-0.5) for narrower bands
                metric[i] = isTransient ? 2 * bestLevel : -2 * bestLevel;
                // bands that cannot be split to -1 get the half-way point, against a bias
                if (narrow && (metric[i] == 0 || metric[i] == -2 * lm))
                    metric[i] -= 1;
            }
            // the optimal TF resolution, including tf_select
            std::int32_t selcost[2];
            for (std::int32_t sel = 0; sel < 2; ++sel) {
                std::int32_t cost0 = importance[0] * std::abs(metric[0] - 2 * TfSelectTable[lm][4 * tr + 2 * sel + 0]);
                std::int32_t cost1 = importance[0] * std::abs(metric[0] - 2 * TfSelectTable[lm][4 * tr + 2 * sel + 1]) + (isTransient ? 0 : lambda);
                for (std::int32_t i = 1; i < len; ++i) {
                    const std::int32_t curr0 = Imin(cost0, cost1 + lambda);
                    const std::int32_t curr1 = Imin(cost0 + lambda, cost1);
                    cost0                    = curr0 + importance[i] * std::abs(metric[i] - 2 * TfSelectTable[lm][4 * tr + 2 * sel + 0]);
                    cost1                    = curr1 + importance[i] * std::abs(metric[i] - 2 * TfSelectTable[lm][4 * tr + 2 * sel + 1]);
                }
                selcost[sel] = Imin(cost0, cost1);
            }
            // conservative: tf_select=1 only for transients
            std::int32_t tfSelect = 0;
            if (selcost[1] < selcost[0] && isTransient)
                tfSelect = 1;
            std::int32_t cost0 = importance[0] * std::abs(metric[0] - 2 * TfSelectTable[lm][4 * tr + 2 * tfSelect + 0]);
            std::int32_t cost1 = importance[0] * std::abs(metric[0] - 2 * TfSelectTable[lm][4 * tr + 2 * tfSelect + 1]) + (isTransient ? 0 : lambda);
            // Viterbi forward pass
            for (std::int32_t i = 1; i < len; ++i) {
                std::int32_t curr0 = 0;
                std::int32_t curr1 = 0;
                std::int32_t from0 = cost0;
                std::int32_t from1 = cost1 + lambda;
                if (from0 < from1) {
                    curr0    = from0;
                    path0[i] = 0;
                } else {
                    curr0    = from1;
                    path0[i] = 1;
                }
                from0 = cost0 + lambda;
                from1 = cost1;
                if (from0 < from1) {
                    curr1    = from0;
                    path1[i] = 0;
                } else {
                    curr1    = from1;
                    path1[i] = 1;
                }
                cost0 = curr0 + importance[i] * std::abs(metric[i] - 2 * TfSelectTable[lm][4 * tr + 2 * tfSelect + 0]);
                cost1 = curr1 + importance[i] * std::abs(metric[i] - 2 * TfSelectTable[lm][4 * tr + 2 * tfSelect + 1]);
            }
            tfRes[len - 1] = cost0 < cost1 ? 0 : 1;
            // Viterbi backward pass
            for (std::int32_t i = len - 2; i >= 0; --i)
                tfRes[i] = tfRes[i + 1] == 1 ? path1[i + 1] : path0[i + 1];
            return tfSelect;
        }

        // libopus: tf_encode() (celt/celt_encoder.c)
        void TfEncode(std::int32_t start, std::int32_t end, bool isTransient, std::int32_t* tfRes, std::int32_t lm, std::int32_t tfSelect,
                      RangeEncoder* enc) noexcept
        {
            const auto    tr     = static_cast<std::int32_t>(isTransient);
            std::uint32_t budget = enc->Storage() * 8;
            auto          tell   = static_cast<std::uint32_t>(enc->Tell());
            std::uint32_t logp   = isTransient ? 2 : 4;
            // space for the tf_select decision
            const std::uint32_t tfSelectRsv = lm > 0 && tell + logp + 1 <= budget ? 1 : 0;
            budget -= tfSelectRsv;
            std::int32_t curr      = 0;
            std::int32_t tfChanged = 0;
            for (std::int32_t i = start; i < end; ++i) {
                if (tell + logp <= budget) {
                    enc->EncodeBitLogp(tfRes[i] ^ curr, logp);
                    tell = static_cast<std::uint32_t>(enc->Tell());
                    curr = tfRes[i];
                    tfChanged |= curr;
                } else {
                    tfRes[i] = curr;
                }
                logp = isTransient ? 4 : 5;
            }
            // tf_select only if it makes a difference
            if (tfSelectRsv != 0 && TfSelectTable[lm][4 * tr + 0 + tfChanged] != TfSelectTable[lm][4 * tr + 2 + tfChanged])
                enc->EncodeBitLogp(tfSelect, 1);
            else
                tfSelect = 0;
            for (std::int32_t i = start; i < end; ++i)
                tfRes[i] = TfSelectTable[lm][4 * tr + 2 * tfSelect + tfRes[i]];
        }

        // libopus: alloc_trim_analysis() (celt/celt_encoder.c)
        std::int32_t AllocTrimAnalysis(const CeltMode::View& m, const float* x, const float* bandLogE, std::int32_t end, std::int32_t lm, std::int32_t c,
                                       std::int32_t n0, const AnalysisInfo& analysis, float& stereoSaving, float tfEstimate, std::int32_t intensity,
                                       float surroundTrim, std::int32_t equivRate) noexcept
        {
            float trim = 5.f;
            // a lower trim helps at low rates; at higher rates it is less clear, so it stays
            if (equivRate < 64000) {
                trim = 4.f;
            } else if (equivRate < 80000) {
                const std::int32_t frac = (equivRate - 64000) >> 10;
                trim                    = 4.f + 1.f / 16.f * frac;
            }
            if (c == 2) {
                // the inter-channel correlation of the low frequencies
                float sum = 0;
                for (std::int32_t i = 0; i < 8; ++i) {
                    const float partial = InnerProd(&x[m.eBands[i] << lm], &x[n0 + (m.eBands[i] << lm)], (m.eBands[i + 1] - m.eBands[i]) << lm);
                    sum                 = sum + partial;
                }
                sum         = 1.f / 8 * sum;
                sum         = Min32(1.f, std::fabs(sum));
                float minXC = sum;
                for (std::int32_t i = 8; i < intensity; ++i) {
                    const float partial = InnerProd(&x[m.eBands[i] << lm], &x[n0 + (m.eBands[i] << lm)], (m.eBands[i + 1] - m.eBands[i]) << lm);
                    minXC               = Min32(minXC, std::fabs(partial));
                }
                minXC = Min32(1.f, std::fabs(minXC));
                // mid-side savings from the LF average and from the minimum correlation
                const float logXC  = Log2D(1.001f - sum * sum);
                const float logXC2 = Max32(.5f * logXC, Log2D(1.001f - minXC * minXC));
                trim += Max32(-4.f, .75f * logXC);
                stereoSaving = Min32(stereoSaving + 0.25f, -.5f * logXC2);
            }

            // the spectral tilt
            float diff = 0;
            for (std::int32_t ch = 0; ch < c; ++ch) {
                for (std::int32_t i = 0; i < end - 1; ++i)
                    diff += bandLogE[i + ch * m.nbEBands] * static_cast<std::int32_t>(2 + 2 * i - end);
            }
            diff /= c * (end - 1);
            trim -= Max32(-2.f, Min32(2.f, (diff + 1.f) / 6));
            trim -= surroundTrim;
            trim -= 2 * tfEstimate;
            if (analysis.valid != 0)
                trim -= Max32(-2.f, Min32(2.f, static_cast<float>(2.f * (analysis.tonalitySlope + .05f))));
            const auto trimIndex = static_cast<std::int32_t>(std::floor(.5f + trim));
            return Imax(0, Imin(10, trimIndex));
        }

        // libopus: stereo_analysis() (celt/celt_encoder.c)
        bool StereoAnalysis(const CeltMode::View& m, const float* x, std::int32_t lm, std::int32_t n0) noexcept
        {
            float sumLR = 1e-15f;
            float sumMS = 1e-15f;
            // the L1 norm models the entropy of L/R vs M/S
            for (std::int32_t i = 0; i < 13; ++i) {
                for (std::int32_t j = m.eBands[i] << lm; j < m.eBands[i + 1] << lm; ++j) {
                    const float l  = x[j];
                    const float r  = x[n0 + j];
                    const float mm = l + r;
                    const float s  = l - r;
                    sumLR          = sumLR + (std::fabs(l) + std::fabs(r));
                    sumMS          = sumMS + (std::fabs(mm) + std::fabs(s));
                }
            }
            sumMS               = 0.707107f * sumMS;
            std::int32_t thetas = 13;
            // no thetas for the lower bands with LM<=1
            if (lm <= 1)
                thetas -= 8;
            return ((m.eBands[13] << (lm + 1)) + thetas) * sumMS > (m.eBands[13] << (lm + 1)) * sumLR;
        }

        // libopus: median_of_5() (celt/celt_encoder.c)
        float MedianOf5(const float* x) noexcept
        {
            float       t0 = 0;
            float       t1 = 0;
            float       t3 = 0;
            float       t4 = 0;
            const float t2 = x[2];
            if (x[0] > x[1]) {
                t0 = x[1];
                t1 = x[0];
            } else {
                t0 = x[0];
                t1 = x[1];
            }
            if (x[3] > x[4]) {
                t3 = x[4];
                t4 = x[3];
            } else {
                t3 = x[3];
                t4 = x[4];
            }
            if (t0 > t3) {
                float tmp = t0;
                t0        = t3;
                t3        = tmp;
                tmp       = t1;
                t1        = t4;
                t4        = tmp;
            }
            if (t2 > t1) {
                if (t1 < t3)
                    return Min32(t2, t3);
                return Min32(t4, t1);
            }
            if (t2 < t3)
                return Min32(t1, t3);
            return Min32(t2, t4);
        }

        // libopus: median_of_3() (celt/celt_encoder.c)
        float MedianOf3(const float* x) noexcept
        {
            float t0 = 0;
            float t1 = 0;
            if (x[0] > x[1]) {
                t0 = x[1];
                t1 = x[0];
            } else {
                t0 = x[0];
                t1 = x[1];
            }
            const float t2 = x[2];
            if (t1 < t2)
                return t1;
            if (t0 < t2)
                return t2;
            return t0;
        }

        // libopus: dynalloc_analysis() (celt/celt_encoder.c)
        float DynallocAnalysis(const float* bandLogE, const float* bandLogE2, const float* oldBandE, std::int32_t nbEBands, std::int32_t start,
                               std::int32_t end, std::int32_t c, std::int32_t* offsets, std::int32_t lsbDepth, const std::int16_t* logN, bool isTransient,
                               std::int32_t vbr, std::int32_t constrainedVbr, const std::int16_t* eBands, std::int32_t lm, std::int32_t effectiveBytes,
                               std::int32_t& totBoostOut, std::int32_t lfe, const float* surroundDynalloc, const AnalysisInfo& analysis,
                               std::int32_t* importance, std::int32_t* spreadWeight, float toneFreq, float toneishness) noexcept
        {
            constexpr std::int32_t nb = CeltEncoder::NbEBands;
            float                  follower[CeltEncoder::MaxChannels * nb];
            float                  noiseFloor[nb];
            float                  bandLogE3[nb];
            std::int32_t           totBoost = 0;
            std::memset(offsets, 0, static_cast<std::size_t>(nbEBands) * sizeof(std::int32_t));
            // dynamic allocation
            float maxDepth = -31.9f;
            for (std::int32_t i = 0; i < end; ++i) {
                // the noise floor accounts for eMeans, the depth, the band widths and the
                // pre-emphasis (about the square of the Bark band number)
                noiseFloor[i] = 0.0625f * logN[i] + .5f + (9 - lsbDepth) - EMeans[i] + .0062f * (i + 5) * (i + 5);
            }
            for (std::int32_t ch = 0; ch < c; ++ch) {
                for (std::int32_t i = 0; i < end; ++i)
                    maxDepth = Max32(maxDepth, bandLogE[ch * nbEBands + i] - noiseFloor[i]);
            }
            {
                // a really simple masking model, so that completely masked bands do not count for
                // the spreading decision
                float mask[nb];
                float sig[nb];
                for (std::int32_t i = 0; i < end; ++i)
                    mask[i] = bandLogE[i] - noiseFloor[i];
                if (c == 2) {
                    for (std::int32_t i = 0; i < end; ++i)
                        mask[i] = Max32(mask[i], bandLogE[nbEBands + i] - noiseFloor[i]);
                }
                std::memcpy(sig, mask, static_cast<std::size_t>(end) * sizeof(float));
                for (std::int32_t i = 1; i < end; ++i)
                    mask[i] = Max32(mask[i], mask[i - 1] - 2.f);
                for (std::int32_t i = end - 2; i >= 0; --i)
                    mask[i] = Max32(mask[i], mask[i + 1] - 3.f);
                for (std::int32_t i = 0; i < end; ++i) {
                    // SMR: the mask is never more than 72 dB below the peak, never below the noise floor
                    const float        smr   = sig[i] - Max32(Max32(0, maxDepth - 12.f), mask[i]);
                    const std::int32_t shift = Imin(5, Imax(0, -static_cast<std::int32_t>(std::floor(.5f + smr))));
                    spreadWeight[i]          = 32 >> shift;
                }
            }
            // dynamic allocation must not bust the budget: from 24 kb/s for 20 ms frames and
            // 96 kb/s for 2.5 ms frames
            if (effectiveBytes >= (30 + 5 * lm) && lfe == 0) {
                std::int32_t last = 0;
                for (std::int32_t ch = 0; ch < c; ++ch) {
                    std::memcpy(bandLogE3, &bandLogE2[ch * nbEBands], static_cast<std::size_t>(end) * sizeof(float));
                    if (lm == 0) {
                        // in 2.5 ms frames the first 8 bands have one bin each: the max with the
                        // previous energy uses at least 2 bins
                        for (std::int32_t i = 0; i < Imin(8, end); ++i)
                            bandLogE3[i] = Max32(bandLogE2[ch * nbEBands + i], oldBandE[ch * nbEBands + i]);
                    }
                    float* f = &follower[ch * nbEBands];
                    f[0]     = bandLogE3[0];
                    for (std::int32_t i = 1; i < end; ++i) {
                        // the last band at least 3 dB above the previous one is the last
                        // considered (against problems with band-limited signals)
                        if (bandLogE3[i] > bandLogE3[i - 1] + .5f)
                            last = i;
                        f[i] = Min32(f[i - 1] + 1.5f, bandLogE3[i]);
                    }
                    for (std::int32_t i = last - 1; i >= 0; --i)
                        f[i] = Min32(f[i], Min32(f[i + 1] + 2.f, bandLogE3[i]));

                    // a median filter against unnecessary dynalloc; a higher offset uses more bits
                    const float offset = 1.f;
                    for (std::int32_t i = 2; i < end - 2; ++i)
                        f[i] = Max32(f[i], MedianOf5(&bandLogE3[i - 2]) - offset);
                    float tmp  = MedianOf3(&bandLogE3[0]) - offset;
                    f[0]       = Max32(f[0], tmp);
                    f[1]       = Max32(f[1], tmp);
                    tmp        = MedianOf3(&bandLogE3[end - 3]) - offset;
                    f[end - 2] = Max32(f[end - 2], tmp);
                    f[end - 1] = Max32(f[end - 1], tmp);

                    for (std::int32_t i = 0; i < end; ++i)
                        f[i] = Max32(f[i], noiseFloor[i]);
                }
                if (c == 2) {
                    for (std::int32_t i = start; i < end; ++i) {
                        // 24 dB of cross-talk
                        follower[nbEBands + i] = Max32(follower[nbEBands + i], follower[i] - 4.f);
                        follower[i]            = Max32(follower[i], follower[nbEBands + i] - 4.f);
                        follower[i]            = .5f * (Max32(0, bandLogE[i] - follower[i]) + Max32(0, bandLogE[nbEBands + i] - follower[nbEBands + i]));
                    }
                } else {
                    for (std::int32_t i = start; i < end; ++i)
                        follower[i] = Max32(0, bandLogE[i] - follower[i]);
                }
                for (std::int32_t i = start; i < end; ++i)
                    follower[i] = Max32(follower[i], surroundDynalloc[i]);
                for (std::int32_t i = start; i < end; ++i)
                    importance[i] = static_cast<std::int32_t>(std::floor(.5f + 13 * Exp2D(Min32(follower[i], 4.f))));
                // half the dynalloc contribution for non-transient CBR/CVBR frames
                if ((vbr == 0 || constrainedVbr != 0) && !isTransient) {
                    for (std::int32_t i = start; i < end; ++i)
                        follower[i] = .5f * follower[i];
                }
                for (std::int32_t i = start; i < end; ++i) {
                    if (i < 8)
                        follower[i] *= 2;
                    if (i >= 12)
                        follower[i] = .5f * follower[i];
                }
                // compensation for the under-allocation of Opus on tones
                if (toneishness > .98f) {
                    const auto freqBin = static_cast<std::int32_t>(std::floor(.5 + toneFreq * 120 / Pi));
                    for (std::int32_t i = start; i < end; ++i) {
                        if (freqBin >= eBands[i] && freqBin <= eBands[i + 1])
                            follower[i] += 2.f;
                        if (freqBin >= eBands[i] - 1 && freqBin <= eBands[i + 1] + 1)
                            follower[i] += 1.f;
                        if (freqBin >= eBands[i] - 2 && freqBin <= eBands[i + 1] + 2)
                            follower[i] += 1.f;
                        if (freqBin >= eBands[i] - 3 && freqBin <= eBands[i + 1] + 3)
                            follower[i] += .5f;
                    }
                    if (freqBin >= eBands[end]) {
                        follower[end - 1] += 2.f;
                        follower[end - 2] += 1.f;
                    }
                }
                if (analysis.valid != 0) {
                    for (std::int32_t i = start; i < Imin(LeakBandsCelt, end); ++i)
                        follower[i] = follower[i] + 1.f / 64.f * analysis.leakBoost[i];
                }
                for (std::int32_t i = start; i < end; ++i) {
                    follower[i]                  = Min32(follower[i], 4);
                    const std::int32_t width     = c * (eBands[i + 1] - eBands[i]) << lm;
                    std::int32_t       boost     = 0;
                    std::int32_t       boostBits = 0;
                    if (width < 6) {
                        boost     = static_cast<std::int32_t>(follower[i]);
                        boostBits = boost * width << BitRes;
                    } else if (width > 48) {
                        boost     = static_cast<std::int32_t>(follower[i] * 8);
                        boostBits = (boost * width << BitRes) / 8;
                    } else {
                        boost     = static_cast<std::int32_t>(follower[i] * width / 6);
                        boostBits = boost * 6 << BitRes;
                    }
                    // CBR and non-transient CVBR frames: dynalloc gets at most 2/3 of the bits
                    if ((vbr == 0 || (constrainedVbr != 0 && !isTransient)) && (totBoost + boostBits) >> BitRes >> 3 > 2 * effectiveBytes / 3) {
                        const std::int32_t cap = ((2 * effectiveBytes / 3) << BitRes << 3);
                        offsets[i]             = cap - totBoost;
                        totBoost               = cap;
                        break;
                    }
                    offsets[i] = boost;
                    totBoost += boostBits;
                }
            } else {
                for (std::int32_t i = start; i < end; ++i)
                    importance[i] = 13;
            }
            totBoostOut = totBoost;
            return maxDepth;
        }

        // libopus: tone_lpc() (celt/celt_encoder.c)
        // LPC coefficients from a least-squares fit of the forward and backward prediction
        bool ToneLpc(const float* x, std::int32_t len, std::int32_t delay, float* lpc) noexcept
        {
            float r00 = 0;
            float r01 = 0;
            float r02 = 0;
            // correlations as for the forward prediction covariance method
            for (std::int32_t i = 0; i < len - 2 * delay; ++i) {
                r00 += x[i] * x[i];
                r01 += x[i] * x[i + delay];
                r02 += x[i] * x[i + 2 * delay];
            }
            float edges = 0;
            for (std::int32_t i = 0; i < delay; ++i)
                edges += x[len + i - 2 * delay] * x[len + i - 2 * delay] - x[i] * x[i];
            float r11 = r00 + edges;
            edges     = 0;
            for (std::int32_t i = 0; i < delay; ++i)
                edges += x[len + i - delay] * x[len + i - delay] - x[i + delay] * x[i + delay];
            float r22 = r11 + edges;
            edges     = 0;
            for (std::int32_t i = 0; i < delay; ++i)
                edges += x[len + i - 2 * delay] * x[len + i - delay] - x[i] * x[i + delay];
            float r12 = r01 + edges;
            // reversed and summed for the backward contribution
            {
                const float rr00 = r00 + r22;
                const float rr01 = r01 + r12;
                const float rr11 = 2 * r11;
                const float rr02 = 2 * r02;
                const float rr12 = r12 + r01;
                const float rr22 = r00 + r22;
                r00              = rr00;
                r01              = rr01;
                r11              = rr11;
                r02              = rr02;
                r12              = rr12;
                r22              = rr22;
            }
            // A*x=b with A=[r00, r01; r01, r11] and b=[r02; r12]
            const float den = r00 * r11 - r01 * r01;
            if (den < .001f * (r00 * r11))
                return true;
            const float num1 = r02 * r11 - r01 * r12;
            if (num1 >= den)
                lpc[1] = 1.f;
            else if (num1 <= -den)
                lpc[1] = -1.f;
            else
                lpc[1] = num1 / den;
            const float num0 = r00 * r12 - r02 * r01;
            if (.5f * num0 >= den)
                lpc[0] = 1.999999f;
            else if (.5f * num0 <= -den)
                lpc[0] = -1.999999f;
            else
                lpc[0] = num0 / den;
            return false;
        }

        // libopus: tone_detect() (celt/celt_encoder.c)
        // pure or nearly pure tones, so that they do not cause problems in the encoder
        float ToneDetect(const float* in, std::int32_t cc, std::int32_t n, float& toneishness, std::int32_t fs) noexcept
        {
            float x[960 + CeltEncoder::Overlap];
            float lpc[2];
            if (cc == 2) {
                for (std::int32_t i = 0; i < n; ++i)
                    x[i] = in[i] + in[i + n];
            } else {
                for (std::int32_t i = 0; i < n; ++i)
                    x[i] = in[i];
            }
            std::int32_t delay = 1;
            bool         fail  = ToneLpc(x, n, delay, lpc);
            // a resonance too close to DC: retry with down-sampling
            while (delay <= fs / 3000 && (fail || (lpc[0] > 1.f && lpc[1] < 0))) {
                delay *= 2;
                fail = ToneLpc(x, n, delay, lpc);
            }
            // the filter must have complex roots
            if (!fail && static_cast<double>(lpc[0] * lpc[0]) + 3.999999 * lpc[1] < 0) {
                // the squared radius of the poles
                toneishness = -lpc[1];
                return static_cast<float>(std::acos(static_cast<double>(.5f * lpc[0])) / delay);
            }
            toneishness = 0;
            return -1;
        }

        // libopus: compute_vbr() (celt/celt_encoder.c)
        std::int32_t ComputeVbr(const CeltMode::View& mode, const AnalysisInfo& analysis, std::int32_t baseTarget, std::int32_t lm, std::int32_t bitrate,
                                std::int32_t lastCodedBands, std::int32_t c, std::int32_t intensity, std::int32_t constrainedVbr, float stereoSaving,
                                std::int32_t totBoost, float tfEstimate, bool pitchChange, float maxDepth, std::int32_t lfe, bool hasSurroundMask,
                                float surroundMasking, float temporalVbr) noexcept
        {
            const std::int32_t  nbEBands   = mode.nbEBands;
            const std::int16_t* eBands     = mode.eBands;
            const std::int32_t  codedBands = lastCodedBands != 0 ? lastCodedBands : nbEBands;
            std::int32_t        codedBins  = eBands[codedBands] << lm;
            if (c == 2)
                codedBins += eBands[Imin(intensity, codedBands)] << lm;

            // the target rate in 8th bits per frame
            std::int32_t target = baseTarget;
            if (analysis.valid != 0 && analysis.activity < .4)
                target -= static_cast<std::int32_t>((codedBins << BitRes) * (.4f - analysis.activity));
            // stereo savings
            if (c == 2) {
                const std::int32_t codedStereoBands = Imin(intensity, codedBands);
                const std::int32_t codedStereoDof   = (eBands[codedStereoBands] << lm) - codedStereoBands;
                // the maximum fraction of the bits saved if the signal is mono
                const float maxFrac = (0.8f * static_cast<float>(codedStereoDof)) / static_cast<float>(codedBins);
                stereoSaving        = Min32(stereoSaving, 1.f);
                target -= static_cast<std::int32_t>(Min32(maxFrac * target, (stereoSaving - 0.1f) * static_cast<float>(codedStereoDof << BitRes)));
            }
            // the dynalloc boost (minus its average for calibration)
            target += totBoost - (19 << lm);
            // the transient boost, compensated for the average boost
            const float tfCalibration = 0.044f;
            target += static_cast<std::int32_t>((tfEstimate - tfCalibration) * target);

            // the tonality boost (compensated for the average)
            if (analysis.valid != 0 && lfe == 0) {
                const float  tonal       = Max32(0.f, analysis.tonality - .15f) - 0.12f;
                std::int32_t tonalTarget = target + static_cast<std::int32_t>((codedBins << BitRes) * 1.2f * tonal);
                if (pitchChange)
                    tonalTarget += static_cast<std::int32_t>((codedBins << BitRes) * .8f);
                target = tonalTarget;
            }

            if (hasSurroundMask && lfe == 0) {
                const std::int32_t surroundTarget = target + static_cast<std::int32_t>(surroundMasking * static_cast<float>(codedBins << BitRes));
                target                            = Imax(target / 4, surroundTarget);
            }

            {
                const std::int32_t bins       = eBands[nbEBands - 2] << lm;
                std::int32_t       floorDepth = static_cast<std::int32_t>((c * bins << BitRes) * maxDepth);
                floorDepth                    = Imax(floorDepth, target >> 2);
                target                        = Imin(target, floorDepth);
            }

            // less aggressive for constrained VBR, which cannot keep a higher rate for long
            if ((!hasSurroundMask || lfe != 0) && constrainedVbr != 0)
                target = baseTarget + static_cast<std::int32_t>(0.67f * (target - baseTarget));

            if (!hasSurroundMask && tfEstimate < .2f) {
                const float amount     = .0000031f * Imax(0, Imin(32000, 96000 - bitrate));
                const float tvbrFactor = temporalVbr * amount;
                target += static_cast<std::int32_t>(tvbrFactor * target);
            }

            // at most double the rate
            return Imin(2 * baseTarget, target);
        }

    } // namespace

    // libopus: celt_preemphasis() (celt/celt_encoder.c)
    void CeltPreemphasis(const float* pcmp, float* inp, std::int32_t n, std::int32_t cc, std::int32_t upsample, const float* coef, float* mem,
                         bool clip) noexcept
    {
        Preemphasis(pcmp, inp, n, cc, upsample, coef, mem, clip);
    }

    // libopus: celt_encoder_init() (celt/celt_encoder.c) with opus_custom_encoder_init_arch()
    std::int32_t CeltEncoder::Init(std::int32_t fs, std::int32_t channels, std::int32_t arch) noexcept
    {
        if (channels < 0 || channels > 2)
            return OPUS_BAD_ARG;
        mode_ = CeltMode::Mode;

        // OPUS_CLEAR of the whole state
        channels_ = streamChannels_ = forceIntra_ = clip_ = disablePf_ = complexity_ = upsample_ = 0;
        start_ = end_ = bitrate_ = vbr_ = signalling_ = constrainedVbr_ = lossRate_ = lsbDepth_ = lfe_ = disableInv_ = arch_ = 0;

        streamChannels_ = channels_ = channels;
        upsample_                   = 1;
        start_                      = 0;
        end_                        = mode_.effEBands;
        signalling_                 = 1;
        arch_                       = arch;
        constrainedVbr_             = 1;
        clip_                       = 1;
        bitrate_                    = OPUS_BITRATE_MAX;
        vbr_                        = 0;
        forceIntra_                 = 0;
        complexity_                 = 5;
        lsbDepth_                   = 24;
        Reset();
        upsample_ = ResamplingFactor(fs);
        return OPUS_OK;
    }

    // libopus: OPUS_RESET_STATE of opus_custom_encoder_ctl() (celt/celt_encoder.c)
    void CeltEncoder::Reset() noexcept
    {
        auto* const start = reinterpret_cast<std::byte*>(&rng_);
        auto* const end   = reinterpret_cast<std::byte*>(this) + sizeof(*this);
        std::memset(start, 0, static_cast<std::size_t>(end - start));
        for (std::int32_t i = 0; i < channels_ * NbEBands; ++i)
            oldLogE_[i] = oldLogE2_[i] = -28.f;
        vbrOffset_      = 0;
        delayedIntra_   = 1;
        spreadDecision_ = SpreadNormal;
        tonalAverage_   = 256;
        hfAverage_      = 0;
        tapsetDecision_ = 0;
    }

    // libopus: opus_custom_encoder_ctl() (celt/celt_encoder.c)
    std::int32_t CeltEncoder::Ctl(std::int32_t request, std::int32_t value) noexcept
    {
        switch (request) {
            case OPUS_SET_COMPLEXITY_REQUEST:
                if (value < 0 || value > 10)
                    return OPUS_BAD_ARG;
                complexity_ = value;
                break;
            case CeltSetStartBandRequest:
                if (value < 0 || value >= mode_.nbEBands)
                    return OPUS_BAD_ARG;
                start_ = value;
                break;
            case CeltSetEndBandRequest:
                if (value < 1 || value > mode_.nbEBands)
                    return OPUS_BAD_ARG;
                end_ = value;
                break;
            case CeltSetPredictionRequest:
                if (value < 0 || value > 2)
                    return OPUS_BAD_ARG;
                disablePf_  = value <= 1;
                forceIntra_ = value == 0;
                break;
            case OPUS_SET_PACKET_LOSS_PERC_REQUEST:
                if (value < 0 || value > 100)
                    return OPUS_BAD_ARG;
                lossRate_ = value;
                break;
            case OPUS_SET_VBR_CONSTRAINT_REQUEST: constrainedVbr_ = value; break;
            case OPUS_SET_VBR_REQUEST: vbr_ = value; break;
            case OPUS_SET_BITRATE_REQUEST:
                if (value <= 500 && value != OPUS_BITRATE_MAX)
                    return OPUS_BAD_ARG;
                bitrate_ = Imin(value, 750000 * channels_);
                break;
            case CeltSetChannelsRequest:
                if (value < 1 || value > 2)
                    return OPUS_BAD_ARG;
                streamChannels_ = value;
                break;
            case OPUS_SET_LSB_DEPTH_REQUEST:
                if (value < 8 || value > 24)
                    return OPUS_BAD_ARG;
                lsbDepth_ = value;
                break;
            case OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST:
                if (value < 0 || value > 1)
                    return OPUS_BAD_ARG;
                disableInv_ = value;
                break;
            case OPUS_RESET_STATE: Reset(); break;
            case CeltSetSignallingRequest: signalling_ = value; break;
            default: return OPUS_UNIMPLEMENTED;
        }
        return OPUS_OK;
    }

    CeltEncoder::SignalConfig CeltEncoder::GetSignalConfig() const noexcept
    {
        return {channels_, streamChannels_, complexity_, clip_, lfe_, upsample_, arch_};
    }

    CeltEncoder::SignalState CeltEncoder::GetSignalState() const noexcept
    {
        SignalState state;
        std::memcpy(state.preemphMem, preemphMemE_, sizeof(state.preemphMem));
        state.overlapMax = overlapMax_;
        std::memcpy(state.prefilterMem, prefilterMem_, sizeof(state.prefilterMem));
        return state;
    }

    // libopus: celt_encode_with_ec() (celt/celt_encoder.c), the part from the sample peak to transient_analysis() and the
    // pitch search of run_prefilter()
    void CeltEncoder::SignalStage(const SignalConfig& config, const float* pcm, std::int32_t n, float* preemphMem, float& overlapMax, const float* prefilterMem,
                                  CeltSignalFrame& frame) const noexcept
    {
        const std::int32_t cc       = config.channels;
        const std::int32_t c        = config.streamChannels;
        const std::int32_t overlap  = mode_.overlap;
        const std::int32_t upsample = config.upsample;

        // the sample peak (of C channels' worth of samples, as libopus), with the overlap
        frame.sampleMax = Max32(overlapMax, MaxAbs(pcm, c * (n - overlap) / upsample));
        overlapMax      = MaxAbs(pcm + c * (n - overlap) / upsample, c * overlap / upsample);
        frame.sampleMax = Max32(frame.sampleMax, overlapMax);

        // the pre-emphasis, after the overlap of the pre-filter memory
        for (std::int32_t ch = 0; ch < cc; ++ch) {
            const bool needClip = config.clip != 0 && frame.sampleMax > 65536.f;
            Preemphasis(pcm + ch, frame.in + ch * (n + overlap) + overlap, n, cc, upsample, mode_.preemph, preemphMem + ch, needClip);
            std::memcpy(frame.in + ch * (n + overlap), &prefilterMem[(1 + ch) * MaxPeriod - overlap], static_cast<std::size_t>(overlap) * sizeof(float));
        }

        frame.toneishness = 0;
        frame.toneFreq    = ToneDetect(frame.in, cc, n + overlap, frame.toneishness, mode_.fs);

        // the transient analysis without weak transients (those are for hybrid at low rates)
        frame.transient     = config.complexity >= 1 && config.lfe == 0;
        frame.isTransient   = false;
        frame.weakTransient = false;
        frame.tfEstimate    = 0;
        frame.tfChan        = 0;
        if (frame.transient)
            frame.isTransient
                = TransientAnalysis(frame.in, n + overlap, cc, frame.tfEstimate, frame.tfChan, false, frame.weakTransient, frame.toneFreq, frame.toneishness);

        // the pitch search of the pre-filter, on the pre-emphasised signal after the pre-filter memory
        frame.pitch      = config.complexity >= 5;
        frame.pitchIndex = 0;
        if (frame.pitch) {
            float  preBuf[MaxChannels * (960 + MaxPeriod)];
            float* pre[2] = {preBuf, preBuf + (n + MaxPeriod)};
            for (std::int32_t ch = 0; ch < cc; ++ch) {
                std::memcpy(pre[ch], prefilterMem + ch * MaxPeriod, MaxPeriod * sizeof(float));
                std::memcpy(pre[ch] + MaxPeriod, frame.in + ch * (n + overlap) + overlap, static_cast<std::size_t>(n) * sizeof(float));
            }
            PitchDownsample(pre, frame.pitchBuf, (MaxPeriod + n) >> 1, cc, 2, config.arch);
            // the last 1.5 octave of the range has too many false positives from short-term correlation
            PitchSearch(frame.pitchBuf + (MaxPeriod >> 1), frame.pitchBuf, n, MaxPeriod - 3 * MinPeriod, &frame.pitchIndex, config.arch);
            frame.pitchIndex = MaxPeriod - frame.pitchIndex;
        }
    }

    // libopus: celt_encode_with_ec() (celt/celt_encoder.c), the part of SignalStage, with the pre-filter memory advanced
    // as run_prefilter() leaves it
    void CeltEncoder::AnalyseSignal(const SignalConfig& config, const float* pcm, std::int32_t frameSize, SignalState& state,
                                    CeltSignalFrame& frame) const noexcept
    {
        const std::int32_t cc      = config.channels;
        const std::int32_t overlap = mode_.overlap;
        const std::int32_t n       = frameSize * config.upsample;

        // the inputs, for the check in Encode
        frame.frameSize = frameSize;
        frame.config    = config;
        std::memcpy(frame.preemphMemBefore, state.preemphMem, sizeof(frame.preemphMemBefore));
        frame.overlapMaxBefore = state.overlapMax;
        std::memcpy(frame.prefilterMemBefore, state.prefilterMem, sizeof(frame.prefilterMemBefore));
        std::memcpy(frame.pcm, pcm, static_cast<std::size_t>(cc * frameSize) * sizeof(float));

        SignalStage(config, pcm, n, state.preemphMem, state.overlapMax, state.prefilterMem, frame);
        std::memcpy(frame.preemphMemAfter, state.preemphMem, sizeof(frame.preemphMemAfter));
        frame.overlapMaxAfter = state.overlapMax;

        // the pre-filter memory: the pre-emphasised signal, whatever the pre-filter decides (as RunPrefilter)
        for (std::int32_t ch = 0; ch < cc; ++ch) {
            const float* pre = frame.in + ch * (n + overlap) + overlap; // pre[c] + maxPeriod
            float*       mem = state.prefilterMem + ch * MaxPeriod;
            if (n > MaxPeriod) {
                std::memcpy(mem, pre + n - MaxPeriod, MaxPeriod * sizeof(float));
            } else {
                std::memmove(mem, mem + n, static_cast<std::size_t>(MaxPeriod - n) * sizeof(float));
                std::memcpy(mem + MaxPeriod - n, pre, static_cast<std::size_t>(n) * sizeof(float));
            }
        }
    }

    // libopus: run_prefilter() (celt/celt_encoder.c)
    std::int32_t CeltEncoder::RunPrefilter(FrontState& fs, float* in, std::int32_t cc, std::int32_t n, std::int32_t prefilterTapset, std::int32_t& pitch,
                                           float& gain, std::int32_t& qgain, bool enabled, std::int32_t complexity, float tfEstimate,
                                           std::int32_t nbAvailableBytes, float toneFreq, float toneishness, const CeltSignalFrame& signal,
                                           std::int32_t lossRate, std::int32_t analysisValid, float analysisMaxPitchRatio) const noexcept
    {
        constexpr std::int32_t maxPeriod = MaxPeriod;
        constexpr std::int32_t minPeriod = MinPeriod;
        const std::int32_t     overlap   = mode_.overlap;
        float                  preBuf[MaxChannels * (960 + MaxPeriod)];
        float*                 pre[2]     = {preBuf, preBuf + (n + maxPeriod)};
        float                  before[2]  = {0, 0};
        float                  after[2]   = {0, 0};
        std::int32_t           pitchIndex = 0;
        float                  gain1      = 0;

        for (std::int32_t c = 0; c < cc; ++c) {
            std::memcpy(pre[c], fs.prefilterMem + c * maxPeriod, maxPeriod * sizeof(float));
            std::memcpy(pre[c] + maxPeriod, in + c * (n + overlap) + overlap, static_cast<std::size_t>(n) * sizeof(float));
        }

        // a signal dominated by a single tone makes the standard pitch estimator unreliable
        if (enabled && toneishness > .99f) {
            std::int32_t multiple = 1;
            // the aliased postfilter above 24 kHz (slightly above pi, so not for 48 kHz)
            if (toneFreq >= 3.1416f)
                toneFreq = 3.141593f - toneFreq;
            // a pitch too high for the post-filter: double it until it fits
            while (toneFreq >= multiple * 0.39f)
                ++multiple;
            if (toneFreq > 0.006148f) {
                pitchIndex = Imin(static_cast<std::int32_t>(std::floor(.5 + 2.f * Pi * multiple / toneFreq)), maxPeriod - 2);
            } else {
                // a very low pitch: a very high pitch puts the DC part of the filter near the tone
                pitchIndex = minPeriod;
            }
            gain1 = .75f;
        } else if (enabled && complexity >= 5) {
            // the pitch search of the signal stage
            float pitchBuf[(MaxPeriod + 960) >> 1];
            std::memcpy(pitchBuf, signal.pitchBuf, static_cast<std::size_t>((maxPeriod + n) >> 1) * sizeof(float));
            pitchIndex = signal.pitchIndex;

            gain1 = RemoveDoubling(pitchBuf, maxPeriod, minPeriod, n, &pitchIndex, fs.prefilterPeriod, fs.prefilterGain);
            if (pitchIndex > maxPeriod - 2)
                pitchIndex = maxPeriod - 2;
            gain1 = .7f * gain1;
            if (lossRate > 2)
                gain1 = .5f * gain1;
            if (lossRate > 4)
                gain1 = .5f * gain1;
            if (lossRate > 8)
                gain1 = 0;
        } else {
            gain1      = 0;
            pitchIndex = minPeriod;
        }
        if (analysisValid != 0)
            gain1 = static_cast<float>(gain1 * analysisMaxPitchRatio);
        // the gain threshold for enabling the pre-filter/post-filter, adjusted for rate and continuity
        float pfThreshold = .2f;
        if (std::abs(pitchIndex - fs.prefilterPeriod) * 10 > pitchIndex) {
            pfThreshold += .2f;
            // completely disabled on strong transients without continuity
            if (tfEstimate > .98f)
                gain1 = 0;
        }
        if (nbAvailableBytes < 25)
            pfThreshold += .1f;
        if (nbAvailableBytes < 35)
            pfThreshold += .1f;
        if (fs.prefilterGain > .4f)
            pfThreshold -= .1f;
        if (fs.prefilterGain > .55f)
            pfThreshold -= .1f;

        // hard threshold at 0.2
        pfThreshold       = Max32(pfThreshold, .2f);
        std::int32_t pfOn = 0;
        std::int32_t qg   = 0;
        if (gain1 < pfThreshold) {
            gain1 = 0;
        } else {
            // not gated by a total bits check, only because of the nbAvailableBytes check above
            if (std::fabs(gain1 - fs.prefilterGain) < .1f)
                gain1 = fs.prefilterGain;
            qg    = static_cast<std::int32_t>(std::floor(.5f + gain1 * 32 / 3)) - 1;
            qg    = Imax(0, Imin(7, qg));
            gain1 = 0.09375f * (qg + 1);
            pfOn  = 1;
        }

        for (std::int32_t c = 0; c < cc; ++c) {
            const std::int32_t offset = mode_.shortMdctSize - overlap;
            fs.prefilterPeriod        = Imax(fs.prefilterPeriod, minPeriod);
            std::memcpy(in + c * (n + overlap), fs.inMem + c * overlap, static_cast<std::size_t>(overlap) * sizeof(float));
            for (std::int32_t i = 0; i < n; ++i)
                before[c] += std::fabs(in[c * (n + overlap) + overlap + i]);
            if (offset != 0)
                CombFilter(in + c * (n + overlap) + overlap, pre[c] + maxPeriod, fs.prefilterPeriod, fs.prefilterPeriod, offset, -fs.prefilterGain,
                           -fs.prefilterGain, fs.prefilterTapset, fs.prefilterTapset, nullptr, 0);

            CombFilter(in + c * (n + overlap) + overlap + offset, pre[c] + maxPeriod + offset, fs.prefilterPeriod, pitchIndex, n - offset, -fs.prefilterGain,
                       -gain1, fs.prefilterTapset, prefilterTapset, mode_.window, overlap);
            for (std::int32_t i = 0; i < n; ++i)
                after[c] += std::fabs(in[c * (n + overlap) + overlap + i]);
        }

        bool cancelPitch = false;
        if (cc == 2) {
            float thresh[2];
            thresh[0] = (.25f * gain1) * before[0] + .01f * before[1];
            thresh[1] = (.25f * gain1) * before[1] + .01f * before[0];
            // not if one channel gets significantly worse
            if (after[0] - before[0] > thresh[0] || after[1] - before[1] > thresh[1])
                cancelPitch = true;
            // only if at least one channel gets significantly better
            if (before[0] - after[0] < thresh[0] && before[1] - after[1] < thresh[1])
                cancelPitch = true;
        } else {
            // the mono channel must get better
            if (after[0] > before[0])
                cancelPitch = true;
        }
        // back to a gain of zero
        if (cancelPitch) {
            for (std::int32_t c = 0; c < cc; ++c) {
                const std::int32_t offset = mode_.shortMdctSize - overlap;
                std::memcpy(in + c * (n + overlap) + overlap, pre[c] + maxPeriod, static_cast<std::size_t>(n) * sizeof(float));
                CombFilter(in + c * (n + overlap) + overlap + offset, pre[c] + maxPeriod + offset, fs.prefilterPeriod, pitchIndex, overlap, -fs.prefilterGain,
                           0, fs.prefilterTapset, prefilterTapset, mode_.window, overlap);
            }
            gain1 = 0;
            pfOn  = 0;
            qg    = 0;
        }

        for (std::int32_t c = 0; c < cc; ++c) {
            std::memcpy(fs.inMem + c * overlap, in + c * (n + overlap) + n, static_cast<std::size_t>(overlap) * sizeof(float));
            if (n > maxPeriod) {
                std::memcpy(fs.prefilterMem + c * maxPeriod, pre[c] + n, maxPeriod * sizeof(float));
            } else {
                std::memmove(fs.prefilterMem + c * maxPeriod, fs.prefilterMem + c * maxPeriod + n, static_cast<std::size_t>(maxPeriod - n) * sizeof(float));
                std::memcpy(fs.prefilterMem + c * maxPeriod + maxPeriod - n, pre[c] + maxPeriod, static_cast<std::size_t>(n) * sizeof(float));
            }
        }

        gain  = gain1;
        pitch = pitchIndex;
        qgain = qg;
        return pfOn;
    }

    CeltEncoder::FrontState CeltEncoder::GetFrontState() const noexcept
    {
        FrontState state;
        std::memcpy(state.prefilterMem, prefilterMem_, sizeof(state.prefilterMem));
        std::memcpy(state.inMem, inMem_, sizeof(state.inMem));
        state.prefilterPeriod = prefilterPeriod_;
        state.prefilterGain   = prefilterGain_;
        state.prefilterTapset = prefilterTapset_;
        state.specAvg         = specAvg_;
        state.tonalAverage    = tonalAverage_;
        state.hfAverage       = hfAverage_;
        state.spreadDecision  = spreadDecision_;
        state.tapsetDecision  = tapsetDecision_;
        return state;
    }

    bool CeltEncoder::FrontStateIs(const FrontState& s, bool prefilterMemKnown) const noexcept
    {
        // as a memcmp of GetFrontState() with s, without the copy
        return (prefilterMemKnown || std::memcmp(prefilterMem_, s.prefilterMem, sizeof(s.prefilterMem)) == 0)
            && std::memcmp(inMem_, s.inMem, sizeof(s.inMem)) == 0 && prefilterPeriod_ == s.prefilterPeriod
            && std::memcmp(&prefilterGain_, &s.prefilterGain, sizeof(float)) == 0 && prefilterTapset_ == s.prefilterTapset
            && std::memcmp(&specAvg_, &s.specAvg, sizeof(float)) == 0 && tonalAverage_ == s.tonalAverage && hfAverage_ == s.hfAverage
            && spreadDecision_ == s.spreadDecision && tapsetDecision_ == s.tapsetDecision;
    }

    // libopus: celt_encode_with_ec() (celt/celt_encoder.c), the end: quant_energy_finalise() and the energy histories
    void CeltEncoder::FinishEnergies(const EnergyTail& t, float* error, std::int32_t bitsLeft, RangeEncoder* enc) noexcept
    {
        constexpr std::int32_t nbEBands = NbEBands;
        const std::int32_t     start    = t.start;
        const std::int32_t     end      = t.end;
        const std::int32_t     c        = t.c;
        const std::int32_t     cc       = t.cc;
        float* const           oldBandE = oldBandE_;
        float* const           oldLogE  = oldLogE_;
        float* const           oldLogE2 = oldLogE2_;
        QuantEnergyFinalise(start, end, oldBandE, error, t.fineQuant, t.finePriority, bitsLeft, enc, c);
        for (std::int32_t ch = 0; ch < c; ++ch) {
            for (std::int32_t i = start; i < end; ++i)
                energyError_[i + ch * nbEBands] = Max32(-0.5f, Min32(0.5f, error[i + ch * nbEBands]));
        }
        if (t.silence) {
            for (std::int32_t i = 0; i < c * nbEBands; ++i)
                oldBandE[i] = -28.f;
        }

        if (cc == 2 && c == 1)
            std::memcpy(&oldBandE[nbEBands], oldBandE, static_cast<std::size_t>(nbEBands) * sizeof(float));

        if (!t.isTransient) {
            std::memcpy(oldLogE2, oldLogE, static_cast<std::size_t>(cc * nbEBands) * sizeof(float));
            std::memcpy(oldLogE, oldBandE, static_cast<std::size_t>(cc * nbEBands) * sizeof(float));
        } else {
            for (std::int32_t i = 0; i < cc * nbEBands; ++i)
                oldLogE[i] = Min32(oldLogE[i], oldBandE[i]);
        }
        // in case start or end change
        for (std::int32_t ch = 0; ch < cc; ++ch) {
            for (std::int32_t i = 0; i < start; ++i) {
                oldBandE[ch * nbEBands + i] = 0;
                oldLogE[ch * nbEBands + i] = oldLogE2[ch * nbEBands + i] = -28.f;
            }
            for (std::int32_t i = end; i < nbEBands; ++i) {
                oldBandE[ch * nbEBands + i] = 0;
                oldLogE[ch * nbEBands + i] = oldLogE2[ch * nbEBands + i] = -28.f;
            }
        }
    }

    void CeltEncoder::SetFrontState(const FrontState& state) noexcept
    {
        std::memcpy(prefilterMem_, state.prefilterMem, sizeof(prefilterMem_));
        std::memcpy(inMem_, state.inMem, sizeof(inMem_));
        prefilterPeriod_ = state.prefilterPeriod;
        prefilterGain_   = state.prefilterGain;
        prefilterTapset_ = state.prefilterTapset;
        specAvg_         = state.specAvg;
        tonalAverage_    = state.tonalAverage;
        hfAverage_       = state.hfAverage;
        spreadDecision_  = state.spreadDecision;
        tapsetDecision_  = state.tapsetDecision;
    }

    // libopus: celt_encode_with_ec() (celt/celt_encoder.c), the symbols of the front part: the silence flag and the
    // pre-filter parameters
    void CeltEncoder::EncodeFrontSymbols(const FrontParams& params, const CeltFrontFrame& frame, RangeEncoder* enc) noexcept
    {
        // the silence flag; a silent frame is as if all remaining bits were filled with zeros
        // (which the initialiser did anyway), in VBR mode with the minimum size
        if (params.tell == 1)
            enc->EncodeBitLogp(frame.silence ? 1 : 0, 15);
        if (frame.silence) {
            if (params.vbrRate > 0)
                enc->Shrink(static_cast<std::uint32_t>(frame.nbCompressedBytes));
            enc->AddTotalBits(frame.tell - enc->Tell());
        }
        // the pre-filter
        if (frame.pfOn == 0) {
            if (params.start == 0 && frame.tell + 16 <= frame.totalBits)
                enc->EncodeBitLogp(0, 1);
        } else {
            // not gated by a total bits check, only because of the nbAvailableBytes check
            enc->EncodeBitLogp(1, 1);
            const std::int32_t pitchIndex = frame.pitchIndex + 1;
            const std::int32_t octave     = EcIlog(static_cast<std::uint32_t>(pitchIndex)) - 5;
            enc->EncodeUint(static_cast<std::uint32_t>(octave), 6);
            enc->EncodeBits(static_cast<std::uint32_t>(pitchIndex - (16 << octave)), static_cast<unsigned>(4 + octave));
            enc->EncodeBits(static_cast<std::uint32_t>(frame.qg), 3);
            enc->EncodeIcdf(frame.prefilterTapset, TapsetIcdf, 2);
        }
    }

    // libopus: celt_encode_with_ec() (celt/celt_encoder.c), the part from the silence decision to the temporal VBR, and
    // normalise_bands() to spreading_decision() (see the remarks on CeltEncoder)
    void CeltEncoder::Front(const FrontParams& p, const CeltSignalFrame& signal, FrontState& fs, RangeEncoder* enc, CeltFrontFrame& out) const noexcept
    {
        const std::int32_t cc       = channels_;
        const std::int32_t c        = p.streamChannels;
        const std::int32_t nbEBands = mode_.nbEBands;
        const std::int32_t overlap  = mode_.overlap;
        const std::int32_t start    = p.start;
        const std::int32_t end      = p.end;
        const std::int32_t effEnd   = p.effEnd;
        const bool         hybrid   = start != 0;
        const std::int32_t lm       = p.lm;
        const std::int32_t m        = 1 << lm;
        const std::int32_t n        = m * mode_.shortMdctSize;

        float* const in = out.in;
        std::memcpy(in, signal.in, static_cast<std::size_t>(cc * (n + overlap)) * sizeof(float));

        std::int32_t tell              = p.tell;
        std::int32_t totalBits         = p.totalBits;
        std::int32_t nbCompressedBytes = p.nbCompressedBytes;
        std::int32_t nbAvailableBytes  = p.nbAvailableBytes;
        std::int32_t effectiveBytes    = p.effectiveBytes;
        bool         silence           = signal.sampleMax <= static_cast<float>(1) / (1 << p.lsbDepth);
        if (tell != 1)
            silence = false;
        if (silence) {
            // in VBR mode the minimum suffices
            if (p.vbrRate > 0) {
                effectiveBytes = nbCompressedBytes = Imin(nbCompressedBytes, p.nbFilledBytes + 2);
                totalBits                          = nbCompressedBytes * 8;
                nbAvailableBytes                   = 2;
            }
            tell = nbCompressedBytes * 8;
        }
        float        tfEstimate    = 0;
        float        toneishness   = signal.toneishness;
        const float  toneFreq      = signal.toneFreq;
        bool         isTransient   = false;
        std::int32_t shortBlocks   = 0;
        std::int32_t tfChan        = 0;
        bool         weakTransient = false;
        if (p.complexity >= 1 && p.lfe == 0) {
            // fewer energy instabilities on fricatives at low rates in hybrid mode, but real
            // transients on vowels (small SILK quantisation offset)
            const bool allowWeakTransients = hybrid && effectiveBytes < 15 && p.silkSignalType != 2;
            if (signal.transient && !allowWeakTransients) {
                isTransient   = signal.isTransient;
                tfEstimate    = signal.tfEstimate;
                tfChan        = signal.tfChan;
                weakTransient = signal.weakTransient;
            } else {
                isTransient = TransientAnalysis(in, n + overlap, cc, tfEstimate, tfChan, allowWeakTransients, weakTransient, toneFreq, toneishness);
            }
        }
        toneishness = Min32(toneishness, 1.f - tfEstimate);

        // pitch period and gain
        std::int32_t pitchIndex      = MinPeriod;
        float        gain1           = 0;
        std::int32_t prefilterTapset = fs.tapsetDecision;
        std::int32_t qg              = 0;
        const bool   enabled
            = ((p.lfe != 0 && nbAvailableBytes > 3) || nbAvailableBytes > 12 * c) && !hybrid && !silence && tell + 16 <= totalBits && p.disablePf == 0;
        const std::int32_t pfOn = RunPrefilter(fs, in, cc, n, prefilterTapset, pitchIndex, gain1, qg, enabled, p.complexity, tfEstimate, nbAvailableBytes,
                                               toneFreq, toneishness, signal, p.lossRate, p.analysisValid, p.analysisMaxPitchRatio);
        const bool         pitchChange = (gain1 > .4f || fs.prefilterGain > .4f) && (p.analysisValid == 0 || p.analysisTonality > .3)
            && (pitchIndex > 1.26 * fs.prefilterPeriod || pitchIndex < .79 * fs.prefilterPeriod);

        out.silence           = silence;
        out.tell              = tell;
        out.totalBits         = totalBits;
        out.nbCompressedBytes = nbCompressedBytes;
        out.nbAvailableBytes  = nbAvailableBytes;
        out.effectiveBytes    = effectiveBytes;
        out.pitchIndex        = pitchIndex;
        out.qg                = qg;
        out.prefilterTapset   = prefilterTapset;
        out.pfOn              = pfOn;
        out.gain1             = gain1;
        out.pitchChange       = pitchChange;
        EncodeFrontSymbols(p, out, enc);

        bool transientGotDisabled = false;
        if (lm > 0 && enc->Tell() + 3 <= totalBits) {
            if (isTransient)
                shortBlocks = m;
        } else {
            isTransient          = false;
            transientGotDisabled = true;
        }

        float* const freq      = out.freq; // interleaved signal MDCTs
        float* const bandE     = out.bandE;
        float* const bandLogE  = out.bandLogE;
        float* const bandLogE2 = out.bandLogE2;

        const bool secondMdct = shortBlocks != 0 && p.complexity >= 8;
        if (secondMdct) {
            ComputeMdcts(mode_, 0, in, freq, c, cc, lm, upsample_);
            ComputeBandEnergies(freq, bandE, effEnd, c, lm);
            Amp2Log2(effEnd, end, bandE, bandLogE2, c);
            for (std::int32_t ch = 0; ch < c; ++ch) {
                for (std::int32_t i = 0; i < end; ++i)
                    bandLogE2[nbEBands * ch + i] += .5f * lm;
            }
        }

        ComputeMdcts(mode_, shortBlocks, in, freq, c, cc, lm, upsample_);
        if (cc == 2 && c == 1)
            tfChan = 0;
        ComputeBandEnergies(freq, bandE, effEnd, c, lm);

        if (p.lfe != 0) {
            for (std::int32_t i = 2; i < end; ++i) {
                bandE[i] = Min32(bandE[i], 1e-4f * bandE[0]);
                bandE[i] = Max32(bandE[i], 1e-15f);
            }
        }
        Amp2Log2(effEnd, end, bandE, bandLogE, c);

        // temporal VBR (not for LFE)
        float temporalVbr = 0;
        if (p.lfe == 0) {
            float       follow   = -10.0f;
            float       frameAvg = 0;
            const float offset   = shortBlocks != 0 ? .5f * lm : 0;
            for (std::int32_t i = start; i < end; ++i) {
                follow = Max32(follow - 1.0f, bandLogE[i] - offset);
                if (c == 2)
                    follow = Max32(follow, bandLogE[i + nbEBands] - offset);
                frameAvg += follow;
            }
            frameAvg /= (end - start);
            temporalVbr = frameAvg - fs.specAvg;
            temporalVbr = Min32(3.f, Max32(-1.5f, temporalVbr));
            fs.specAvg += .02f * temporalVbr;
        }

        if (!secondMdct)
            std::memcpy(bandLogE2, bandLogE, static_cast<std::size_t>(c * nbEBands) * sizeof(float));

        // the pre-filter of the next frame
        fs.prefilterPeriod = pitchIndex;
        fs.prefilterGain   = gain1;
        fs.prefilterTapset = prefilterTapset;

        // the extended part: the normalisation, the dynamic allocation and TF analyses, the
        // spreading decision (as if there were room for its symbol)
        out.extended = lm >= 1 && !p.masking;
        if (out.extended) {
            const std::int16_t* eBands = mode_.eBands;
            NormaliseBands(freq, out.x, bandE, effEnd, c, m);
            const bool   enableTfAnalysis = effectiveBytes >= 15 * c && !hybrid && p.complexity >= 2 && p.lfe == 0 && toneishness < .98f;
            AnalysisInfo analysis         = {};
            analysis.valid                = p.analysisValid;
            std::memcpy(analysis.leakBoost, p.analysisLeakBoost, sizeof(analysis.leakBoost));
            static constexpr float noSurround[NbEBands * MaxChannels] = {};
            std::int32_t           importance[NbEBands];
            std::int32_t           spreadWeight[NbEBands];
            out.totBoost = 0;
            // oldBandE is only read for 2.5 ms frames
            out.maxDepth = DynallocAnalysis(bandLogE, bandLogE2, nullptr, nbEBands, start, end, c, out.offsets, p.lsbDepth, mode_.logN, isTransient, p.vbr,
                                            p.constrainedVbr, eBands, lm, effectiveBytes, out.totBoost, p.lfe, noSurround, analysis, importance, spreadWeight,
                                            toneFreq, toneishness);
            std::int32_t* tfRes = out.tfRes;
            // no variable TF resolution for hybrid and at very low rates
            if (enableTfAnalysis) {
                const std::int32_t lambda = Imax(80, 20480 / effectiveBytes + 2);
                out.tfSelect              = TfAnalysis(mode_, effEnd, isTransient, tfRes, lambda, out.x, n, lm, tfEstimate, tfChan, importance);
                for (std::int32_t i = effEnd; i < end; ++i)
                    tfRes[i] = tfRes[effEnd - 1];
            } else if (hybrid && weakTransient) {
                for (std::int32_t i = 0; i < end; ++i)
                    tfRes[i] = 1;
                out.tfSelect = 0;
            } else if (hybrid && effectiveBytes < 15 && p.silkSignalType != 2) {
                for (std::int32_t i = 0; i < end; ++i)
                    tfRes[i] = 0;
                out.tfSelect = isTransient ? 1 : 0;
            } else {
                for (std::int32_t i = 0; i < end; ++i)
                    tfRes[i] = isTransient ? 1 : 0;
                out.tfSelect = 0;
            }
            out.spreadBefore[0] = fs.tonalAverage;
            out.spreadBefore[1] = fs.hfAverage;
            out.spreadBefore[2] = fs.spreadDecision;
            out.spreadBefore[3] = fs.tapsetDecision;
            if (p.lfe != 0) {
                fs.tapsetDecision = 0;
                fs.spreadDecision = SpreadNormal;
            } else if (hybrid) {
                if (p.complexity == 0)
                    fs.spreadDecision = SpreadNone;
                else if (isTransient)
                    fs.spreadDecision = SpreadNormal;
                else
                    fs.spreadDecision = SpreadAggressive;
            } else if (shortBlocks != 0 || p.complexity < 3 || nbAvailableBytes < 10 * c) {
                if (p.complexity == 0)
                    fs.spreadDecision = SpreadNone;
                else
                    fs.spreadDecision = SpreadNormal;
            } else {
                fs.spreadDecision = SpreadingDecision(out.x, &fs.tonalAverage, fs.spreadDecision, &fs.hfAverage, &fs.tapsetDecision,
                                                      pfOn != 0 && shortBlocks == 0, effEnd, c, m, spreadWeight);
            }
            out.spreadDecision = fs.spreadDecision;
            // the stereo thetas of the residual quantisation (its input x as here)
            if (c == 2) {
                for (std::int32_t i = start; i < end; ++i) {
                    const std::int32_t width = m * (eBands[i + 1] - eBands[i]);
                    out.stereoItheta[i]      = BandStereoItheta(out.x + m * eBands[i], out.x + n + m * eBands[i], width, bandE[i], bandE[nbEBands + i]);
                }
            }
        }

        out.tfEstimate           = tfEstimate;
        out.toneishness          = toneishness;
        out.toneFreq             = toneFreq;
        out.isTransient          = isTransient;
        out.weakTransient        = weakTransient;
        out.transientGotDisabled = transientGotDisabled;
        out.secondMdct           = secondMdct;
        out.tfChan               = tfChan;
        out.shortBlocks          = shortBlocks;
        out.temporalVbr          = temporalVbr;
    }

    // libopus: celt_encode_with_ec() (celt/celt_encoder.c); its sections are marked "// ---" below
    std::int32_t CeltEncoder::Encode(const float* pcm, std::int32_t frameSize, std::uint8_t* compressed, std::int32_t nbCompressedBytes, RangeEncoder* enc,
                                     const CeltSignalFrame* signal, const CeltFrontFrame* front) noexcept
    {
        const std::int32_t  frameSizeIn = frameSize;
        const std::int32_t  cc          = channels_;
        const std::int32_t  c           = streamChannels_;
        const std::int32_t  nbEBands    = mode_.nbEBands;
        const std::int16_t* eBands      = mode_.eBands;
        const std::int32_t  start       = start_;
        const std::int32_t  end         = end_;
        const bool          hybrid      = start != 0;
        if (nbCompressedBytes < 2 || pcm == nullptr)
            return OPUS_BAD_ARG;

        // --- celt_encode_with_ec: the frame size (LM), the start of the range coder, the bit budget
        // and the VBR rate

        // the main frames (with a range coder from outside) are counted; their expected successor
        // is speculated on from this state
        const std::int64_t frame                = enc != nullptr && specOut_ == nullptr ? frames_++ : -1;
        const std::int32_t frameSizeArg         = frameSize;
        const std::int32_t nbCompressedBytesArg = nbCompressedBytes;
        RangeEncoder       entryEnc;
        if (enc != nullptr)
            entryEnc = *enc;

        frameSize *= upsample_;
        std::int32_t lm = 0;
        for (lm = 0; lm <= mode_.maxLM; ++lm) {
            if (mode_.shortMdctSize << lm == frameSize)
                break;
        }
        if (lm > mode_.maxLM)
            return OPUS_BAD_ARG;
        const std::int32_t m = 1 << lm;
        const std::int32_t n = m * mode_.shortMdctSize;

        float* const oldBandE    = oldBandE_;
        float* const energyError = energyError_;

        std::int32_t tell          = 0;
        std::int32_t tell0Frac     = 0;
        std::int32_t nbFilledBytes = 0;
        if (enc == nullptr) {
            tell0Frac = tell = 1;
            nbFilledBytes    = 0;
        } else {
            tell0Frac     = static_cast<std::int32_t>(enc->TellFrac());
            tell          = enc->Tell();
            nbFilledBytes = (tell + 4) >> 3;
        }

        // at most 1275 bytes of payload
        nbCompressedBytes = Imin(nbCompressedBytes, PacketSizeCap);

        std::int32_t vbrRate        = 0;
        std::int32_t effectiveBytes = 0;
        if (vbr_ != 0 && bitrate_ != OPUS_BITRATE_MAX) {
            vbrRate        = BitrateToBits(bitrate_, mode_.fs, frameSize) << BitRes;
            effectiveBytes = vbrRate >> (3 + BitRes);
        } else {
            vbrRate          = 0;
            std::int32_t tmp = bitrate_ * frameSize;
            if (tell > 1)
                tmp += tell * mode_.fs;
            if (bitrate_ != OPUS_BITRATE_MAX) {
                nbCompressedBytes = Imax(2, Imin(nbCompressedBytes, (tmp + 4 * mode_.fs) / (8 * mode_.fs) - static_cast<std::int32_t>(signalling_ != 0)));
                if (enc != nullptr)
                    enc->Shrink(static_cast<std::uint32_t>(nbCompressedBytes));
            }
            effectiveBytes = nbCompressedBytes - nbFilledBytes;
        }
        std::int32_t nbAvailableBytes = nbCompressedBytes - nbFilledBytes;
        std::int32_t equivRate        = (nbCompressedBytes * 8 * 50 << (3 - lm)) - (40 * c + 20) * ((400 >> lm) - 50);
        if (bitrate_ != OPUS_BITRATE_MAX)
            equivRate = Imin(equivRate, bitrate_ - (40 * c + 20) * ((400 >> lm) - 50));

        RangeEncoder localEnc;
        if (enc == nullptr) {
            localEnc.Init(compressed, static_cast<std::uint32_t>(nbCompressedBytes));
            enc = &localEnc;
        }

        // --- the constrained VBR limit (max_allowed)

        if (vbrRate > 0) {
            // the max rate allowed in VBR mode against violating the target rate and buffering,
            // up front so that the bust prevention triggers correctly without enough bits
            if (constrainedVbr_ != 0) {
                // any multiple of vbr_rate works as bound (depending on the delay); at least two
                // bytes for an empty encoder, but 0 in hybrid mode
                const std::int32_t vbrBound   = vbrRate;
                const std::int32_t maxAllowed = Imin(Imax(tell == 1 ? 2 : 0, (vbrRate + vbrBound - vbrReservoir_) >> (BitRes + 3)), nbAvailableBytes);
                if (maxAllowed < nbAvailableBytes) {
                    nbCompressedBytes = nbFilledBytes + maxAllowed;
                    nbAvailableBytes  = maxAllowed;
                    enc->Shrink(static_cast<std::uint32_t>(nbCompressedBytes));
                }
            }
        }
        std::int32_t totalBits = nbCompressedBytes * 8;

        std::int32_t effEnd = end;
        if (effEnd > mode_.effEBands)
            effEnd = mode_.effEBands;

        // --- the sample peak, celt_preemphasis(), tone_detect(), transient_analysis() and the pitch
        // search of run_prefilter(): SignalStage

        // the signal stage: from elsewhere if it was computed from exactly this input and state
        const SignalConfig config      = GetSignalConfig();
        const bool         signalGiven = signal != nullptr;
        if (signal != nullptr) {
            const bool same = signal->frameSize == frameSizeIn && signal->config == config
                && std::memcmp(signal->preemphMemBefore, preemphMemE_, sizeof(signal->preemphMemBefore)) == 0
                && std::memcmp(&signal->overlapMaxBefore, &overlapMax_, sizeof(float)) == 0
                && std::memcmp(signal->prefilterMemBefore, prefilterMem_, sizeof(signal->prefilterMemBefore)) == 0
                && std::memcmp(signal->pcm, pcm, static_cast<std::size_t>(cc * frameSizeIn) * sizeof(float)) == 0;
            if (same) {
                std::memcpy(preemphMemE_, signal->preemphMemAfter, sizeof(signal->preemphMemAfter));
                overlapMax_ = signal->overlapMaxAfter;
            } else {
                signal = nullptr;
            }
        }
        CeltSignalFrame localSignal;
        if (signal == nullptr) {
            SignalStage(config, pcm, n, preemphMemE_, overlapMax_, prefilterMem_, localSignal);
            signal = &localSignal;
        }

        // --- the silence decision, run_prefilter() and its symbols, compute_mdcts(),
        // compute_band_energies(), amp2Log2(), the temporal VBR (and the extended part below): Front

        // the front part: from elsewhere if it was computed from exactly these parameters, this
        // state and the signal stage used here
        FrontParams params           = {};
        params.frameSize             = frameSizeIn;
        params.lm                    = lm;
        params.tell                  = tell;
        params.totalBits             = totalBits;
        params.nbCompressedBytes     = nbCompressedBytes;
        params.nbAvailableBytes      = nbAvailableBytes;
        params.nbFilledBytes         = nbFilledBytes;
        params.effectiveBytes        = effectiveBytes;
        params.vbrRate               = vbrRate;
        params.start                 = start;
        params.end                   = end;
        params.effEnd                = effEnd;
        params.streamChannels        = c;
        params.lsbDepth              = lsbDepth_;
        params.complexity            = complexity_;
        params.lfe                   = lfe_;
        params.disablePf             = disablePf_;
        params.lossRate              = lossRate_;
        params.vbr                   = vbr_;
        params.constrainedVbr        = constrainedVbr_;
        params.masking               = !hybrid && energyMask_ != nullptr && lfe_ == 0;
        params.silkSignalType        = silkSignalType_;
        params.analysisValid         = analysis_.valid;
        params.analysisTonality      = analysis_.tonality;
        params.analysisMaxPitchRatio = analysis_.maxPitchRatio;
        std::memcpy(params.analysisLeakBoost, analysis_.leakBoost, sizeof(params.analysisLeakBoost));
        lastFrontParams_ = params;
        lastFrontUsed_   = false;
        if (front != nullptr) {
            // the signal stage of the chain matched the state (prefilterMem included)
            const bool chainSignal = signalGiven && signal != &localSignal;
            if (chainSignal && front->valid && front->params == params && FrontStateIs(front->before, front->prefilterMemAsSignal)) {
                EncodeFrontSymbols(params, *front, enc);
                SetFrontState(front->after);
                lastFrontUsed_ = true;
            } else {
                front = nullptr;
            }
        }
        CeltFrontFrame localFront;
        if (front == nullptr) {
            FrontState state = GetFrontState();
            Front(params, *signal, state, enc, localFront);
            SetFrontState(state);
            front = &localFront;
        }

        const float* const in                   = front->in;
        const bool         silence              = front->silence;
        tell                                    = front->tell;
        totalBits                               = front->totalBits;
        nbCompressedBytes                       = front->nbCompressedBytes;
        nbAvailableBytes                        = front->nbAvailableBytes;
        effectiveBytes                          = front->effectiveBytes;
        float              tfEstimate           = front->tfEstimate;
        const float        toneishness          = front->toneishness;
        const float        toneFreq             = front->toneFreq;
        bool               isTransient          = front->isTransient;
        const bool         weakTransient        = front->weakTransient;
        const bool         transientGotDisabled = front->transientGotDisabled;
        const bool         pitchChange          = front->pitchChange;
        std::int32_t       tfChan               = front->tfChan;
        std::int32_t       shortBlocks          = front->shortBlocks;
        const std::int32_t pfOn                 = front->pfOn;
        const float        temporalVbr          = front->temporalVbr;
        const float*       freq                 = front->freq;
        float              freqPatched[MaxChannels * 960]; // the MDCTs of a late transient decision
        float              bandE[NbEBands * MaxChannels];
        float              bandLogE[NbEBands * MaxChannels];
        float              bandLogE2[NbEBands * MaxChannels];
        std::memcpy(bandE, front->bandE, sizeof(bandE));
        std::memcpy(bandLogE, front->bandLogE, sizeof(bandLogE));
        std::memcpy(bandLogE2, front->bandLogE2, sizeof(bandLogE2));

        // --- the surround masking analysis (energy_mask)

        float surroundDynalloc[NbEBands * MaxChannels];
        std::memset(surroundDynalloc, 0, static_cast<std::size_t>(end) * sizeof(float));
        float surroundMasking = 0;
        float surroundTrim    = 0;
        // the masking between surround channels
        if (!hybrid && energyMask_ != nullptr && lfe_ == 0) {
            float              maskAvg = 0;
            float              diff    = 0;
            std::int32_t       count   = 0;
            const std::int32_t maskEnd = Imax(2, lastCodedBands_);
            for (std::int32_t ch = 0; ch < c; ++ch) {
                for (std::int32_t i = 0; i < maskEnd; ++i) {
                    float mask = Max32(Min32(energyMask_[nbEBands * ch + i], .25f), -2.0f);
                    if (mask > 0)
                        mask = .5f * mask;
                    const float mask16 = mask;
                    maskAvg += mask16 * static_cast<float>(eBands[i + 1] - eBands[i]);
                    count += eBands[i + 1] - eBands[i];
                    diff += mask16 * static_cast<float>(1 + 2 * i - maskEnd);
                }
            }
            maskAvg = maskAvg / static_cast<float>(count);
            maskAvg += .2f;
            diff = diff * 6 / (c * (maskEnd - 1) * (maskEnd + 1) * maskEnd);
            // again, conservative
            diff = .5f * diff;
            diff = Max32(Min32(diff, .031f), -.031f);
            // the band in the middle of the coded spectrum
            std::int32_t midband = 0;
            for (midband = 0; eBands[midband + 1] < eBands[maskEnd] / 2; ++midband) {
            }
            std::int32_t countDynalloc = 0;
            for (std::int32_t i = 0; i < maskEnd; ++i) {
                const float lin    = maskAvg + diff * (i - midband);
                float       unmask = c == 2 ? Max32(energyMask_[i], energyMask_[nbEBands + i]) : energyMask_[i];
                unmask             = Min32(unmask, .0f);
                unmask -= lin;
                if (unmask > .25f) {
                    surroundDynalloc[i] = unmask - .25f;
                    ++countDynalloc;
                }
            }
            if (countDynalloc >= 3) {
                // dynalloc in many bands: the initial masking rate was probably too low
                maskAvg += .25f;
                if (maskAvg > 0) {
                    // something went really wrong: no masking
                    maskAvg = 0;
                    diff    = 0;
                    std::memset(surroundDynalloc, 0, static_cast<std::size_t>(maskEnd) * sizeof(float));
                } else {
                    for (std::int32_t i = 0; i < maskEnd; ++i)
                        surroundDynalloc[i] = Max32(0, surroundDynalloc[i] - .25f);
                }
            }
            maskAvg += .2f;
            // in the 1/64 units of the trim
            surroundTrim    = 64 * diff;
            surroundMasking = maskAvg;
        }

        // --- patch_transient_decision() and the transient flag

        // the last chance for a transient missed by the time-domain analysis
        bool patched = false;
        if (lm > 0 && enc->Tell() + 3 <= totalBits && !isTransient && complexity_ >= 5 && lfe_ == 0 && !hybrid) {
            if (PatchTransientDecision(bandLogE, oldBandE, nbEBands, start, end, c)) {
                patched     = true;
                isTransient = true;
                shortBlocks = m;
                ComputeMdcts(mode_, shortBlocks, in, freqPatched, c, cc, lm, upsample_);
                freq = freqPatched;
                ComputeBandEnergies(freq, bandE, effEnd, c, lm);
                Amp2Log2(effEnd, end, bandE, bandLogE, c);
                // the scaling of short vs long MDCTs
                for (std::int32_t ch = 0; ch < c; ++ch) {
                    for (std::int32_t i = 0; i < end; ++i)
                        bandLogE2[nbEBands * ch + i] += .5f * lm;
                }
                tfEstimate = .2f;
            }
        }

        if (lm > 0 && enc->Tell() + 3 <= totalBits)
            enc->EncodeBitLogp(isTransient ? 1 : 0, 3);

        // --- normalise_bands(), dynalloc_analysis() and tf_analysis(), from Front if it did them

        // the extended part of the front part, unless a late transient changed the blocks; else
        // its spreading decision (in the front state) is undone
        const bool frontRest = front->extended && !patched;
        if (front->extended && !frontRest) {
            tonalAverage_   = front->spreadBefore[0];
            hfAverage_      = front->spreadBefore[1];
            spreadDecision_ = front->spreadBefore[2];
            tapsetDecision_ = front->spreadBefore[3];
        }

        float        x[MaxChannels * 960]; // interleaved normalised MDCTs
        std::int32_t offsets[NbEBands];
        std::int32_t importance[NbEBands];
        std::int32_t spreadWeight[NbEBands];
        std::int32_t totBoost = 0;
        std::int32_t tfRes[NbEBands];
        std::int32_t tfSelect = 0;
        float        maxDepth = 0;
        if (frontRest) {
            // a copy: the residual quantisation then works in this core's cache
            std::memcpy(x, front->x, static_cast<std::size_t>(c * n) * sizeof(float));
            std::memcpy(offsets, front->offsets, sizeof(offsets));
            std::memcpy(tfRes, front->tfRes, sizeof(tfRes));
            tfSelect = front->tfSelect;
            totBoost = front->totBoost;
            maxDepth = front->maxDepth;
        } else {
            // band normalisation
            NormaliseBands(freq, x, bandE, effEnd, c, m);

            const bool enableTfAnalysis = effectiveBytes >= 15 * c && !hybrid && complexity_ >= 2 && lfe_ == 0 && toneishness < .98f;

            maxDepth
                = DynallocAnalysis(bandLogE, bandLogE2, oldBandE, nbEBands, start, end, c, offsets, lsbDepth_, mode_.logN, isTransient, vbr_, constrainedVbr_,
                                   eBands, lm, effectiveBytes, totBoost, lfe_, surroundDynalloc, analysis_, importance, spreadWeight, toneFreq, toneishness);

            // no variable TF resolution for hybrid and at very low rates
            if (enableTfAnalysis) {
                const std::int32_t lambda = Imax(80, 20480 / effectiveBytes + 2);
                tfSelect                  = TfAnalysis(mode_, effEnd, isTransient, tfRes, lambda, x, n, lm, tfEstimate, tfChan, importance);
                for (std::int32_t i = effEnd; i < end; ++i)
                    tfRes[i] = tfRes[effEnd - 1];
            } else if (hybrid && weakTransient) {
                // weak transients: improving the time resolution with TF on a long window is
                // imperfect and does not collapse the energy at low rates
                for (std::int32_t i = 0; i < end; ++i)
                    tfRes[i] = 1;
                tfSelect = 0;
            } else if (hybrid && effectiveBytes < 15 && silkSignalType_ != 2) {
                // low-rate hybrid: a temporal resolution of 5 ms rather than 2.5 ms
                for (std::int32_t i = 0; i < end; ++i)
                    tfRes[i] = 0;
                tfSelect = isTransient ? 1 : 0;
            } else {
                for (std::int32_t i = 0; i < end; ++i)
                    tfRes[i] = isTransient ? 1 : 0;
                tfSelect = 0;
            }
        }

        // --- the energy error bias, quant_coarse_energy(), tf_encode() and the spreading decision
        // (spreading_decision())

        float error[NbEBands * MaxChannels];
        for (std::int32_t ch = 0; ch < c; ++ch) {
            for (std::int32_t i = start; i < end; ++i) {
                // with a stable energy, bias the quantisation slightly towards the previous error
                // (a constant offset is better than fluctuations)
                if (std::fabs(bandLogE[i + ch * nbEBands] - oldBandE[i + ch * nbEBands]) < 2.f)
                    bandLogE[i + ch * nbEBands] -= .25f * energyError[i + ch * nbEBands];
            }
        }
        QuantCoarseEnergy(start, end, effEnd, bandLogE, oldBandE, static_cast<std::uint32_t>(totalBits), error, enc, c, lm, nbAvailableBytes, forceIntra_,
                          &delayedIntra_, complexity_ >= 4, lossRate_, lfe_);

        TfEncode(start, end, isTransient, tfRes, lm, tfSelect, enc);

        if (frontRest) {
            // the decision of the front part is in the state already
            if (enc->Tell() + 4 <= totalBits) {
                enc->EncodeIcdf(spreadDecision_, SpreadIcdf, 5);
            } else {
                tonalAverage_   = front->spreadBefore[0];
                hfAverage_      = front->spreadBefore[1];
                tapsetDecision_ = front->spreadBefore[3];
                spreadDecision_ = SpreadNormal;
            }
        } else if (enc->Tell() + 4 <= totalBits) {
            if (lfe_ != 0) {
                tapsetDecision_ = 0;
                spreadDecision_ = SpreadNormal;
            } else if (hybrid) {
                if (complexity_ == 0)
                    spreadDecision_ = SpreadNone;
                else if (isTransient)
                    spreadDecision_ = SpreadNormal;
                else
                    spreadDecision_ = SpreadAggressive;
            } else if (shortBlocks != 0 || complexity_ < 3 || nbAvailableBytes < 10 * c) {
                if (complexity_ == 0)
                    spreadDecision_ = SpreadNone;
                else
                    spreadDecision_ = SpreadNormal;
            } else {
                spreadDecision_ = SpreadingDecision(x, &tonalAverage_, spreadDecision_, &hfAverage_, &tapsetDecision_, pfOn != 0 && shortBlocks == 0, effEnd, c,
                                                    m, spreadWeight);
            }
            enc->EncodeIcdf(spreadDecision_, SpreadIcdf, 5);
        } else {
            spreadDecision_ = SpreadNormal;
        }

        // --- the dynalloc symbols (init_caps(), the boosts)

        // for LFE, everything interesting is in the first band
        if (lfe_ != 0)
            offsets[0] = Imin(8, effectiveBytes / 3);
        std::int32_t cap[NbEBands];
        InitCaps(cap, lm, c);

        std::int32_t dynallocLogp = 6;
        totalBits <<= BitRes;
        std::int32_t totalBoost = 0;
        tell                    = static_cast<std::int32_t>(enc->TellFrac());
        for (std::int32_t i = start; i < end; ++i) {
            const std::int32_t width = c * (eBands[i + 1] - eBands[i]) << lm;
            // quanta: 6 bits, but at most 1 bit and at least 1/8 bit per sample
            const std::int32_t quanta           = Imin(width << BitRes, Imax(6 << BitRes, width));
            std::int32_t       dynallocLoopLogp = dynallocLogp;
            std::int32_t       boost            = 0;
            std::int32_t       j                = 0;
            for (j = 0; tell + (dynallocLoopLogp << BitRes) < totalBits - totalBoost && boost < cap[i]; ++j) {
                const std::int32_t flag = j < offsets[i] ? 1 : 0;
                enc->EncodeBitLogp(flag, static_cast<unsigned>(dynallocLoopLogp));
                tell = static_cast<std::int32_t>(enc->TellFrac());
                if (flag == 0)
                    break;
                boost += quanta;
                totalBoost += quanta;
                dynallocLoopLogp = 1;
            }
            // dynalloc more likely
            if (j != 0)
                dynallocLogp = Imax(2, dynallocLogp - 1);
            offsets[i] = boost;
        }

        // --- stereo_analysis(), the intensity (hysteresis_decision()) and alloc_trim_analysis()

        std::int32_t dualStereo = 0;
        if (c == 2) {
            // always M/S for 2.5 ms frames until there is a better analysis
            if (lm != 0)
                dualStereo = StereoAnalysis(mode_, x, lm, n) ? 1 : 0;
            intensity_ = HysteresisDecision(static_cast<float>(equivRate / 1000), IntensityThresholds, IntensityHysteresis, 21, intensity_);
            intensity_ = Imin(end, Imax(start, intensity_));
        }

        std::int32_t allocTrim = 5;
        if (tell + (6 << BitRes) <= totalBits - totalBoost) {
            if (start > 0 || lfe_ != 0) {
                stereoSaving_ = 0;
                allocTrim     = 5;
            } else {
                allocTrim = AllocTrimAnalysis(mode_, x, bandLogE, end, lm, c, n, analysis_, stereoSaving_, tfEstimate, intensity_, surroundTrim, equivRate);
            }
            enc->EncodeIcdf(allocTrim, TrimIcdf, 7);
            tell = static_cast<std::int32_t>(enc->TellFrac());
        }

        // --- the VBR target (compute_vbr()) and the VBR state (reservoir, drift, offset)

        // VBR must not shrink the frame so much that the encoder runs out of bits; the margin of
        // 2 bytes keeps all bust prevention of the decoder untriggered so far
        std::int32_t minAllowed = ((tell + totalBoost + (1 << (BitRes + 3)) - 1) >> (BitRes + 3)) + 2;
        // the 37 bits needed left in the packet for a redundant frame in hybrid mode (a shorter
        // packet would desync the entropy coder)
        if (hybrid)
            minAllowed = Imax(minAllowed, (tell0Frac + (37 << BitRes) + totalBoost + (1 << (BitRes + 3)) - 1) >> (BitRes + 3));
        // variable bitrate
        if (vbrRate > 0) {
            const std::int32_t lmDiff = mode_.maxLM - lm;
            // not more than 510 kb/s, even for frames below 20 ms (the allocator cannot use more)
            nbCompressedBytes       = Imin(nbCompressedBytes, PacketSizeCap >> (3 - lm));
            std::int32_t baseTarget = 0;
            if (!hybrid)
                baseTarget = vbrRate - ((40 * c + 20) << BitRes);
            else
                baseTarget = Imax(0, vbrRate - ((9 * c + 4) << BitRes));

            if (constrainedVbr_ != 0)
                baseTarget += (vbrOffset_ >> lmDiff);

            // the target rate in 8th bits per frame
            std::int32_t target = 0;
            if (!hybrid) {
                target = ComputeVbr(mode_, analysis_, baseTarget, lm, equivRate, lastCodedBands_, c, intensity_, constrainedVbr_, stereoSaving_, totBoost,
                                    tfEstimate, pitchChange, maxDepth, lfe_, energyMask_ != nullptr, surroundMasking, temporalVbr);
            } else {
                target = baseTarget;
                // tonal frames (offset < 100) need more bits than noisy ones (offset > 100)
                if (silkOffset_ < 100)
                    target += 12 << BitRes >> (3 - lm);
                if (silkOffset_ > 100)
                    target -= 18 << BitRes >> (3 - lm);
                // more on transients and on vowels with significant temporal spikes
                target += static_cast<std::int32_t>((tfEstimate - .25f) * (50 << BitRes));
                // a strong transient gets enough bits for the first two bands (folding rather than noise)
                if (tfEstimate > .7f)
                    target = Imax(target, 50 << BitRes);
            }
            // the current offset removed from the target, the space used so far added
            target = target + tell;

            nbAvailableBytes = (target + (1 << (BitRes + 2))) >> (BitRes + 3);
            nbAvailableBytes = Imax(minAllowed, nbAvailableBytes);
            nbAvailableBytes = Imin(nbCompressedBytes, nbAvailableBytes);

            // by how much the frame missed the target
            std::int32_t delta = target - vbrRate;

            target = nbAvailableBytes << (BitRes + 3);

            // silent frames do not adjust the drift (the encoder would shoot to very high rates
            // after silence), but the reservoir refills: VBR undershoots on files with much silence
            if (silence) {
                nbAvailableBytes = 2;
                target           = 2 * 8 << BitRes;
                delta            = 0;
            }

            float alpha = 0;
            if (vbrCount_ < 970) {
                ++vbrCount_;
                alpha = 1.f / (vbrCount_ + 20);
            } else {
                alpha = .001f;
            }
            // the bits used in excess of what is allowed
            if (constrainedVbr_ != 0)
                vbrReservoir_ += target - vbrRate;

            // the offset that reaches the target
            if (constrainedVbr_ != 0) {
                vbrDrift_ += static_cast<std::int32_t>(alpha * ((delta * (1 << lmDiff)) - vbrOffset_ - vbrDrift_));
                vbrOffset_ = -vbrDrift_;
            }

            if (constrainedVbr_ != 0 && vbrReservoir_ < 0) {
                // under the minimum: more rate, unless coding silence
                const std::int32_t adjust = (-vbrReservoir_) / (8 << BitRes);
                nbAvailableBytes += silence ? 0 : adjust;
                vbrReservoir_ = 0;
            }
            nbCompressedBytes = Imin(nbCompressedBytes, nbAvailableBytes);
            // moves the raw bits to the new compressed size
            enc->Shrink(static_cast<std::uint32_t>(nbCompressedBytes));
        }

        // --- clt_compute_allocation() and quant_fine_energy()

        std::int32_t fineQuant[NbEBands];
        std::int32_t pulses[NbEBands];
        std::int32_t finePriority[NbEBands];

        // bits = packet size - where we are - safety
        std::int32_t       bits            = ((nbCompressedBytes * 8) << BitRes) - static_cast<std::int32_t>(enc->TellFrac()) - 1;
        const std::int32_t antiCollapseRsv = isTransient && lm >= 2 && bits >= ((lm + 2) << BitRes) ? (1 << BitRes) : 0;
        bits -= antiCollapseRsv;
        std::int32_t signalBandwidth = end - 1;
        if (analysis_.valid != 0) {
            std::int32_t minBandwidth = 20;
            if (equivRate < 32000 * c)
                minBandwidth = 13;
            else if (equivRate < 48000 * c)
                minBandwidth = 16;
            else if (equivRate < 60000 * c)
                minBandwidth = 18;
            else if (equivRate < 80000 * c)
                minBandwidth = 19;
            signalBandwidth = Imax(analysis_.bandwidth, minBandwidth);
        }
        if (lfe_ != 0)
            signalBandwidth = 1;
        std::int32_t       balance    = 0;
        const std::int32_t codedBands = ComputeAllocation(start, end, offsets, cap, allocTrim, &intensity_, &dualStereo, bits, &balance, pulses, fineQuant,
                                                          finePriority, c, lm, enc, lastCodedBands_, signalBandwidth);
        if (lastCodedBands_ != 0)
            lastCodedBands_ = Imin(lastCodedBands_ + 1, Imax(lastCodedBands_ - 1, codedBands));
        else
            lastCodedBands_ = codedBands;

        QuantFineEnergy(start, end, oldBandE, error, fineQuant, enc, c);
        std::memset(energyError, 0, static_cast<std::size_t>(nbEBands * cc) * sizeof(float));

        // --- quant_all_bands(), or the replay of the speculation (CeltSpeculation.h)

        std::uint8_t collapseMasks[NbEBands * MaxChannels];
        // the inputs of the residual quantisation
        QabInputs  qin;
        const bool specOn = specOut_ != nullptr || (spec_ != nullptr && enc != nullptr);
        if (specOn) {
            qin.start       = start;
            qin.end         = end;
            qin.c           = c;
            qin.n           = n;
            qin.m           = m;
            qin.lm          = lm;
            qin.shortBlocks = shortBlocks;
            qin.spread      = spreadDecision_;
            qin.dualStereo  = dualStereo;
            qin.intensity   = intensity_;
            qin.totalBits   = nbCompressedBytes * (8 << BitRes) - antiCollapseRsv;
            qin.balance     = balance;
            qin.codedBands  = codedBands;
            qin.complexity  = complexity_;
            qin.disableInv  = disableInv_;
            qin.range       = enc->Range();
            qin.tellFrac    = enc->TellFrac();
            qin.tell        = enc->Tell();
            qin.storage     = enc->Storage();
            std::memcpy(qin.pulses, pulses, sizeof(qin.pulses));
            std::memcpy(qin.tfRes, tfRes, sizeof(qin.tfRes));
            qin.hasItheta = frontRest && c == 2;
            if (qin.hasItheta)
                std::memcpy(qin.stereoItheta, front->stereoItheta, sizeof(qin.stereoItheta));
            std::memcpy(qin.bandE, bandE, sizeof(qin.bandE));
            std::memcpy(qin.x, x, static_cast<std::size_t>(c * n) * sizeof(float));
        }
        EnergyTail specTail;
        if (spec_ != nullptr) {
            specTail.start = start;
            specTail.end   = end;
            specTail.c     = c;
            specTail.cc    = cc;
            std::memcpy(specTail.fineQuant, fineQuant, sizeof(specTail.fineQuant));
            std::memcpy(specTail.finePriority, finePriority, sizeof(specTail.finePriority));
            specTail.silence     = silence;
            specTail.isTransient = isTransient;
        }
        if (specOut_ != nullptr) {
            // a speculative run: the residual quantisation as a job (and the speculation of the
            // frame after from this speculative state); the run ends here
            PostQabJob(qin, *enc, specTail, error, transientGotDisabled, frameSizeArg, nbCompressedBytesArg, entryEnc);
            return 0;
        }
        // the speculations for this frame, if there are any (the first one or a cascaded one)
        const bool fromJob = enc != nullptr && TakeSpeculation(frame, qin, *enc);
        // the speculation of the next frame, if this one is quantised here (else the next one is,
        // and the speculation goes on from there): encoder and speculation take turns
        const bool speculating = spec_ != nullptr && enc != nullptr && !fromJob
            && SpeculateNext(specTail, error, transientGotDisabled, frameSizeArg, nbCompressedBytesArg, entryEnc, frame);
        next_ = nullptr;
        if (!fromJob)
            QuantAllBands(start, end, x, c == 2 ? x + n : nullptr, collapseMasks, bandE, pulses, shortBlocks, spreadDecision_, dualStereo, intensity_, tfRes,
                          nbCompressedBytes * (8 << BitRes) - antiCollapseRsv, balance, enc, lm, codedBands, &rng_, complexity_, disableInv_, thetaRunner_,
                          frontRest && c == 2 ? front->stereoItheta : nullptr);
        // the seed of the next frame: the final range (only raw bits follow)
        if (speculating)
            PublishSeed(enc->Range());

        // --- the anti-collapse bit, quant_energy_finalise() and the energy histories
        // (FinishEnergies), consec_transient, the final range and ec_enc_done()

        if (antiCollapseRsv > 0) {
            const std::int32_t antiCollapseOn = consecTransient_ < 2 ? 1 : 0;
            enc->EncodeBits(static_cast<std::uint32_t>(antiCollapseOn), 1);
        }
        EnergyTail tail;
        tail.start = start;
        tail.end   = end;
        tail.c     = c;
        tail.cc    = cc;
        std::memcpy(tail.fineQuant, fineQuant, sizeof(tail.fineQuant));
        std::memcpy(tail.finePriority, finePriority, sizeof(tail.finePriority));
        tail.silence     = silence;
        tail.isTransient = isTransient;
        FinishEnergies(tail, error, nbCompressedBytes * 8 - enc->Tell(), enc);

        if (isTransient || transientGotDisabled)
            ++consecTransient_;
        else
            consecTransient_ = 0;
        rng_ = enc->Range();

        // any room left (only at very high rates) is already zero-filled
        enc->Done();

        if (enc->Error() != 0)
            return OPUS_INTERNAL_ERROR;
        return nbCompressedBytes;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
