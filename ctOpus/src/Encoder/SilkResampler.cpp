// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkResampler.h"

#include <cstring>

#include "SilkMacros.h"
#include "SilkTables.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    namespace {

        constexpr std::int32_t DownOrderFir0  = 18;
        constexpr std::int32_t DownOrderFir1  = 24;
        constexpr std::int32_t DownOrderFir2  = 36;
        constexpr std::int32_t OrderFir12     = 8;
        constexpr std::int32_t MaxBatchSizeMs = 10;
        constexpr std::int32_t MaxBatchSizeIn = MaxBatchSizeMs * 48;

        constexpr std::int32_t UseCopy         = 0;
        constexpr std::int32_t UseUp2HqWrapper = 1;
        constexpr std::int32_t UseIirFir       = 2;
        constexpr std::int32_t UseDownFir      = 3;

        // delay compensation, equalising the total delay of the modes (in \ out: 8, 12, 16 kHz)
        constexpr std::int8_t DelayMatrixEnc[6][3] = {{6, 0, 3}, {0, 7, 3}, {0, 1, 10}, {0, 2, 6}, {18, 10, 12}, {0, 0, 44}};
        constexpr std::int8_t DelayMatrixDec[3][6] = {{4, 0, 2, 0, 0, 0}, {0, 9, 4, 7, 4, 4}, {0, 3, 12, 7, 7, 7}};

        // [8000, 12000, 16000, 24000, 48000] to [0, 1, 2, 3, 4]
        constexpr std::int32_t RateId(std::int32_t r) noexcept
        {
            const std::int32_t v = (((r >> 12) - (r > 16000 ? 1 : 0)) >> (r > 24000 ? 1 : 0)) - 1;
            return v < 5 ? v : 5;
        }

        // a second order AR filter with single delay elements
        void PrivateAr2(std::int32_t s[], std::int32_t outQ8[], const std::int16_t in[], const std::int16_t aQ14[], std::int32_t len) noexcept
        {
            for (std::int32_t k = 0; k < len; ++k) {
                std::int32_t out32 = AddLshift32(s[0], static_cast<std::int32_t>(in[k]), 8);
                outQ8[k]           = out32;
                out32              = Lshift(out32, 2);
                s[0]               = Smlawb(s[1], out32, aQ14[0]);
                s[1]               = Smulwb(out32, aQ14[1]);
            }
        }

        std::int16_t* DownFirInterpol(std::int16_t* out, const std::int32_t* buf, const std::int16_t* firCoefs, std::int32_t firOrder, std::int32_t firFracs,
                                      std::int32_t maxIndexQ16, std::int32_t indexIncrementQ16) noexcept
        {
            switch (firOrder) {
                case DownOrderFir0:
                    for (std::int32_t indexQ16 = 0; indexQ16 < maxIndexQ16; indexQ16 += indexIncrementQ16) {
                        // the integer part points to the buffered input, the fraction gives the coefficients
                        const std::int32_t* bufPtr      = buf + (indexQ16 >> 16);
                        const std::int32_t  interpolInd = Smulwb(indexQ16 & 0xFFFF, firFracs);
                        const std::int16_t* ip          = &firCoefs[DownOrderFir0 / 2 * interpolInd];
                        std::int32_t        resQ6       = Smulwb(bufPtr[0], ip[0]);
                        resQ6                           = Smlawb(resQ6, bufPtr[1], ip[1]);
                        resQ6                           = Smlawb(resQ6, bufPtr[2], ip[2]);
                        resQ6                           = Smlawb(resQ6, bufPtr[3], ip[3]);
                        resQ6                           = Smlawb(resQ6, bufPtr[4], ip[4]);
                        resQ6                           = Smlawb(resQ6, bufPtr[5], ip[5]);
                        resQ6                           = Smlawb(resQ6, bufPtr[6], ip[6]);
                        resQ6                           = Smlawb(resQ6, bufPtr[7], ip[7]);
                        resQ6                           = Smlawb(resQ6, bufPtr[8], ip[8]);
                        ip                              = &firCoefs[DownOrderFir0 / 2 * (firFracs - 1 - interpolInd)];
                        resQ6                           = Smlawb(resQ6, bufPtr[17], ip[0]);
                        resQ6                           = Smlawb(resQ6, bufPtr[16], ip[1]);
                        resQ6                           = Smlawb(resQ6, bufPtr[15], ip[2]);
                        resQ6                           = Smlawb(resQ6, bufPtr[14], ip[3]);
                        resQ6                           = Smlawb(resQ6, bufPtr[13], ip[4]);
                        resQ6                           = Smlawb(resQ6, bufPtr[12], ip[5]);
                        resQ6                           = Smlawb(resQ6, bufPtr[11], ip[6]);
                        resQ6                           = Smlawb(resQ6, bufPtr[10], ip[7]);
                        resQ6                           = Smlawb(resQ6, bufPtr[9], ip[8]);
                        // scaled down, saturated
                        *out++ = static_cast<std::int16_t>(Sat16(RshiftRound(resQ6, 6)));
                    }
                    break;
                case DownOrderFir1:
                    for (std::int32_t indexQ16 = 0; indexQ16 < maxIndexQ16; indexQ16 += indexIncrementQ16) {
                        const std::int32_t* bufPtr = buf + (indexQ16 >> 16);
                        std::int32_t        resQ6  = Smulwb(bufPtr[0] + bufPtr[23], firCoefs[0]);
                        for (std::int32_t j = 1; j < 12; ++j)
                            resQ6 = Smlawb(resQ6, bufPtr[j] + bufPtr[23 - j], firCoefs[j]);
                        *out++ = static_cast<std::int16_t>(Sat16(RshiftRound(resQ6, 6)));
                    }
                    break;
                case DownOrderFir2:
                    for (std::int32_t indexQ16 = 0; indexQ16 < maxIndexQ16; indexQ16 += indexIncrementQ16) {
                        const std::int32_t* bufPtr = buf + (indexQ16 >> 16);
                        std::int32_t        resQ6  = Smulwb(bufPtr[0] + bufPtr[35], firCoefs[0]);
                        for (std::int32_t j = 1; j < 18; ++j)
                            resQ6 = Smlawb(resQ6, bufPtr[j] + bufPtr[35 - j], firCoefs[j]);
                        *out++ = static_cast<std::int16_t>(Sat16(RshiftRound(resQ6, 6)));
                    }
                    break;
                default: break;
            }
            return out;
        }

        // a 2nd order AR filter followed by FIR interpolation
        void PrivateDownFir(SilkResamplerState* s, std::int16_t out[], const std::int16_t in[], std::int32_t inLen) noexcept
        {
            std::int32_t buf[MaxBatchSizeIn + DownOrderFir2];
            // the buffered samples to the start of the buffer
            std::memcpy(buf, s->sFIR.i32, static_cast<std::size_t>(s->FIR_Order) * sizeof(std::int32_t));
            const std::int16_t* firCoefs          = &s->Coefs[2];
            const std::int32_t  indexIncrementQ16 = s->invRatio_Q16;
            std::int32_t        nSamplesIn        = 0;
            while (true) {
                nSamplesIn = Min(inLen, s->batchSize);
                // the second-order AR filter (output in Q8)
                PrivateAr2(s->sIIR, &buf[s->FIR_Order], in, s->Coefs, nSamplesIn);
                const std::int32_t maxIndexQ16 = Lshift32(nSamplesIn, 16);
                // interpolation of the filtered signal
                out = DownFirInterpol(out, buf, firCoefs, s->FIR_Order, s->FIR_Fracs, maxIndexQ16, indexIncrementQ16);
                in += nSamplesIn;
                inLen -= nSamplesIn;
                if (inLen > 1)
                    std::memcpy(buf, &buf[nSamplesIn], static_cast<std::size_t>(s->FIR_Order) * sizeof(std::int32_t));
                else
                    break;
            }
            // the last part of the filtered signal into the state
            std::memcpy(s->sFIR.i32, &buf[nSamplesIn], static_cast<std::size_t>(s->FIR_Order) * sizeof(std::int32_t));
        }

        // 2x upsampling with 2nd order allpass filters and a notch filter just above Nyquist (Q10)
        void PrivateUp2Hq(std::int32_t* s, std::int16_t* out, const std::int16_t* in, std::int32_t len) noexcept
        {
            for (std::int32_t k = 0; k < len; ++k) {
                const std::int32_t in32 = Lshift(static_cast<std::int32_t>(in[k]), 10);
                // the three all-pass sections of the even output sample
                std::int32_t y    = in32 - s[0];
                std::int32_t x    = Smulwb(y, resampler_up2_hq_0[0]);
                std::int32_t out1 = s[0] + x;
                s[0]              = in32 + x;
                y                 = out1 - s[1];
                x                 = Smulwb(y, resampler_up2_hq_0[1]);
                std::int32_t out2 = s[1] + x;
                s[1]              = out1 + x;
                y                 = out2 - s[2];
                x                 = Smlawb(y, y, resampler_up2_hq_0[2]);
                out1              = s[2] + x;
                s[2]              = out2 + x;
                out[2 * k]        = static_cast<std::int16_t>(Sat16(RshiftRound(out1, 10)));
                // the three all-pass sections of the odd output sample
                y              = in32 - s[3];
                x              = Smulwb(y, resampler_up2_hq_1[0]);
                out1           = s[3] + x;
                s[3]           = in32 + x;
                y              = out1 - s[4];
                x              = Smulwb(y, resampler_up2_hq_1[1]);
                out2           = s[4] + x;
                s[4]           = out1 + x;
                y              = out2 - s[5];
                x              = Smlawb(y, y, resampler_up2_hq_1[2]);
                out1           = s[5] + x;
                s[5]           = out2 + x;
                out[2 * k + 1] = static_cast<std::int16_t>(Sat16(RshiftRound(out1, 10)));
            }
        }

        std::int16_t* IirFirInterpol(std::int16_t* out, const std::int16_t* buf, std::int32_t maxIndexQ16, std::int32_t indexIncrementQ16) noexcept
        {
            for (std::int32_t indexQ16 = 0; indexQ16 < maxIndexQ16; indexQ16 += indexIncrementQ16) {
                const std::int32_t  tableIndex = Smulwb(indexQ16 & 0xFFFF, 12);
                const std::int16_t* bufPtr     = &buf[indexQ16 >> 16];
                std::int32_t        resQ15     = Smulbb(bufPtr[0], resampler_frac_FIR_12[tableIndex][0]);
                resQ15                         = Smlabb(resQ15, bufPtr[1], resampler_frac_FIR_12[tableIndex][1]);
                resQ15                         = Smlabb(resQ15, bufPtr[2], resampler_frac_FIR_12[tableIndex][2]);
                resQ15                         = Smlabb(resQ15, bufPtr[3], resampler_frac_FIR_12[tableIndex][3]);
                resQ15                         = Smlabb(resQ15, bufPtr[4], resampler_frac_FIR_12[11 - tableIndex][3]);
                resQ15                         = Smlabb(resQ15, bufPtr[5], resampler_frac_FIR_12[11 - tableIndex][2]);
                resQ15                         = Smlabb(resQ15, bufPtr[6], resampler_frac_FIR_12[11 - tableIndex][1]);
                resQ15                         = Smlabb(resQ15, bufPtr[7], resampler_frac_FIR_12[11 - tableIndex][0]);
                *out++                         = static_cast<std::int16_t>(Sat16(RshiftRound(resQ15, 15)));
            }
            return out;
        }

        // allpass-based 2x upsampling followed by FIR interpolation
        void PrivateIirFir(SilkResamplerState* s, std::int16_t out[], const std::int16_t in[], std::int32_t inLen) noexcept
        {
            std::int16_t buf[2 * MaxBatchSizeIn + OrderFir12];
            std::memcpy(buf, s->sFIR.i16, OrderFir12 * sizeof(std::int16_t));
            const std::int32_t indexIncrementQ16 = s->invRatio_Q16;
            std::int32_t       nSamplesIn        = 0;
            while (true) {
                nSamplesIn = Min(inLen, s->batchSize);
                // 2x upsampling
                PrivateUp2Hq(s->sIIR, &buf[OrderFir12], in, nSamplesIn);
                const std::int32_t maxIndexQ16 = Lshift32(nSamplesIn, 16 + 1); // + 1: 2x upsampling
                out                            = IirFirInterpol(out, buf, maxIndexQ16, indexIncrementQ16);
                in += nSamplesIn;
                inLen -= nSamplesIn;
                if (inLen > 0)
                    std::memcpy(buf, &buf[nSamplesIn << 1], OrderFir12 * sizeof(std::int16_t));
                else
                    break;
            }
            std::memcpy(s->sFIR.i16, &buf[nSamplesIn << 1], OrderFir12 * sizeof(std::int16_t));
        }

    } // namespace

    std::int32_t ResamplerInit(SilkResamplerState* s, std::int32_t fsHzIn, std::int32_t fsHzOut, bool forEnc) noexcept
    {
        std::memset(s, 0, sizeof(SilkResamplerState));
        if (forEnc) {
            if ((fsHzIn != 8000 && fsHzIn != 12000 && fsHzIn != 16000 && fsHzIn != 24000 && fsHzIn != 48000)
                || (fsHzOut != 8000 && fsHzOut != 12000 && fsHzOut != 16000))
                return -1;
            s->inputDelay = DelayMatrixEnc[RateId(fsHzIn)][RateId(fsHzOut)];
        } else {
            if ((fsHzIn != 8000 && fsHzIn != 12000 && fsHzIn != 16000)
                || (fsHzOut != 8000 && fsHzOut != 12000 && fsHzOut != 16000 && fsHzOut != 24000 && fsHzOut != 48000))
                return -1;
            s->inputDelay = DelayMatrixDec[RateId(fsHzIn)][RateId(fsHzOut)];
        }
        s->Fs_in_kHz  = Div32_16(fsHzIn, 1000);
        s->Fs_out_kHz = Div32_16(fsHzOut, 1000);
        // samples per batch
        s->batchSize = s->Fs_in_kHz * MaxBatchSizeMs;

        // the resampler with the right ratio
        std::int32_t up2x = 0;
        if (fsHzOut > fsHzIn) {
            if (fsHzOut == fsHzIn * 2) {
                s->resampler_function = UseUp2HqWrapper; // directly the 2x upsampler
            } else {
                s->resampler_function = UseIirFir;
                up2x                  = 1;
            }
        } else if (fsHzOut < fsHzIn) {
            s->resampler_function = UseDownFir;
            if (fsHzOut * 4 == fsHzIn * 3) {
                s->FIR_Fracs = 3;
                s->FIR_Order = DownOrderFir0;
                s->Coefs     = Resampler_3_4_COEFS;
            } else if (fsHzOut * 3 == fsHzIn * 2) {
                s->FIR_Fracs = 2;
                s->FIR_Order = DownOrderFir0;
                s->Coefs     = Resampler_2_3_COEFS;
            } else if (fsHzOut * 2 == fsHzIn) {
                s->FIR_Fracs = 1;
                s->FIR_Order = DownOrderFir1;
                s->Coefs     = Resampler_1_2_COEFS;
            } else if (fsHzOut * 3 == fsHzIn) {
                s->FIR_Fracs = 1;
                s->FIR_Order = DownOrderFir2;
                s->Coefs     = Resampler_1_3_COEFS;
            } else if (fsHzOut * 4 == fsHzIn) {
                s->FIR_Fracs = 1;
                s->FIR_Order = DownOrderFir2;
                s->Coefs     = Resampler_1_4_COEFS;
            } else if (fsHzOut * 6 == fsHzIn) {
                s->FIR_Fracs = 1;
                s->FIR_Order = DownOrderFir2;
                s->Coefs     = Resampler_1_6_COEFS;
            } else {
                return -1; // none available
            }
        } else {
            s->resampler_function = UseCopy; // equal rates
        }

        // the ratio of input/output samples, rounded up
        s->invRatio_Q16 = Lshift32(Div32(Lshift32(fsHzIn, 14 + up2x), fsHzOut), 2);
        while (Smulww(s->invRatio_Q16, fsHzOut) < Lshift32(fsHzIn, up2x))
            ++s->invRatio_Q16;
        return 0;
    }

    std::int32_t Resample(SilkResamplerState* s, std::int16_t* out, const std::int16_t* in, std::int32_t inLen) noexcept
    {
        const std::int32_t nSamples = s->Fs_in_kHz - s->inputDelay;
        // to the delay buffer
        std::memcpy(&s->delayBuf[s->inputDelay], in, static_cast<std::size_t>(nSamples) * sizeof(std::int16_t));
        switch (s->resampler_function) {
            case UseUp2HqWrapper:
                PrivateUp2Hq(s->sIIR, out, s->delayBuf, s->Fs_in_kHz);
                PrivateUp2Hq(s->sIIR, &out[s->Fs_out_kHz], &in[nSamples], inLen - s->Fs_in_kHz);
                break;
            case UseIirFir:
                PrivateIirFir(s, out, s->delayBuf, s->Fs_in_kHz);
                PrivateIirFir(s, &out[s->Fs_out_kHz], &in[nSamples], inLen - s->Fs_in_kHz);
                break;
            case UseDownFir:
                PrivateDownFir(s, out, s->delayBuf, s->Fs_in_kHz);
                PrivateDownFir(s, &out[s->Fs_out_kHz], &in[nSamples], inLen - s->Fs_in_kHz);
                break;
            default:
                std::memcpy(out, s->delayBuf, static_cast<std::size_t>(s->Fs_in_kHz) * sizeof(std::int16_t));
                std::memcpy(&out[s->Fs_out_kHz], &in[nSamples], static_cast<std::size_t>(inLen - s->Fs_in_kHz) * sizeof(std::int16_t));
        }
        // to the delay buffer
        std::memcpy(s->delayBuf, &in[inLen - s->inputDelay], static_cast<std::size_t>(s->inputDelay) * sizeof(std::int16_t));
        return 0;
    }

    void ResamplerDown2(std::int32_t* s, std::int16_t* out, const std::int16_t* in, std::int32_t inLen) noexcept
    {
        const std::int32_t len2 = inLen >> 1;
        // the variables and the state are in Q10
        for (std::int32_t k = 0; k < len2; ++k) {
            // the all-pass section for the even input sample
            std::int32_t in32  = Lshift(static_cast<std::int32_t>(in[2 * k]), 10);
            std::int32_t y     = in32 - s[0];
            std::int32_t x     = Smlawb(y, y, resampler_down2_1);
            std::int32_t out32 = s[0] + x;
            s[0]               = in32 + x;
            // the all-pass section for the odd input sample, added to the output of the previous one
            in32  = Lshift(static_cast<std::int32_t>(in[2 * k + 1]), 10);
            y     = in32 - s[1];
            x     = Smulwb(y, resampler_down2_0);
            out32 = out32 + s[1];
            out32 = out32 + x;
            s[1]  = in32 + x;
            // back to int16
            out[k] = static_cast<std::int16_t>(Sat16(RshiftRound(out32, 11)));
        }
    }

    void ResamplerDown2_3(std::int32_t* s, std::int16_t* out, const std::int16_t* in, std::int32_t inLen) noexcept
    {
        constexpr std::int32_t orderFir = 4;
        std::int32_t           buf[MaxBatchSizeIn + orderFir];
        // the buffered samples to the start of the buffer
        std::memcpy(buf, s, orderFir * sizeof(std::int32_t));
        std::int32_t nSamplesIn = 0;
        // over blocks of input samples
        for (;;) {
            nSamplesIn = Min(inLen, MaxBatchSizeIn);
            // a second-order AR filter (output in Q8)
            PrivateAr2(&s[orderFir], &buf[orderFir], in, Resampler_2_3_COEFS_LQ, nSamplesIn);
            // the filtered signal interpolated
            const std::int32_t* bufPtr  = buf;
            std::int32_t        counter = nSamplesIn;
            while (counter > 2) {
                // inner products, scaled down, saturated
                std::int32_t resQ6 = Smulwb(bufPtr[0], Resampler_2_3_COEFS_LQ[2]);
                resQ6              = Smlawb(resQ6, bufPtr[1], Resampler_2_3_COEFS_LQ[3]);
                resQ6              = Smlawb(resQ6, bufPtr[2], Resampler_2_3_COEFS_LQ[5]);
                resQ6              = Smlawb(resQ6, bufPtr[3], Resampler_2_3_COEFS_LQ[4]);
                *out++             = static_cast<std::int16_t>(Sat16(RshiftRound(resQ6, 6)));
                resQ6              = Smulwb(bufPtr[1], Resampler_2_3_COEFS_LQ[4]);
                resQ6              = Smlawb(resQ6, bufPtr[2], Resampler_2_3_COEFS_LQ[5]);
                resQ6              = Smlawb(resQ6, bufPtr[3], Resampler_2_3_COEFS_LQ[3]);
                resQ6              = Smlawb(resQ6, bufPtr[4], Resampler_2_3_COEFS_LQ[2]);
                *out++             = static_cast<std::int16_t>(Sat16(RshiftRound(resQ6, 6)));
                bufPtr += 3;
                counter -= 3;
            }
            in += nSamplesIn;
            inLen -= nSamplesIn;
            if (inLen <= 0)
                break;
            // more to do: the last part of the filtered signal to the start of the buffer
            std::memcpy(buf, &buf[nSamplesIn], orderFir * sizeof(std::int32_t));
        }
        // the last part of the filtered signal to the state of the next call
        std::memcpy(s, &buf[nSamplesIn], orderFir * sizeof(std::int32_t));
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
