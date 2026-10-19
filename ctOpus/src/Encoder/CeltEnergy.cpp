// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "CeltEnergy.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "CeltMath.h"
#include "CeltMode.h"
#include "IntMath.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    const float EMeans[25]
        = {6.437500f, 6.250000f, 5.750000f, 5.312500f, 5.062500f, 4.812500f, 4.500000f, 4.375000f, 4.875000f, 4.687500f, 4.562500f, 4.437500f, 4.875000f,
           4.625000f, 4.312500f, 4.500000f, 4.375000f, 4.625000f, 4.750000f, 4.437500f, 3.750000f, 3.750000f, 3.750000f, 3.750000f, 3.750000f};

    namespace {

        constexpr std::int32_t NbEBands = CeltMode::NbEBands;

        // prediction coefficients 0.9, 0.8, 0.65, 0.5 (as the float build rounds them)
        constexpr float PredCoef[4]
            = {static_cast<float>(29440 / 32768.), static_cast<float>(26112 / 32768.), static_cast<float>(21248 / 32768.), static_cast<float>(16384 / 32768.)};
        constexpr float BetaCoef[4]
            = {static_cast<float>(30147 / 32768.), static_cast<float>(22282 / 32768.), static_cast<float>(12124 / 32768.), static_cast<float>(6554 / 32768.)};
        constexpr float BetaIntra = static_cast<float>(4915 / 32768.);

        // The Laplace-like models of the coarse energy: per frame size, prediction type
        // (inter/intra) and band, the probability of 0 and the decay rate, both in Q8.
        constexpr std::uint8_t EProbModel[4][2][42] = {
            // 120 sample frames
            {{72, 127, 65, 129, 66, 128, 65, 128, 64, 128, 62, 128, 64, 128, 64, 128, 92, 78,  92, 79,  92,
              78, 90,  79, 116, 41, 115, 40, 114, 40, 132, 26, 132, 26, 145, 17, 161, 12, 176, 10, 177, 11},
             {24, 179, 48, 138, 54, 135, 54, 132, 53, 134, 56, 133, 55, 132, 55, 132, 61, 114, 70, 96, 74,
              88, 75,  88, 87,  74, 89,  66, 91,  67, 100, 59, 108, 50, 120, 40, 122, 37, 97,  43, 78, 50}},
            // 240 sample frames
            {{83, 78,  84, 81,  88, 75,  86, 74,  87, 71,  90, 73,  93, 74,  93, 74,  109, 40,  114, 36,  117,
              34, 117, 34, 143, 17, 145, 18, 146, 19, 162, 12, 165, 10, 178, 7,  189, 6,   190, 8,   177, 9},
             {23, 178, 54, 115, 63, 102, 66, 98,  69, 99,  74, 89,  71, 91,  73, 91,  78, 89, 86, 80, 92,
              66, 93,  64, 102, 59, 103, 60, 104, 60, 117, 52, 123, 44, 138, 35, 133, 31, 97, 38, 77, 45}},
            // 480 sample frames
            {{61, 90,  93, 60,  105, 42,  107, 41,  110, 45,  116, 38,  113, 38,  112, 38,  124, 26,  132, 27,  136,
              19, 140, 20, 155, 14,  159, 16,  158, 18,  170, 13,  177, 10,  187, 8,   192, 6,   175, 9,   159, 10},
             {21, 178, 59, 110, 71, 86,  75, 85,  84, 83,  91, 66,  88, 73,  87, 72,  92, 75, 98, 72, 105,
              58, 107, 54, 115, 52, 114, 55, 112, 56, 129, 51, 132, 40, 150, 33, 140, 29, 98, 35, 77, 42}},
            // 960 sample frames
            {{42, 121, 96, 66,  108, 43,  111, 40,  117, 44,  123, 32,  120, 36,  119, 33,  127, 33,  134, 34,  139,
              21, 147, 23, 152, 20,  158, 25,  154, 26,  166, 21,  173, 16,  184, 13,  184, 10,  150, 13,  139, 15},
             {22, 178, 63, 114, 74, 82,  84, 83,  92, 82,  103, 62,  96, 72,  96, 67,  101, 73, 107, 72, 113,
              55, 118, 52, 125, 52, 118, 52, 117, 55, 135, 49,  137, 39, 157, 32, 145, 29,  97, 33,  77, 40}},
        };

        constexpr std::uint8_t SmallEnergyIcdf[3] = {2, 1, 0};

        // --- laplace.c ----------------------------------------------------------------------

        constexpr std::uint32_t LaplaceLogMinp = 0;
        constexpr std::uint32_t LaplaceMinp    = 1U << LaplaceLogMinp;
        constexpr std::uint32_t LaplaceNmin    = 16; // guaranteed representable deltas in one direction

        // libopus: ec_laplace_get_freq1() (celt/laplace.c)
        // decay is positive and at most 11456
        std::uint32_t LaplaceGetFreq1(std::uint32_t fs0, std::int32_t decay) noexcept
        {
            const std::uint32_t ft = 32768 - LaplaceMinp * (2 * LaplaceNmin) - fs0;
            return ft * static_cast<std::uint32_t>(16384 - decay) >> 15;
        }

        // libopus: ec_laplace_encode() (celt/laplace.c)
        void LaplaceEncode(RangeEncoder* enc, std::int32_t* value, std::uint32_t fs, std::int32_t decay) noexcept
        {
            std::uint32_t fl  = 0;
            std::int32_t  val = *value;
            if (val != 0) {
                const std::int32_t s = -static_cast<std::int32_t>(val < 0);
                val                  = (val + s) ^ s;
                fl                   = fs;
                fs                   = LaplaceGetFreq1(fs, decay);
                // the decaying part of the PDF
                std::int32_t i = 1;
                for (i = 1; fs > 0 && i < val; ++i) {
                    fs *= 2;
                    fl += fs + 2 * LaplaceMinp;
                    fs = (fs * static_cast<std::uint32_t>(decay)) >> 15;
                }
                // everything beyond has probability LAPLACE_MINP
                if (fs == 0) {
                    std::int32_t ndiMax   = static_cast<std::int32_t>((32768 - fl + LaplaceMinp - 1) >> LaplaceLogMinp);
                    ndiMax                = (ndiMax - s) >> 1;
                    const std::int32_t di = Imin(val - i, ndiMax - 1);
                    fl += static_cast<std::uint32_t>((2 * di + 1 + s) * static_cast<std::int32_t>(LaplaceMinp));
                    fs     = LaplaceMinp < 32768 - fl ? LaplaceMinp : 32768 - fl;
                    *value = (i + di + s) ^ s;
                } else {
                    fs += LaplaceMinp;
                    fl += fs & ~static_cast<std::uint32_t>(s);
                }
            }
            enc->EncodeBin(fl, fl + fs, 15);
        }

        // libopus: loss_distortion() (celt/quant_bands.c)
        float LossDistortion(const float* eBands, const float* oldEBands, std::int32_t start, std::int32_t end, std::int32_t len, std::int32_t c) noexcept
        {
            float dist = 0;
            for (std::int32_t ch = 0; ch < c; ++ch) {
                for (std::int32_t i = start; i < end; ++i) {
                    const float d = eBands[i + ch * len] - oldEBands[i + ch * len];
                    dist          = dist + d * d;
                }
            }
            return Min32(200, dist);
        }

        // libopus: quant_coarse_energy_impl() (celt/quant_bands.c)
        std::int32_t QuantCoarseEnergyImpl(std::int32_t start, std::int32_t end, const float* eBands, float* oldEBands, std::int32_t budget, std::int32_t tell,
                                           const std::uint8_t* probModel, float* error, RangeEncoder* enc, std::int32_t c, std::int32_t lm, bool intra,
                                           float maxDecay, std::int32_t lfe) noexcept
        {
            std::int32_t badness = 0;
            float        prev[2] = {0, 0};

            if (tell + 3 <= budget)
                enc->EncodeBitLogp(intra ? 1 : 0, 3);
            const float coef = intra ? 0 : PredCoef[lm];
            const float beta = intra ? BetaIntra : BetaCoef[lm];

            // a fixed coarse resolution
            for (std::int32_t i = start; i < end; ++i) {
                for (std::int32_t ch = 0; ch < c; ++ch) {
                    const float x    = eBands[i + ch * NbEBands];
                    const float oldE = Max32(-9.f, oldEBands[i + ch * NbEBands]);
                    const float f    = x - coef * oldE - prev[ch];
                    // rounding to the nearest integer is really important here
                    std::int32_t qi         = static_cast<std::int32_t>(std::floor(.5f + f));
                    const float  decayBound = Max32(-28.f, oldEBands[i + ch * NbEBands]) - maxDecay;
                    // the energy must not drop too quickly (e.g. in bands of one bin)
                    if (qi < 0 && x < decayBound) {
                        qi += static_cast<std::int32_t>(decayBound - x);
                        if (qi > 0)
                            qi = 0;
                    }
                    const std::int32_t qi0 = qi;
                    // without enough bits for all the energy, assume something safe
                    tell                        = enc->Tell();
                    const std::int32_t bitsLeft = budget - tell - 3 * c * (end - i);
                    if (i != start && bitsLeft < 30) {
                        if (bitsLeft < 24)
                            qi = Imin(1, qi);
                        if (bitsLeft < 16)
                            qi = Imax(-1, qi);
                    }
                    if (lfe != 0 && i >= 2)
                        qi = Imin(qi, 0);
                    if (budget - tell >= 15) {
                        const std::int32_t pi = 2 * Imin(i, 20);
                        LaplaceEncode(enc, &qi, static_cast<std::uint32_t>(probModel[pi]) << 7, probModel[pi + 1] << 6);
                    } else if (budget - tell >= 2) {
                        qi = Imax(-1, Imin(qi, 1));
                        enc->EncodeIcdf(2 * qi ^ -static_cast<std::int32_t>(qi < 0), SmallEnergyIcdf, 2);
                    } else if (budget - tell >= 1) {
                        qi = Imin(0, qi);
                        enc->EncodeBitLogp(-qi, 1);
                    } else {
                        qi = -1;
                    }
                    error[i + ch * NbEBands] = f - static_cast<float>(qi);
                    badness += std::abs(qi0 - qi);
                    const auto q = static_cast<float>(qi);

                    const float tmp              = coef * oldE + prev[ch] + q;
                    oldEBands[i + ch * NbEBands] = tmp;
                    prev[ch]                     = prev[ch] + q - beta * q;
                }
            }
            return lfe != 0 ? 0 : badness;
        }

    } // namespace

    // libopus: amp2Log2() (celt/quant_bands.c)
    void Amp2Log2(std::int32_t effEnd, std::int32_t end, const float* bandE, float* bandLogE, std::int32_t c) noexcept
    {
        for (std::int32_t ch = 0; ch < c; ++ch) {
            for (std::int32_t i = 0; i < effEnd; ++i)
                bandLogE[i + ch * NbEBands] = static_cast<float>(1.442695040888963387 * std::log(static_cast<double>(bandE[i + ch * NbEBands]))) - EMeans[i];
            for (std::int32_t i = effEnd; i < end; ++i)
                bandLogE[ch * NbEBands + i] = -14.f;
        }
    }

    // libopus: quant_coarse_energy() (celt/quant_bands.c)
    void QuantCoarseEnergy(std::int32_t start, std::int32_t end, std::int32_t effEnd, const float* eBands, float* oldEBands, std::uint32_t budget, float* error,
                           RangeEncoder* enc, std::int32_t c, std::int32_t lm, std::int32_t nbAvailableBytes, std::int32_t forceIntra, float* delayedIntra,
                           bool twoPass, std::int32_t lossRate, std::int32_t lfe) noexcept
    {
        bool        intra         = forceIntra != 0 || (!twoPass && *delayedIntra > 2 * c * (end - start) && nbAvailableBytes > (end - start) * c);
        const auto  intraBias     = static_cast<std::int32_t>((budget * *delayedIntra * lossRate) / (c * 512));
        const float newDistortion = LossDistortion(eBands, oldEBands, start, effEnd, NbEBands, c);

        const auto tell = static_cast<std::uint32_t>(enc->Tell());
        if (tell + 3 > budget)
            twoPass = intra = false;

        float maxDecay = 16.f;
        if (end - start > 10)
            maxDecay = Min32(maxDecay, .125f * nbAvailableBytes);
        if (lfe != 0)
            maxDecay = 3.f;
        const RangeEncoder encStartState = *enc;
        SymbolLog* const   log           = enc->Log();
        const std::size_t  logStart      = log != nullptr ? log->Size() : 0;

        float oldEBandsIntra[2 * NbEBands];
        float errorIntra[2 * NbEBands];
        std::memcpy(oldEBandsIntra, oldEBands, static_cast<std::size_t>(c * NbEBands) * sizeof(float));

        std::int32_t badness1 = 0;
        if (twoPass || intra)
            badness1 = QuantCoarseEnergyImpl(start, end, eBands, oldEBandsIntra, static_cast<std::int32_t>(budget), static_cast<std::int32_t>(tell),
                                             EProbModel[lm][1], errorIntra, enc, c, lm, true, maxDecay, lfe);

        if (!intra) {
            const auto          tellIntra     = static_cast<std::int32_t>(enc->TellFrac());
            const RangeEncoder  encIntraState = *enc;
            const std::uint32_t nstartBytes   = encStartState.RangeBytes();
            const std::uint32_t nintraBytes   = encIntraState.RangeBytes();
            std::uint8_t* const intraBuf      = encIntraState.Buffer() + nstartBytes;
            const std::uint32_t saveBytes     = nintraBytes - nstartBytes;
            std::uint8_t        intraBits[1275];
            // the bits of the intra bit-stream
            std::memcpy(intraBits, intraBuf, saveBytes);
            const std::size_t logIntra = log != nullptr ? log->Size() : 0;

            *enc = encStartState;

            const std::int32_t badness2 = QuantCoarseEnergyImpl(start, end, eBands, oldEBands, static_cast<std::int32_t>(budget),
                                                                static_cast<std::int32_t>(tell), EProbModel[lm][0], error, enc, c, lm, false, maxDecay, lfe);

            if (twoPass && (badness1 < badness2 || (badness1 == badness2 && static_cast<std::int32_t>(enc->TellFrac()) + intraBias > tellIntra))) {
                *enc = encIntraState;
                // the intra bits back into the bit-stream (and the log)
                std::memcpy(intraBuf, intraBits, saveBytes);
                if (log != nullptr)
                    log->Truncate(logIntra);
                std::memcpy(oldEBands, oldEBandsIntra, static_cast<std::size_t>(c * NbEBands) * sizeof(float));
                std::memcpy(error, errorIntra, static_cast<std::size_t>(c * NbEBands) * sizeof(float));
                intra = true;
            } else if (log != nullptr) {
                log->Erase(logStart, logIntra);
            }
        } else {
            std::memcpy(oldEBands, oldEBandsIntra, static_cast<std::size_t>(c * NbEBands) * sizeof(float));
            std::memcpy(error, errorIntra, static_cast<std::size_t>(c * NbEBands) * sizeof(float));
        }

        if (intra)
            *delayedIntra = newDistortion;
        else
            *delayedIntra = (PredCoef[lm] * PredCoef[lm]) * *delayedIntra + newDistortion;
    }

    // libopus: quant_fine_energy() (celt/quant_bands.c)
    void QuantFineEnergy(std::int32_t start, std::int32_t end, float* oldEBands, float* error, const std::int32_t* extraQuant, RangeEncoder* enc,
                         std::int32_t c) noexcept
    {
        constexpr std::int32_t prev = 0; // prev_quant is NULL
        // a finer resolution
        for (std::int32_t i = start; i < end; ++i) {
            const auto extra = static_cast<std::int16_t>(1 << extraQuant[i]);
            if (extraQuant[i] <= 0)
                continue;
            if (enc->Tell() + c * extraQuant[i] > static_cast<std::int32_t>(enc->Storage()) * 8)
                continue;
            for (std::int32_t ch = 0; ch < c; ++ch) {
                std::int32_t q2 = static_cast<std::int32_t>(std::floor((error[i + ch * NbEBands] * (1 << prev) + .5f) * extra));
                if (q2 > extra - 1)
                    q2 = extra - 1;
                if (q2 < 0)
                    q2 = 0;
                enc->EncodeBits(static_cast<std::uint32_t>(q2), static_cast<std::uint32_t>(extraQuant[i]));
                float offset = (q2 + .5f) * (1 << (14 - extraQuant[i])) * (1.f / 16384) - .5f;
                offset *= (1 << (14 - prev)) * (1.f / 16384);
                oldEBands[i + ch * NbEBands] += offset;
                error[i + ch * NbEBands] -= offset;
            }
        }
    }

    // libopus: quant_energy_finalise() (celt/quant_bands.c)
    void QuantEnergyFinalise(std::int32_t start, std::int32_t end, float* oldEBands, float* error, const std::int32_t* fineQuant,
                             const std::int32_t* finePriority, std::int32_t bitsLeft, RangeEncoder* enc, std::int32_t c) noexcept
    {
        // the remaining bits
        for (std::int32_t prio = 0; prio < 2; ++prio) {
            for (std::int32_t i = start; i < end && bitsLeft >= c; ++i) {
                if (fineQuant[i] >= MaxFineBits || finePriority[i] != prio)
                    continue;
                for (std::int32_t ch = 0; ch < c; ++ch) {
                    const std::int32_t q2 = error[i + ch * NbEBands] < 0 ? 0 : 1;
                    enc->EncodeBits(static_cast<std::uint32_t>(q2), 1);
                    const float offset = (q2 - .5f) * (1 << (14 - fineQuant[i] - 1)) * (1.f / 16384);
                    if (oldEBands != nullptr)
                        oldEBands[i + ch * NbEBands] += offset;
                    error[i + ch * NbEBands] -= offset;
                    --bitsLeft;
                }
            }
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
