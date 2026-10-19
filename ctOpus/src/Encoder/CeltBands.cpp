// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "CeltBands.h"

#include <cmath>
#include <cstring>
#include <new>

#include "CeltMath.h"
#include "CeltMode.h"
#include "CeltRate.h"
#include "CeltSimd.h"
#include "CeltVq.h"
#include "IntMath.h"

#include <intrin.h>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    // libopus: hysteresis_decision() (celt/bands.c)
    std::int32_t HysteresisDecision(float val, const float* thresholds, const float* hysteresis, std::int32_t n, std::int32_t prev) noexcept
    {
        std::int32_t i = 0;
        for (i = 0; i < n; ++i) {
            if (val < thresholds[i])
                break;
        }
        if (i > prev && val < thresholds[prev] + hysteresis[prev])
            i = prev;
        if (i < prev && val > thresholds[prev - 1] - hysteresis[prev - 1])
            i = prev;
        return i;
    }

    // libopus: compute_band_energies() (celt/bands.c)
    void ComputeBandEnergies(const float* x, float* bandE, std::int32_t end, std::int32_t c, std::int32_t lm) noexcept
    {
        const std::int16_t* eBands = CeltMode::EBands;
        const std::int32_t  n      = CeltMode::ShortMdctSize << lm;
        for (std::int32_t ch = 0; ch < c; ++ch) {
            for (std::int32_t i = 0; i < end; ++i) {
                const float* band                  = &x[ch * n + (eBands[i] << lm)];
                const float  sum                   = 1e-27f + InnerProd(band, band, (eBands[i + 1] - eBands[i]) << lm);
                bandE[i + ch * CeltMode::NbEBands] = static_cast<float>(std::sqrt(static_cast<double>(sum)));
            }
        }
    }

    // libopus: normalise_bands() (celt/bands.c)
    void NormaliseBands(const float* freq, float* x, const float* bandE, std::int32_t end, std::int32_t c, std::int32_t m) noexcept
    {
        const std::int16_t* eBands = CeltMode::EBands;
        const std::int32_t  n      = m * CeltMode::ShortMdctSize;
        for (std::int32_t ch = 0; ch < c; ++ch) {
            for (std::int32_t i = 0; i < end; ++i) {
                const float g = 1.f / (1e-27f + bandE[i + ch * CeltMode::NbEBands]);
                for (std::int32_t j = m * eBands[i]; j < m * eBands[i + 1]; ++j)
                    x[j + ch * n] = freq[j + ch * n] * g;
            }
        }
    }

    // libopus: haar1() (celt/bands.c)
    void Haar1(float* x, std::int32_t n0, std::int32_t stride) noexcept
    {
        n0 >>= 1;
        for (std::int32_t i = 0; i < stride; ++i) {
            for (std::int32_t j = 0; j < n0; ++j) {
                const float tmp1            = .70710678f * x[stride * 2 * j + i];
                const float tmp2            = .70710678f * x[stride * (2 * j + 1) + i];
                x[stride * 2 * j + i]       = tmp1 + tmp2;
                x[stride * (2 * j + 1) + i] = tmp1 - tmp2;
            }
        }
    }

    // libopus: spreading_decision() (celt/bands.c)
    std::int32_t SpreadingDecision(const float* x, std::int32_t* average, std::int32_t lastDecision, std::int32_t* hfAverage, std::int32_t* tapsetDecision,
                                   bool updateHf, std::int32_t end, std::int32_t c, std::int32_t m, const std::int32_t* spreadWeight) noexcept
    {
        const std::int16_t* eBands  = CeltMode::EBands;
        const std::int32_t  n0      = m * CeltMode::ShortMdctSize;
        std::int32_t        sum     = 0;
        std::int32_t        nbBands = 0;
        std::int32_t        hfSum   = 0;

        if (m * (eBands[end] - eBands[end - 1]) <= 8)
            return SpreadNone;
        for (std::int32_t ch = 0; ch < c; ++ch) {
            for (std::int32_t i = 0; i < end; ++i) {
                std::int32_t       tcount[3] = {0, 0, 0};
                const float*       xb        = x + m * eBands[i] + ch * n0;
                const std::int32_t n         = m * (eBands[i + 1] - eBands[i]);
                if (n <= 8)
                    continue;
                // a rough CDF of |x[j]|
                for (std::int32_t j = 0; j < n; ++j) {
                    const float x2N = (xb[j] * xb[j]) * static_cast<float>(n);
                    if (x2N < 0.25f)
                        ++tcount[0];
                    if (x2N < 0.0625f)
                        ++tcount[1];
                    if (x2N < 0.015625f)
                        ++tcount[2];
                }
                // only the four last bands (8 kHz and up)
                if (i > CeltMode::NbEBands - 4)
                    hfSum += static_cast<std::int32_t>(static_cast<std::uint32_t>(32 * (tcount[1] + tcount[0])) / static_cast<std::uint32_t>(n));
                const std::int32_t tmp = (2 * tcount[2] >= n) + (2 * tcount[1] >= n) + (2 * tcount[0] >= n);
                sum += tmp * spreadWeight[i];
                nbBands += spreadWeight[i];
            }
        }

        if (updateHf) {
            if (hfSum != 0)
                hfSum = static_cast<std::int32_t>(static_cast<std::uint32_t>(hfSum) / static_cast<std::uint32_t>(c * (4 - CeltMode::NbEBands + end)));
            *hfAverage = (*hfAverage + hfSum) >> 1;
            hfSum      = *hfAverage;
            if (*tapsetDecision == 2)
                hfSum += 4;
            else if (*tapsetDecision == 0)
                hfSum -= 4;
            if (hfSum > 22)
                *tapsetDecision = 2;
            else if (hfSum > 18)
                *tapsetDecision = 1;
            else
                *tapsetDecision = 0;
        }
        sum = static_cast<std::int32_t>(static_cast<std::uint32_t>(sum << 8) / static_cast<std::uint32_t>(nbBands));
        // recursive averaging
        sum      = (sum + *average) >> 1;
        *average = sum;
        // hysteresis
        sum = (3 * sum + (((3 - lastDecision) << 7) + 64) + 2) >> 2;
        if (sum < 80)
            return SpreadAggressive;
        if (sum < 256)
            return SpreadNormal;
        if (sum < 384)
            return SpreadLight;
        return SpreadNone;
    }

    namespace {

        constexpr std::int32_t NbEBands        = CeltMode::NbEBands;
        constexpr float        MinStereoEnergy = 1e-10f;

        // libopus: celt_udiv() (celt/entcode.h)
        inline std::int32_t Udiv(std::int32_t n, std::int32_t d) noexcept
        {
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(n) / static_cast<std::uint32_t>(d));
        }

        // libopus: celt_sudiv() (celt/entcode.h): division rounding towards zero
        inline std::int32_t Sudiv(std::int32_t n, std::int32_t d) noexcept
        {
            return n < 0 ? -Udiv(-n, d) : Udiv(n, d);
        }

        // FRAC_MUL16 (mathops.h): bit-exactness matters
        constexpr std::int32_t FracMul16(std::int32_t a, std::int32_t b) noexcept
        {
            return (16384 + static_cast<std::int32_t>(static_cast<std::int16_t>(a)) * static_cast<std::int16_t>(b)) >> 15;
        }

        // libopus: celt_lcg_rand() (celt/bands.c)
        inline std::uint32_t LcgRand(std::uint32_t seed) noexcept
        {
            return 1664525 * seed + 1013904223;
        }

        // libopus: bitexact_cos() (celt/bands.c)
        // a cos() approximation that is bit-exact on any platform (it affects the bit allocation)
        std::int16_t BitexactCos(std::int16_t x) noexcept
        {
            const std::int32_t tmp = (4096 + static_cast<std::int32_t>(x) * x) >> 13;
            auto               x2  = static_cast<std::int16_t>(tmp);
            x2                     = static_cast<std::int16_t>((32767 - x2) + FracMul16(x2, (-7651 + FracMul16(x2, (8277 + FracMul16(-626, x2))))));
            return static_cast<std::int16_t>(1 + x2);
        }

        // libopus: bitexact_log2tan() (celt/bands.c)
        std::int32_t BitexactLog2tan(std::int32_t isin, std::int32_t icos) noexcept
        {
            const std::int32_t lc = EcIlog(static_cast<std::uint32_t>(icos));
            const std::int32_t ls = EcIlog(static_cast<std::uint32_t>(isin));
            icos <<= 15 - lc;
            isin <<= 15 - ls;
            return (ls - lc) * (1 << 11) + FracMul16(isin, FracMul16(isin, -2597) + 7932) - FracMul16(icos, FracMul16(icos, -2597) + 7932);
        }

        // natural Hadamard to ordery Hadamard: a bit-reversed Gray order, inverted to put DC at
        // the end (lines for N=2, 4, 8, 16)
        constexpr std::int32_t OrderyTable[] = {
            1, 0, 3, 0, 2, 1, 7, 0, 4, 3, 6, 1, 5, 2, 15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5,
        };

        // libopus: deinterleave_hadamard() (celt/bands.c)
        void DeinterleaveHadamard(float* x, std::int32_t n0, std::int32_t stride, bool hadamard) noexcept
        {
            float              tmp[176];
            const std::int32_t n = n0 * stride;
            if (hadamard) {
                const std::int32_t* ordery = OrderyTable + stride - 2;
                for (std::int32_t i = 0; i < stride; ++i)
                    for (std::int32_t j = 0; j < n0; ++j)
                        tmp[ordery[i] * n0 + j] = x[j * stride + i];
            } else {
                for (std::int32_t i = 0; i < stride; ++i)
                    for (std::int32_t j = 0; j < n0; ++j)
                        tmp[i * n0 + j] = x[j * stride + i];
            }
            std::memcpy(x, tmp, static_cast<std::size_t>(n) * sizeof(float));
        }

        // libopus: interleave_hadamard() (celt/bands.c)
        void InterleaveHadamard(float* x, std::int32_t n0, std::int32_t stride, bool hadamard) noexcept
        {
            float              tmp[176];
            const std::int32_t n = n0 * stride;
            if (hadamard) {
                const std::int32_t* ordery = OrderyTable + stride - 2;
                for (std::int32_t i = 0; i < stride; ++i)
                    for (std::int32_t j = 0; j < n0; ++j)
                        tmp[j * stride + i] = x[ordery[i] * n0 + j];
            } else {
                for (std::int32_t i = 0; i < stride; ++i)
                    for (std::int32_t j = 0; j < n0; ++j)
                        tmp[j * stride + i] = x[i * n0 + j];
            }
            std::memcpy(x, tmp, static_cast<std::size_t>(n) * sizeof(float));
        }

        // libopus: compute_channel_weights() (celt/bands.c)
        // weights for the normalised distortion across channels: the amplitude weights the square
        // distortion (a quick perceptual compromise for inter-aural masking)
        void ComputeChannelWeights(float ex, float ey, float w[2]) noexcept
        {
            const float minE = Min32(ex, ey);
            // a bit more conservative
            ex   = ex + minE / 3;
            ey   = ey + minE / 3;
            w[0] = ex;
            w[1] = ey;
        }

        // libopus: intensity_stereo() (celt/bands.c)
        void IntensityStereo(float* x, const float* y, const float* bandE, std::int32_t bandId, std::int32_t n) noexcept
        {
            const std::int32_t i     = bandId;
            const float        left  = bandE[i];
            const float        right = bandE[i + NbEBands];
            const float        norm  = 1e-15f + static_cast<float>(std::sqrt(static_cast<double>(1e-15f + left * left + right * right)));
            const float        a1    = left / norm;
            const float        a2    = right / norm;
            for (std::int32_t j = 0; j < n; ++j)
                x[j] = a1 * x[j] + a2 * y[j]; // the side is not encoded
        }

        // libopus: stereo_split() (celt/bands.c)
        void StereoSplit(float* x, float* y, std::int32_t n) noexcept
        {
            for (std::int32_t j = 0; j < n; ++j) {
                const float l = .70710678f * x[j];
                const float r = .70710678f * y[j];
                x[j]          = l + r;
                y[j]          = r - l;
            }
        }

        // libopus: stereo_merge() (celt/bands.c)
        void StereoMerge(float* x, float* y, float mid, std::int32_t n) noexcept
        {
            // the norms of X+Y and X-Y as |X|^2 + |Y|^2 +/- sum(xy)
            float       xp   = InnerProd(y, x, n);
            const float side = InnerProd(y, y, n);
            // the mid normalisation
            xp             = mid * xp;
            const float el = mid * mid + side - 2 * xp;
            const float er = mid * mid + side + 2 * xp;
            if (er < 6e-4f || el < 6e-4f) {
                std::memcpy(y, x, static_cast<std::size_t>(n) * sizeof(float));
                return;
            }
            const float lgain = 1.f / static_cast<float>(std::sqrt(static_cast<double>(el)));
            const float rgain = 1.f / static_cast<float>(std::sqrt(static_cast<double>(er)));
            for (std::int32_t j = 0; j < n; ++j) {
                // the mid scaling (the side is already scaled)
                const float l = mid * x[j];
                const float r = y[j];
                x[j]          = lgain * (l - r);
                y[j]          = rgain * (l + r);
            }
        }

        // libopus: compute_qn() (celt/bands.c)
        std::int32_t ComputeQn(std::int32_t n, std::int32_t b, std::int32_t offset, std::int32_t pulseCap, bool stereo) noexcept
        {
            static constexpr std::int16_t exp2Table8[8] = {16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
            std::int32_t                  n2            = 2 * n - 1;
            if (stereo && n == 2)
                --n2;
            // the upper limit leaves, in a stereo split with itheta==16384, enough bits for at
            // least one pulse in the side (else it would collapse, not being folded)
            std::int32_t qb = Sudiv(b + n2 * offset, n2);
            qb              = Imin(b - pulseCap - (4 << BitRes), qb);
            qb              = Imin(8 << BitRes, qb);
            if (qb < (1 << BitRes >> 1))
                return 1;
            std::int32_t qn = exp2Table8[qb & 0x7] >> (14 - (qb >> BitRes));
            qn              = (qn + 1) >> 1 << 1;
            return qn;
        }

        struct BandCtx {
            bool                             resynth;
            std::int32_t                     i;
            std::int32_t                     intensity;
            std::int32_t                     spread;
            std::int32_t                     tfChange;
            RangeEncoder*                    ec;
            std::int32_t                     remainingBits;
            const float*                     bandE;
            std::uint32_t                    seed;
            std::int32_t                     thetaRound;
            std::int32_t                     disableInv;
            bool                             avoidSplitNoise;
            std::int32_t                     stereoItheta; // the band's stereo theta computed beforehand, or -1
            SeedPromise*                     seedPromise;  // the seed is still to come from there
            const std::atomic<std::int32_t>* abort;
        };

        // the seed at its first use, if it is still to come (the speculation, see
        // CeltSpeculation.h): waits until it is published, or until the run is cancelled
        inline void TakeSeed(BandCtx* ctx) noexcept
        {
            if (ctx->seedPromise == nullptr)
                return;
            while (ctx->seedPromise->ready.load(std::memory_order_acquire) == 0) {
                if (ctx->abort != nullptr && ctx->abort->load(std::memory_order_acquire) != 0)
                    break;
                _mm_pause();
            }
            ctx->seed = ctx->seedPromise->value.load(std::memory_order_acquire);
            ctx->seedPromise->used.store(1, std::memory_order_release);
            ctx->seedPromise = nullptr;
        }

        struct SplitCtx {
            std::int32_t inv;
            std::int32_t imid;
            std::int32_t iside;
            std::int32_t delta;
            std::int32_t itheta;
            std::int32_t qalloc;
        };

        // libopus: compute_theta() (celt/bands.c)
        void ComputeTheta(BandCtx* ctx, SplitCtx* sctx, float* x, float* y, std::int32_t n, std::int32_t* b, std::int32_t bb, std::int32_t b0, std::int32_t lm,
                          bool stereo, std::int32_t* fill) noexcept
        {
            const std::int32_t i      = ctx->i;
            RangeEncoder*      ec     = ctx->ec;
            std::int32_t       itheta = 0;
            std::int32_t       inv    = 0;
            std::int32_t       imid   = 0;
            std::int32_t       iside  = 0;
            std::int32_t       delta  = 0;

            // the resolution of the split parameter theta
            const std::int32_t pulseCap = CeltMode::LogN[i] + lm * (1 << BitRes);
            const std::int32_t offset   = (pulseCap >> 1) - (stereo && n == 2 ? QthetaOffsetTwophase : QthetaOffset);
            std::int32_t       qn       = ComputeQn(n, *b, offset, pulseCap, stereo);
            if (stereo && i >= ctx->intensity)
                qn = 1;
            // theta: the atan() of the ratio of the (normalised) side and mid; enough to rescale
            // both because they have unit norm and are orthogonal
            const std::int32_t ithetaQ30 = stereo && ctx->stereoItheta >= 0 ? ctx->stereoItheta : StereoItheta(x, y, stereo, n);
            itheta                       = ithetaQ30 >> 16;
            const auto tell              = static_cast<std::int32_t>(ec->TellFrac());
            if (qn != 1) {
                if (!stereo || ctx->thetaRound == 0) {
                    itheta = (itheta * qn + 8192) >> 14;
                    if (!stereo && ctx->avoidSplitNoise && itheta > 0 && itheta < qn) {
                        // a theta whose allocation injects noise on one side must leave that
                        // side's energy at zero
                        const std::int32_t unquantized = Udiv(itheta * 16384, qn);
                        imid                           = BitexactCos(static_cast<std::int16_t>(unquantized));
                        iside                          = BitexactCos(static_cast<std::int16_t>(16384 - unquantized));
                        delta                          = FracMul16((n - 1) << 7, BitexactLog2tan(iside, imid));
                        if (delta > *b)
                            itheta = qn;
                        else if (delta < -*b)
                            itheta = 0;
                    }
                } else {
                    // bias the quantisation towards itheta=0 and itheta=16384
                    const std::int32_t bias = itheta > 8192 ? 32767 / qn : -32767 / qn;
                    const std::int32_t down = Imin(qn - 1, Imax(0, (itheta * qn + bias) >> 14));
                    itheta                  = ctx->thetaRound < 0 ? down : down + 1;
                }
                // the angle: a uniform pdf for the time split, a step for stereo, a triangle otherwise
                if (stereo && n > 2) {
                    constexpr std::int32_t p0 = 3;
                    const std::int32_t     xx = itheta;
                    const std::int32_t     x0 = qn / 2;
                    const std::int32_t     ft = p0 * (x0 + 1) + x0;
                    // probability p0 up to itheta=8192, then 1
                    ec->Encode(static_cast<std::uint32_t>(xx <= x0 ? p0 * xx : (xx - 1 - x0) + (x0 + 1) * p0),
                               static_cast<std::uint32_t>(xx <= x0 ? p0 * (xx + 1) : (xx - x0) + (x0 + 1) * p0), static_cast<std::uint32_t>(ft));
                } else if (b0 > 1 || stereo) {
                    ec->EncodeUint(static_cast<std::uint32_t>(itheta), static_cast<std::uint32_t>(qn + 1));
                } else {
                    const std::int32_t ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
                    const std::int32_t fs = itheta <= (qn >> 1) ? itheta + 1 : qn + 1 - itheta;
                    const std::int32_t fl = itheta <= (qn >> 1) ? itheta * (itheta + 1) >> 1 : ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
                    ec->Encode(static_cast<std::uint32_t>(fl), static_cast<std::uint32_t>(fl + fs), static_cast<std::uint32_t>(ft));
                }
                itheta = Udiv(itheta * 16384, qn);
                if (stereo) {
                    if (itheta == 0)
                        IntensityStereo(x, y, ctx->bandE, i, n);
                    else
                        StereoSplit(x, y, n);
                }
            } else if (stereo) {
                inv = itheta > 8192 && ctx->disableInv == 0 ? 1 : 0;
                if (inv != 0) {
                    for (std::int32_t j = 0; j < n; ++j)
                        y[j] = -y[j];
                }
                IntensityStereo(x, y, ctx->bandE, i, n);
                if (*b > 2 << BitRes && ctx->remainingBits > 2 << BitRes)
                    ec->EncodeBitLogp(inv, 2);
                else
                    inv = 0;
                // no inversion against problems with downmixing
                if (ctx->disableInv != 0)
                    inv = 0;
                itheta = 0;
            }
            const std::int32_t qalloc = static_cast<std::int32_t>(ec->TellFrac()) - tell;
            *b -= qalloc;

            if (itheta == 0) {
                imid  = 32767;
                iside = 0;
                *fill &= (1 << bb) - 1;
                delta = -16384;
            } else if (itheta == 16384) {
                imid  = 0;
                iside = 32767;
                *fill &= ((1 << bb) - 1) << bb;
                delta = 16384;
            } else {
                imid  = BitexactCos(static_cast<std::int16_t>(itheta));
                iside = BitexactCos(static_cast<std::int16_t>(16384 - itheta));
                // the mid vs side allocation that minimises the squared error in the band
                delta = FracMul16((n - 1) << 7, BitexactLog2tan(iside, imid));
            }

            sctx->inv    = inv;
            sctx->imid   = imid;
            sctx->iside  = iside;
            sctx->delta  = delta;
            sctx->itheta = itheta;
            sctx->qalloc = qalloc;
        }

        // libopus: quant_band_n1() (celt/bands.c)
        std::uint32_t QuantBandN1(BandCtx* ctx, float* x, float* y, float* lowbandOut) noexcept
        {
            const bool stereo = y != nullptr;
            float*     xc     = x;
            for (std::int32_t c = 0; c < 1 + (stereo ? 1 : 0); ++c) {
                std::int32_t sign = 0;
                if (ctx->remainingBits >= 1 << BitRes) {
                    sign = xc[0] < 0 ? 1 : 0;
                    ctx->ec->EncodeBits(static_cast<std::uint32_t>(sign), 1);
                    ctx->remainingBits -= 1 << BitRes;
                }
                if (ctx->resynth)
                    xc[0] = sign != 0 ? -1.f : 1.f;
                xc = y;
            }
            if (lowbandOut != nullptr)
                lowbandOut[0] = x[0];
            return 1;
        }

        // libopus: quant_partition() (celt/bands.c)
        // A mono partition: possibly split in two with the energy difference of the halves
        // transmitted, recursively (a band can end up in 8 parts).
        std::uint32_t QuantPartition(BandCtx* ctx, float* x, std::int32_t n, std::int32_t b, std::int32_t bb, float* lowband, std::int32_t lm, float gain,
                                     std::int32_t fill) noexcept
        {
            const std::int32_t b0 = bb;
            const std::int32_t i  = ctx->i;
            std::uint32_t      cm = 0;

            // split the band in two if 1.5 more bits are needed than can be produced
            const std::uint8_t* cache = CeltMode::CacheBits + CeltMode::CacheIndex[(lm + 1) * NbEBands + i];
            if (lm != -1 && b > cache[cache[0]] + 12 && n > 2) {
                float* nextLowband2 = nullptr;
                n >>= 1;
                float* y = x + n;
                lm -= 1;
                if (bb == 1)
                    fill = (fill & 1) | (fill << 1);
                bb = (bb + 1) >> 1;

                SplitCtx sctx;
                ComputeTheta(ctx, &sctx, x, y, n, &b, bb, b0, lm, false, &fill);
                const std::int32_t itheta = sctx.itheta;
                std::int32_t       delta  = sctx.delta;
                const float        mid    = (1.f / 32768) * sctx.imid;
                const float        side   = (1.f / 32768) * sctx.iside;

                // more bits to low-energy MDCTs than they would otherwise deserve
                if (b0 > 1 && (itheta & 0x3fff) != 0) {
                    if (itheta > 8192)
                        delta -= delta >> (4 - lm); // a rough approximation of pre-echo masking
                    else
                        delta = Imin(0, delta + (n << BitRes >> (5 - lm))); // a forward-masking slope of 1.5 dB per 10 ms
                }
                std::int32_t mbits = Imax(0, Imin(b, (b - delta) / 2));
                std::int32_t sbits = b - mbits;
                ctx->remainingBits -= sctx.qalloc;

                if (lowband != nullptr)
                    nextLowband2 = lowband + n; // the >32-bit split case

                std::int32_t rebalance = ctx->remainingBits;
                if (mbits >= sbits) {
                    cm        = QuantPartition(ctx, x, n, mbits, bb, lowband, lm, gain * mid, fill);
                    rebalance = mbits - (rebalance - ctx->remainingBits);
                    if (rebalance > 3 << BitRes && itheta != 0)
                        sbits += rebalance - (3 << BitRes);
                    cm |= QuantPartition(ctx, y, n, sbits, bb, nextLowband2, lm, gain * side, fill >> bb) << (b0 >> 1);
                } else {
                    cm        = QuantPartition(ctx, y, n, sbits, bb, nextLowband2, lm, gain * side, fill >> bb) << (b0 >> 1);
                    rebalance = sbits - (rebalance - ctx->remainingBits);
                    if (rebalance > 3 << BitRes && itheta != 16384)
                        mbits += rebalance - (3 << BitRes);
                    cm |= QuantPartition(ctx, x, n, mbits, bb, lowband, lm, gain * mid, fill);
                }
                return cm;
            }

            // the basic no-split case
            std::int32_t q        = Bits2Pulses(i, lm, b);
            std::int32_t currBits = Pulses2Bits(i, lm, q);
            ctx->remainingBits -= currBits;
            // never bust the budget
            while (ctx->remainingBits < 0 && q > 0) {
                ctx->remainingBits += currBits;
                --q;
                currBits = Pulses2Bits(i, lm, q);
                ctx->remainingBits -= currBits;
            }

            if (q != 0) {
                const std::int32_t k = GetPulses(q);
                // the actual quantisation
                cm = AlgQuant(x, n, k, ctx->spread, bb, ctx->ec, gain, ctx->resynth);
            } else if (ctx->resynth) {
                // no pulse: fill the band anyway
                const auto cmMask = static_cast<std::uint32_t>((1ULL << bb) - 1);
                fill &= static_cast<std::int32_t>(cmMask);
                if (fill == 0) {
                    std::memset(x, 0, static_cast<std::size_t>(n) * sizeof(float));
                } else {
                    TakeSeed(ctx);
                    if (lowband == nullptr) {
                        // noise
                        for (std::int32_t j = 0; j < n; ++j) {
                            ctx->seed = LcgRand(ctx->seed);
                            x[j]      = static_cast<float>(static_cast<std::int32_t>(ctx->seed) >> 20);
                        }
                        cm = cmMask;
                    } else {
                        // folded spectrum, about 48 dB below the normal folding level
                        for (std::int32_t j = 0; j < n; ++j) {
                            ctx->seed = LcgRand(ctx->seed);
                            float tmp = 1.0f / 256;
                            tmp       = (ctx->seed & 0x8000) != 0 ? tmp : -tmp;
                            x[j]      = lowband[j] + tmp;
                        }
                        cm = static_cast<std::uint32_t>(fill);
                    }
                    RenormaliseVector(x, n, gain);
                }
            }
            return cm;
        }

        // libopus: quant_band() (celt/bands.c)
        std::uint32_t QuantBand(BandCtx* ctx, float* x, std::int32_t n, std::int32_t b, std::int32_t bb, float* lowband, std::int32_t lm, float* lowbandOut,
                                float gain, float* lowbandScratch, std::int32_t fill) noexcept
        {
            const std::int32_t n0         = n;
            std::int32_t       nB         = n;
            std::int32_t       b0         = bb;
            std::int32_t       timeDivide = 0;
            std::int32_t       recombine  = 0;
            std::int32_t       tfChange   = ctx->tfChange;
            const bool         longBlocks = b0 == 1;
            std::uint32_t      cm         = 0;

            nB = Udiv(nB, bb);

            // a single sample
            if (n == 1)
                return QuantBandN1(ctx, x, nullptr, lowbandOut);

            if (tfChange > 0)
                recombine = tfChange;
            // band recombining for more frequency resolution
            if (lowbandScratch != nullptr && lowband != nullptr && (recombine != 0 || ((nB & 1) == 0 && tfChange < 0) || b0 > 1)) {
                std::memcpy(lowbandScratch, lowband, static_cast<std::size_t>(n) * sizeof(float));
                lowband = lowbandScratch;
            }

            for (std::int32_t k = 0; k < recombine; ++k) {
                static constexpr std::uint8_t bitInterleaveTable[16] = {0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3};
                Haar1(x, n >> k, 1 << k);
                if (lowband != nullptr)
                    Haar1(lowband, n >> k, 1 << k);
                fill = bitInterleaveTable[fill & 0xF] | bitInterleaveTable[fill >> 4] << 2;
            }
            bb >>= recombine;
            nB <<= recombine;

            // more time resolution
            while ((nB & 1) == 0 && tfChange < 0) {
                Haar1(x, nB, bb);
                if (lowband != nullptr)
                    Haar1(lowband, nB, bb);
                fill |= fill << bb;
                bb <<= 1;
                nB >>= 1;
                ++timeDivide;
                ++tfChange;
            }
            b0                     = bb;
            const std::int32_t nB0 = nB;

            // the samples in time order instead of frequency order
            if (b0 > 1) {
                DeinterleaveHadamard(x, nB >> recombine, b0 << recombine, longBlocks);
                if (lowband != nullptr)
                    DeinterleaveHadamard(lowband, nB >> recombine, b0 << recombine, longBlocks);
            }

            cm = QuantPartition(ctx, x, n, b, bb, lowband, lm, gain, fill);

            // for the resynthesis (and the decoder)
            if (ctx->resynth) {
                // back from time order to frequency order
                if (b0 > 1)
                    InterleaveHadamard(x, nB >> recombine, b0 << recombine, longBlocks);

                // undo the time-frequency changes
                nB = nB0;
                bb = b0;
                for (std::int32_t k = 0; k < timeDivide; ++k) {
                    bb >>= 1;
                    nB <<= 1;
                    cm |= cm >> bb;
                    Haar1(x, nB, bb);
                }

                for (std::int32_t k = 0; k < recombine; ++k) {
                    static constexpr std::uint8_t bitDeinterleaveTable[16]
                        = {0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F, 0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF};
                    cm = bitDeinterleaveTable[cm];
                    Haar1(x, n0 >> k, 1 << k);
                }
                bb <<= recombine;

                // the output scaled for later folding
                if (lowbandOut != nullptr) {
                    const auto scale = static_cast<float>(std::sqrt(static_cast<double>(n0)));
                    for (std::int32_t j = 0; j < n0; ++j)
                        lowbandOut[j] = scale * x[j];
                }
                cm &= static_cast<std::uint32_t>((1 << bb) - 1);
            }
            return cm;
        }

        // libopus: quant_band_stereo() (celt/bands.c)
        std::uint32_t QuantBandStereo(BandCtx* ctx, float* x, float* y, std::int32_t n, std::int32_t b, std::int32_t bb, float* lowband, std::int32_t lm,
                                      float* lowbandOut, float* lowbandScratch, std::int32_t fill) noexcept
        {
            std::uint32_t cm = 0;

            // a single sample
            if (n == 1)
                return QuantBandN1(ctx, x, y, lowbandOut);

            const std::int32_t origFill = fill;

            if (ctx->bandE[ctx->i] < MinStereoEnergy || ctx->bandE[NbEBands + ctx->i] < MinStereoEnergy) {
                if (ctx->bandE[ctx->i] > ctx->bandE[NbEBands + ctx->i])
                    std::memcpy(y, x, static_cast<std::size_t>(n) * sizeof(float));
                else
                    std::memcpy(x, y, static_cast<std::size_t>(n) * sizeof(float));
            }
            SplitCtx sctx;
            ComputeTheta(ctx, &sctx, x, y, n, &b, bb, bb, lm, true, &fill);
            const std::int32_t inv    = sctx.inv;
            const std::int32_t itheta = sctx.itheta;
            const float        mid    = (1.f / 32768) * sctx.imid;
            const float        side   = (1.f / 32768) * sctx.iside;

            if (n == 2) {
                // N=2, stereo only: mid and side are orthogonal, so the side takes one bit
                std::int32_t mbits = b;
                std::int32_t sbits = 0;
                // one bit for the side
                if (itheta != 0 && itheta != 16384)
                    sbits = 1 << BitRes;
                mbits -= sbits;
                const bool c = itheta > 8192;
                ctx->remainingBits -= sctx.qalloc + sbits;

                float*       x2   = c ? y : x;
                float*       y2   = c ? x : y;
                std::int32_t sign = 0;
                if (sbits != 0) {
                    // only the sign of the side
                    sign = x2[0] * y2[1] - x2[1] * y2[0] < 0 ? 1 : 0;
                    ctx->ec->EncodeBits(static_cast<std::uint32_t>(sign), 1);
                }
                sign = 1 - 2 * sign;
                // orig_fill folds the side; with itheta==16384 the low bits of fill are cleared
                cm = QuantBand(ctx, x2, n, mbits, bb, lowband, lm, lowbandOut, 1.0f, lowbandScratch, origFill);
                // N=2 bands are not split, so cm is 1 or 0 (fold-collapse): no mixing with the
                // other channel to worry about
                y2[0] = -sign * x2[1];
                y2[1] = sign * x2[0];
                if (ctx->resynth) {
                    x[0]      = mid * x[0];
                    x[1]      = mid * x[1];
                    y[0]      = side * y[0];
                    y[1]      = side * y[1];
                    float tmp = x[0];
                    x[0]      = tmp - y[0];
                    y[0]      = tmp + y[0];
                    tmp       = x[1];
                    x[1]      = tmp - y[1];
                    y[1]      = tmp + y[1];
                }
            } else {
                // the normal split
                std::int32_t mbits = Imax(0, Imin(b, (b - sctx.delta) / 2));
                std::int32_t sbits = b - mbits;
                ctx->remainingBits -= sctx.qalloc;

                std::int32_t rebalance = ctx->remainingBits;
                if (mbits >= sbits) {
                    // no scaling of the mid in stereo: the normalised mid is needed for folding later
                    cm        = QuantBand(ctx, x, n, mbits, bb, lowband, lm, lowbandOut, 1.0f, lowbandScratch, fill);
                    rebalance = mbits - (rebalance - ctx->remainingBits);
                    if (rebalance > 3 << BitRes && itheta != 0)
                        sbits += rebalance - (3 << BitRes);
                    // the high bits of fill are always zero in a stereo split: no folding of the side
                    cm |= QuantBand(ctx, y, n, sbits, bb, nullptr, lm, nullptr, side, nullptr, fill >> bb);
                } else {
                    // the high bits of fill are always zero in a stereo split: no folding of the side
                    cm        = QuantBand(ctx, y, n, sbits, bb, nullptr, lm, nullptr, side, nullptr, fill >> bb);
                    rebalance = sbits - (rebalance - ctx->remainingBits);
                    if (rebalance > 3 << BitRes && itheta != 16384)
                        mbits += rebalance - (3 << BitRes);
                    // no scaling of the mid in stereo: the normalised mid is needed for folding later
                    cm |= QuantBand(ctx, x, n, mbits, bb, lowband, lm, lowbandOut, 1.0f, lowbandScratch, fill);
                }
            }

            // for the resynthesis (and the decoder)
            if (ctx->resynth) {
                if (n != 2)
                    StereoMerge(x, y, mid, n);
                if (inv != 0) {
                    for (std::int32_t j = 0; j < n; ++j)
                        y[j] = -y[j];
                }
            }
            return cm;
        }

        // libopus: special_hybrid_folding() (celt/bands.c)
        void SpecialHybridFolding(float* norm, float* norm2, std::int32_t start, std::int32_t m, bool dualStereo) noexcept
        {
            const std::int16_t* eBands = CeltMode::EBands;
            const std::int32_t  n1     = m * (eBands[start + 1] - eBands[start]);
            const std::int32_t  n2     = m * (eBands[start + 2] - eBands[start + 1]);
            // enough of the first band's folding data to fold the second band (none for CELT-only)
            if (n2 > n1) {
                std::memcpy(&norm[n1], &norm[2 * n1 - n2], static_cast<std::size_t>(n2 - n1) * sizeof(float));
                if (dualStereo)
                    std::memcpy(&norm2[n1], &norm2[2 * n1 - n2], static_cast<std::size_t>(n2 - n1) * sizeof(float));
            }
        }

    } // namespace

    // libopus: compute_theta() (celt/bands.c), the theta of a stereo band before its quantisation
    std::int32_t BandStereoItheta(const float* x, const float* y, std::int32_t n, float bandEx, float bandEy) noexcept
    {
        // the copy of quant_band_stereo
        if (bandEx < MinStereoEnergy || bandEy < MinStereoEnergy) {
            const float* louder = bandEx > bandEy ? x : y;
            return StereoItheta(louder, louder, true, n);
        }
        return StereoItheta(x, y, true, n);
    }

    struct ThetaTrial {
        // the state at the start of the band (copied by the trial), with its own coder (writing to bytes)
        const BandCtx*      ctxSave;
        const RangeEncoder* ecSave;
        BandCtx             ctx;
        RangeEncoder        ec;
        std::int32_t        n;
        std::int32_t        b;
        std::int32_t        bb;
        std::int32_t        lm;
        std::int32_t        fill;
        const float*        lowbandIn;  // read only (before the band's output)
        bool                lowbandOut; // the band's output for later folding (into out)
        const float*        xSave;
        const float*        ySave;
        float               w[2];
        // the result
        std::uint32_t cm;
        float         dist;
        float         x[8 * 22];
        float         y[8 * 22];
        float         out[8 * 22];
        float         scratch[8 * 22];
        std::uint8_t  bytes[1275];
        // the trial's coding operations, if the band's coder logs them
        static constexpr std::size_t MaxOps = 1024;
        SymbolLog                    log;
        SymbolLog::Op                logOps[MaxOps];
    };

    ThetaTrial* NewThetaTrial() noexcept
    {
        return new (std::nothrow) ThetaTrial;
    }

    void DeleteThetaTrial(ThetaTrial* trial) noexcept
    {
        delete trial;
    }

    void RunThetaTrial(ThetaTrial* t) noexcept
    {
        t->ctx            = *t->ctxSave;
        t->ctx.thetaRound = 1;
        t->ec             = *t->ecSave;
        t->ec.Rebase(t->bytes);
        t->log.Attach(t->logOps, ThetaTrial::MaxOps);
        t->ec.SetLog(t->ecSave->Log() != nullptr ? &t->log : nullptr);
        t->ctx.ec = &t->ec;
        std::memcpy(t->x, t->xSave, static_cast<std::size_t>(t->n) * sizeof(float));
        std::memcpy(t->y, t->ySave, static_cast<std::size_t>(t->n) * sizeof(float));
        // with a scratch buffer, the lowband is copied before it is changed
        t->cm   = QuantBandStereo(&t->ctx, t->x, t->y, t->n, t->b, t->bb, const_cast<float*>(t->lowbandIn), t->lm, t->lowbandOut ? t->out : nullptr, t->scratch,
                                  t->fill);
        t->dist = t->w[0] * InnerProd(t->xSave, t->x, t->n) + t->w[1] * InnerProd(t->ySave, t->y, t->n);
    }

    // libopus: quant_all_bands() (celt/bands.c) (encoding)
    void QuantAllBands(std::int32_t start, std::int32_t end, float* xAll, float* yAll, std::uint8_t* collapseMasks, const float* bandE,
                       const std::int32_t* pulses, std::int32_t shortBlocks, std::int32_t spread, std::int32_t dualStereo, std::int32_t intensity,
                       const std::int32_t* tfRes, std::int32_t totalBits, std::int32_t balance, RangeEncoder* ec, std::int32_t lm, std::int32_t codedBands,
                       std::uint32_t* seed, std::int32_t complexity, std::int32_t disableInv, ThetaTrialRunner* runner, const std::int32_t* stereoItheta,
                       SeedPromise* seedPromise, const std::atomic<std::int32_t>* abort) noexcept
    {
        const std::int16_t* eBands        = CeltMode::EBands;
        const std::int32_t  m             = 1 << lm;
        const std::int32_t  bb            = shortBlocks != 0 ? m : 1;
        const std::int32_t  normOffset    = m * eBands[start];
        const std::int32_t  c             = yAll != nullptr ? 2 : 1;
        const bool          thetaRdo      = yAll != nullptr && dualStereo == 0 && complexity >= 8;
        const bool          resynth       = thetaRdo;
        bool                updateLowband = true;
        std::int32_t        lowbandOffset = 0;

        // no norm for the last band: no output is needed there
        float        normBuf[2 * 8 * 78];
        float* const norm  = normBuf;
        float* const norm2 = norm + m * eBands[NbEBands - 1] - normOffset;

        float        lowbandScratchBuf[8 * 22];
        float        xSave[8 * 22];
        float        ySave[8 * 22];
        float        xSave2[8 * 22];
        float        ySave2[8 * 22];
        float        normSave2[8 * 22];
        float*       lowbandScratch = resynth ? lowbandScratchBuf : xAll + m * eBands[CeltMode::EffEBands - 1];
        std::uint8_t bytesSave[1275];

        BandCtx ctx;
        ctx.bandE         = bandE;
        ctx.ec            = ec;
        ctx.intensity     = intensity;
        ctx.seed          = *seed;
        ctx.spread        = spread;
        ctx.disableInv    = disableInv;
        ctx.resynth       = resynth;
        ctx.thetaRound    = 0;
        ctx.tfChange      = 0;
        ctx.i             = 0;
        ctx.remainingBits = 0;
        ctx.stereoItheta  = -1;
        ctx.seedPromise   = seedPromise;
        ctx.abort         = abort;

        // no noise injected into the first band on transients
        ctx.avoidSplitNoise = bb > 1;
        for (std::int32_t i = start; i < end; ++i) {
            if (abort != nullptr && abort->load(std::memory_order_acquire) != 0)
                break;
            ctx.i                   = i;
            const bool         last = i == end - 1;
            float*             x    = xAll + m * eBands[i];
            float*             y    = yAll != nullptr ? yAll + m * eBands[i] : nullptr;
            const std::int32_t n    = m * eBands[i + 1] - m * eBands[i];
            const auto         tell = static_cast<std::int32_t>(ec->TellFrac());

            // the bits for this band
            if (i != start)
                balance -= tell;
            const std::int32_t remainingBits = totalBits - tell - 1;
            ctx.remainingBits                = remainingBits;
            std::int32_t b                   = 0;
            if (i <= codedBands - 1) {
                const std::int32_t currBalance = Sudiv(balance, Imin(3, codedBands - i));
                b                              = Imax(0, Imin(16383, Imin(remainingBits + 1, pulses[i] + currBalance)));
            }

            if (resynth && (m * eBands[i] - n >= m * eBands[start] || i == start + 1) && (updateLowband || lowbandOffset == 0))
                lowbandOffset = i;
            if (i == start + 1)
                SpecialHybridFolding(norm, norm2, start, m, dualStereo != 0);

            const std::int32_t tfChange = tfRes[i];
            ctx.tfChange                = tfChange;
            ctx.stereoItheta            = stereoItheta != nullptr && resynth && i < CeltMode::EffEBands ? stereoItheta[i] : -1;
            if (i >= CeltMode::EffEBands) {
                x = norm;
                if (yAll != nullptr)
                    y = norm;
                lowbandScratch = nullptr;
            }
            if (last && !thetaRdo)
                lowbandScratch = nullptr;

            // a conservative estimate of the collapse masks of the bands folded from
            std::int32_t  effectiveLowband = -1;
            std::uint32_t xCm              = 0;
            std::uint32_t yCm              = 0;
            if (lowbandOffset != 0 && (spread != SpreadAggressive || bb > 1 || tfChange < 0)) {
                // never repeat spectral content within one band
                effectiveLowband       = Imax(0, m * eBands[lowbandOffset] - normOffset - n);
                std::int32_t foldStart = lowbandOffset;
                while (m * eBands[--foldStart] > effectiveLowband + normOffset) {
                }
                std::int32_t foldEnd = lowbandOffset - 1;
                while (++foldEnd < i && m * eBands[foldEnd] < effectiveLowband + normOffset + n) {
                }
                std::int32_t foldI = foldStart;
                do {
                    xCm |= collapseMasks[foldI * c + 0];
                    yCm |= collapseMasks[foldI * c + c - 1];
                } while (++foldI < foldEnd);
            } else {
                // folding with the LCG: all blocks (almost always) non-zero
                xCm = yCm = static_cast<std::uint32_t>((1 << bb) - 1);
            }

            if (dualStereo != 0 && i == intensity) {
                // dual stereo off for intensity
                dualStereo = 0;
                if (resynth)
                    for (std::int32_t j = 0; j < m * eBands[i] - normOffset; ++j)
                        norm[j] = .5f * (norm[j] + norm2[j]);
            }
            float* const lowbandIn  = effectiveLowband != -1 ? norm + effectiveLowband : nullptr;
            float* const lowbandOut = last ? nullptr : norm + m * eBands[i] - normOffset;
            if (dualStereo != 0) {
                xCm = QuantBand(&ctx, x, n, b / 2, bb, lowbandIn, lm, lowbandOut, 1.0f, lowbandScratch, static_cast<std::int32_t>(xCm));
                yCm = QuantBand(&ctx, y, n, b / 2, bb, effectiveLowband != -1 ? norm2 + effectiveLowband : nullptr, lm,
                                last ? nullptr : norm2 + m * eBands[i] - normOffset, 1.0f, lowbandScratch, static_cast<std::int32_t>(yCm));
            } else {
                if (y != nullptr) {
                    if (thetaRdo && i < intensity) {
                        float w[2];
                        ComputeChannelWeights(bandE[i], bandE[i + NbEBands], w);
                        // a copy
                        const std::uint32_t cm       = xCm | yCm;
                        const RangeEncoder  ecSave   = *ec;
                        SymbolLog* const    log      = ec->Log();
                        const std::size_t   logStart = log != nullptr ? log->Size() : 0;
                        const BandCtx       ctxSave  = ctx;
                        std::memcpy(xSave, x, static_cast<std::size_t>(n) * sizeof(float));
                        std::memcpy(ySave, y, static_cast<std::size_t>(n) * sizeof(float));
                        // the round-up trial on the runner, unless it would read what the round-down one writes
                        ThetaTrial* const trial = runner != nullptr && i < CeltMode::EffEBands && ec->Storage() <= 1275
                                && (lowbandIn == nullptr || lowbandOut == nullptr || lowbandIn + n <= lowbandOut)
                            ? runner->Trial()
                            : nullptr;
                        if (trial != nullptr) {
                            trial->ctxSave    = &ctxSave;
                            trial->ecSave     = &ecSave;
                            trial->n          = n;
                            trial->b          = b;
                            trial->bb         = bb;
                            trial->lm         = lm;
                            trial->fill       = static_cast<std::int32_t>(cm);
                            trial->lowbandIn  = lowbandIn;
                            trial->lowbandOut = lowbandOut != nullptr;
                            trial->xSave      = xSave;
                            trial->ySave      = ySave;
                            trial->w[0]       = w[0];
                            trial->w[1]       = w[1];
                            runner->Start(trial);
                        }
                        // encode and round down
                        ctx.thetaRound    = -1;
                        xCm               = QuantBandStereo(&ctx, x, y, n, b, bb, lowbandIn, lm, lowbandOut, lowbandScratch, static_cast<std::int32_t>(cm));
                        const float dist0 = w[0] * InnerProd(xSave, x, n) + w[1] * InnerProd(ySave, y, n);

                        if (trial != nullptr) {
                            runner->Finish();
                            if (!(dist0 >= trial->dist)) {
                                // the round-up result: its bytes over those of the round-down trial, as in place
                                std::uint8_t* const buffer  = ec->Buffer();
                                const std::uint32_t storage = ec->Storage();
                                std::memcpy(buffer + ecSave.RangeBytes(), trial->bytes + ecSave.RangeBytes(), trial->ec.RangeBytes() - ecSave.RangeBytes());
                                std::memcpy(buffer + storage - trial->ec.EndBytes(), trial->bytes + storage - trial->ec.EndBytes(),
                                            trial->ec.EndBytes() - ecSave.EndBytes());
                                *ec = trial->ec;
                                ec->Rebase(buffer);
                                ec->SetLog(log);
                                if (log != nullptr) {
                                    log->Truncate(logStart);
                                    log->AppendFrom(trial->log, 0, trial->log.Size());
                                }
                                ctx    = trial->ctx;
                                ctx.ec = ec;
                                xCm    = trial->cm;
                                // x and y of a coded band are not read again (later bands fold from norm)
                                if (lowbandOut != nullptr)
                                    std::memcpy(lowbandOut, trial->out, static_cast<std::size_t>(n) * sizeof(float));
                            }
                        } else {
                            // the first result
                            const std::uint32_t cm2      = xCm;
                            const RangeEncoder  ecSave2  = *ec;
                            const std::size_t   logMid   = log != nullptr ? log->Size() : 0;
                            const BandCtx       ctxSave2 = ctx;
                            std::memcpy(xSave2, x, static_cast<std::size_t>(n) * sizeof(float));
                            std::memcpy(ySave2, y, static_cast<std::size_t>(n) * sizeof(float));
                            if (!last)
                                std::memcpy(normSave2, norm + m * eBands[i] - normOffset, static_cast<std::size_t>(n) * sizeof(float));
                            const std::uint32_t nstartBytes = ecSave.RangeBytes();
                            const std::uint32_t nendBytes   = ecSave.Storage();
                            std::uint8_t* const bytesBuf    = ecSave.Buffer() + nstartBytes;
                            const std::uint32_t saveBytes   = nendBytes - nstartBytes;
                            std::memcpy(bytesSave, bytesBuf, saveBytes);
                            // restore
                            *ec = ecSave;
                            ctx = ctxSave;
                            std::memcpy(x, xSave, static_cast<std::size_t>(n) * sizeof(float));
                            std::memcpy(y, ySave, static_cast<std::size_t>(n) * sizeof(float));
                            if (i == start + 1)
                                SpecialHybridFolding(norm, norm2, start, m, dualStereo != 0);
                            // encode and round up
                            ctx.thetaRound    = 1;
                            xCm               = QuantBandStereo(&ctx, x, y, n, b, bb, lowbandIn, lm, lowbandOut, lowbandScratch, static_cast<std::int32_t>(cm));
                            const float dist1 = w[0] * InnerProd(xSave, x, n) + w[1] * InnerProd(ySave, y, n);
                            if (dist0 >= dist1) {
                                xCm = cm2;
                                *ec = ecSave2;
                                ctx = ctxSave2;
                                std::memcpy(x, xSave2, static_cast<std::size_t>(n) * sizeof(float));
                                std::memcpy(y, ySave2, static_cast<std::size_t>(n) * sizeof(float));
                                if (!last)
                                    std::memcpy(norm + m * eBands[i] - normOffset, normSave2, static_cast<std::size_t>(n) * sizeof(float));
                                std::memcpy(bytesBuf, bytesSave, saveBytes);
                                if (log != nullptr)
                                    log->Truncate(logMid);
                            } else if (log != nullptr) {
                                log->Erase(logStart, logMid);
                            }
                        }
                    } else {
                        ctx.thetaRound = 0;
                        // the resynthesis is there for the theta RDO only: after its last band it
                        // would feed nothing but the resynthesis of later bands (x and y are not
                        // read again, the collapse masks only steer the filling, the seed is
                        // replaced at the end of the frame), so none of it from here on
                        const bool noResynth = thetaRdo && i >= intensity;
                        if (noResynth)
                            ctx.resynth = false;
                        xCm = QuantBandStereo(&ctx, x, y, n, b, bb, noResynth ? nullptr : lowbandIn, lm, noResynth ? nullptr : lowbandOut, lowbandScratch,
                                              static_cast<std::int32_t>(xCm | yCm));
                    }
                } else {
                    xCm = QuantBand(&ctx, x, n, b, bb, lowbandIn, lm, lowbandOut, 1.0f, lowbandScratch, static_cast<std::int32_t>(xCm | yCm));
                }
                yCm = xCm;
            }
            collapseMasks[i * c + 0]     = static_cast<std::uint8_t>(xCm);
            collapseMasks[i * c + c - 1] = static_cast<std::uint8_t>(yCm);
            balance += pulses[i] + tell;

            // the folding position moves only while there is 1 bit/sample of depth
            updateLowband = b > (n << BitRes);
            // noise on a split is avoided only in the first band; after that there is folding
            ctx.avoidSplitNoise = false;
        }
        *seed = ctx.seed;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
