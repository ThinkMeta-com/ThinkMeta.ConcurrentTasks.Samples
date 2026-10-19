// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkPitchAnalysis.h"

#include <cstring>

#include "CeltPitch.h"
#include "SilkDefines.h"
#include "SilkMacros.h"
#include "SilkResampler.h"
#include "SilkSigProcFlp.h"
#include "SilkTables.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    namespace {

        // pitch_est_defines.h
        constexpr std::int32_t PeMaxNbSubfr         = 4;
        constexpr std::int32_t PeSubfrLengthMs      = 5;
        constexpr std::int32_t PeLtpMemLengthMs     = 4 * PeSubfrLengthMs;
        constexpr std::int32_t PeMaxFrameLengthMs   = PeLtpMemLengthMs + PeMaxNbSubfr * PeSubfrLengthMs;
        constexpr std::int32_t PeMaxLagMs           = 18; // 56 Hz
        constexpr std::int32_t PeMinLagMs           = 2;  // 500 Hz
        constexpr std::int32_t PeMaxLag             = PeMaxLagMs * SilkMaxFsKhz;
        constexpr std::int32_t PeDSrchLength        = 24;
        constexpr std::int32_t PeNbStage3Lags       = 5;
        constexpr std::int32_t PeNbCbksStage2       = 3;
        constexpr std::int32_t PeNbCbksStage2Ext    = 11;
        constexpr std::int32_t PeNbCbksStage3Max    = 34;
        constexpr std::int32_t PeNbCbksStage3_10Ms  = 12;
        constexpr std::int32_t PeNbCbksStage2_10Ms  = 3;
        constexpr float        PeShortlagBias       = 0.2f;
        constexpr float        PePrevlagBias        = 0.2f;
        constexpr float        PeFlatcontourBias    = 0.05f;
        constexpr std::int32_t ScratchSize          = 22;
        constexpr std::int32_t MaxFindPitchLpcOrder = 16;
        constexpr std::int32_t FindPitchLpcWinMax   = FindPitchLpcWinMs * SilkMaxFsKhz;

        using Stage3Array = float[PeMaxNbSubfr][PeNbCbksStage3Max][PeNbStage3Lags];

        // matrix_ptr
        constexpr std::int32_t MatrixPtr(const std::int8_t* ptr, std::int32_t row, std::int32_t col, std::int32_t n) noexcept
        {
            return ptr[row * n + col];
        }

        // the stage 3 range and codebook tables of the frame length and complexity
        void Stage3Tables(std::int32_t nbSubfr, std::int32_t complexity, const std::int8_t** lagRange, const std::int8_t** lagCb, std::int32_t* nbCbkSearch,
                          std::int32_t* cbkSize) noexcept
        {
            if (nbSubfr == PeMaxNbSubfr) {
                *lagRange    = &Lag_range_stage3[complexity][0][0];
                *lagCb       = &CB_lags_stage3[0][0];
                *nbCbkSearch = nb_cbk_searchs_stage3[complexity];
                *cbkSize     = PeNbCbksStage3Max;
            } else {
                *lagRange    = &Lag_range_stage3_10_ms[0][0];
                *lagCb       = &CB_lags_stage3_10_ms[0][0];
                *nbCbkSearch = PeNbCbksStage3_10Ms;
                *cbkSize     = PeNbCbksStage3_10Ms;
            }
        }

        // silk_P_Ana_calc_corr_st3: the correlations of the stage 3 search; for the whole lag
        // codebook of all searched offset lags (lag +- 2) each sub-frame needs (sf1: [-8, 7],
        // sf2: [-4, 4], sf3: [-3, 4], sf4: [-6, 8]) 48 correlations in total
        void CalcCorrSt3(Stage3Array& crossCorrSt3, const float frame[], std::int32_t startLag, std::int32_t sfLength, std::int32_t nbSubfr,
                         std::int32_t complexity, std::int32_t arch) noexcept
        {
            const std::int8_t* lagRange    = nullptr;
            const std::int8_t* lagCb       = nullptr;
            std::int32_t       nbCbkSearch = 0;
            std::int32_t       cbkSize     = 0;
            Stage3Tables(nbSubfr, complexity, &lagRange, &lagCb, &nbCbkSearch, &cbkSize);

            float        scratchMem[ScratchSize];
            float        xcorr[ScratchSize];
            const float* targetPtr = &frame[Lshift(sfLength, 2)]; // the middle of the frame
            for (std::int32_t k = 0; k < nbSubfr; ++k) {
                // the correlations of the sub-frame
                const std::int32_t lagLow  = MatrixPtr(lagRange, k, 0, 2);
                const std::int32_t lagHigh = MatrixPtr(lagRange, k, 1, 2);
                PitchXcorr(targetPtr, targetPtr - startLag - lagHigh, xcorr, sfLength, lagHigh - lagLow + 1, arch);
                std::int32_t lagCounter = 0;
                for (std::int32_t j = lagLow; j <= lagHigh; ++j)
                    scratchMem[lagCounter++] = xcorr[lagHigh - j];

                // the correlations of each codebook vector for each start lag
                const std::int32_t delta = MatrixPtr(lagRange, k, 0, 2);
                for (std::int32_t i = 0; i < nbCbkSearch; ++i) {
                    const std::int32_t idx = MatrixPtr(lagCb, k, i, cbkSize) - delta;
                    for (std::int32_t j = 0; j < PeNbStage3Lags; ++j)
                        crossCorrSt3[k][i][j] = scratchMem[idx + j];
                }
                targetPtr += sfLength;
            }
        }

        // silk_P_Ana_calc_energy_st3: the energies of the stage 3 search, recursively
        void CalcEnergySt3(Stage3Array& energiesSt3, const float frame[], std::int32_t startLag, std::int32_t sfLength, std::int32_t nbSubfr,
                           std::int32_t complexity) noexcept
        {
            const std::int8_t* lagRange    = nullptr;
            const std::int8_t* lagCb       = nullptr;
            std::int32_t       nbCbkSearch = 0;
            std::int32_t       cbkSize     = 0;
            Stage3Tables(nbSubfr, complexity, &lagRange, &lagCb, &nbCbkSearch, &cbkSize);

            float        scratchMem[ScratchSize];
            const float* targetPtr = &frame[Lshift(sfLength, 2)];
            for (std::int32_t k = 0; k < nbSubfr; ++k) {
                // the energy of the first lag
                const float* basisPtr      = targetPtr - (startLag + MatrixPtr(lagRange, k, 0, 2));
                double       energy        = EnergyFlp(basisPtr, sfLength) + 1e-3;
                std::int32_t lagCounter    = 0;
                scratchMem[lagCounter++]   = static_cast<float>(energy);
                const std::int32_t lagDiff = MatrixPtr(lagRange, k, 1, 2) - MatrixPtr(lagRange, k, 0, 2) + 1;
                for (std::int32_t i = 1; i < lagDiff; ++i) {
                    // the part leaving the window removed, the part entering added
                    energy -= basisPtr[sfLength - i] * static_cast<double>(basisPtr[sfLength - i]);
                    energy += basisPtr[-i] * static_cast<double>(basisPtr[-i]);
                    scratchMem[lagCounter++] = static_cast<float>(energy);
                }

                // the energies of each codebook vector for each start lag
                const std::int32_t delta = MatrixPtr(lagRange, k, 0, 2);
                for (std::int32_t i = 0; i < nbCbkSearch; ++i) {
                    const std::int32_t idx = MatrixPtr(lagCb, k, i, cbkSize) - delta;
                    for (std::int32_t j = 0; j < PeNbStage3Lags; ++j)
                        energiesSt3[k][i][j] = scratchMem[idx + j];
                }
                targetPtr += sfLength;
            }
        }

        // silk_pitch_analysis_core_FLP: 0 voiced, 1 unvoiced
        std::int32_t PitchAnalysisCore(const float* frame, std::int32_t* pitchOut, std::int16_t* lagIndex, std::int8_t* contourIndex, float* ltpCorr,
                                       std::int32_t prevLag, float searchThres1, float searchThres2, std::int32_t fsKhz, std::int32_t complexity,
                                       std::int32_t nbSubfr, std::int32_t arch) noexcept
        {
            // the frame lengths and the lag range at the sampling rate
            const std::int32_t frameLength     = (PeLtpMemLengthMs + nbSubfr * PeSubfrLengthMs) * fsKhz;
            const std::int32_t frameLength4kHz = (PeLtpMemLengthMs + nbSubfr * PeSubfrLengthMs) * 4;
            const std::int32_t frameLength8kHz = (PeLtpMemLengthMs + nbSubfr * PeSubfrLengthMs) * 8;
            const std::int32_t sfLength        = PeSubfrLengthMs * fsKhz;
            const std::int32_t sfLength4kHz    = PeSubfrLengthMs * 4;
            const std::int32_t sfLength8kHz    = PeSubfrLengthMs * 8;
            const std::int32_t minLag          = PeMinLagMs * fsKhz;
            const std::int32_t minLag4kHz      = PeMinLagMs * 4;
            const std::int32_t minLag8kHz      = PeMinLagMs * 8;
            const std::int32_t maxLag          = PeMaxLagMs * fsKhz - 1;
            const std::int32_t maxLag4kHz      = PeMaxLagMs * 4;
            const std::int32_t maxLag8kHz      = PeMaxLagMs * 8 - 1;

            // resampled to 8 kHz
            float        frame8kHz[PeMaxFrameLengthMs * 8];
            float        frame4kHz[PeMaxFrameLengthMs * 4];
            std::int16_t frame8Fix[PeMaxFrameLengthMs * 8];
            std::int16_t frame4Fix[PeMaxFrameLengthMs * 4];
            std::int32_t filtState[6];
            if (fsKhz == 16) {
                std::int16_t frame16Fix[16 * PeMaxFrameLengthMs];
                Float2ShortArray(frame16Fix, frame, frameLength);
                std::memset(filtState, 0, 2 * sizeof(std::int32_t));
                ResamplerDown2(filtState, frame8Fix, frame16Fix, frameLength);
                Short2FloatArray(frame8kHz, frame8Fix, frameLength8kHz);
            } else if (fsKhz == 12) {
                std::int16_t frame12Fix[12 * PeMaxFrameLengthMs];
                Float2ShortArray(frame12Fix, frame, frameLength);
                std::memset(filtState, 0, 6 * sizeof(std::int32_t));
                ResamplerDown2_3(filtState, frame8Fix, frame12Fix, frameLength);
                Short2FloatArray(frame8kHz, frame8Fix, frameLength8kHz);
            } else {
                Float2ShortArray(frame8Fix, frame, frameLength8kHz);
            }

            // decimated again to 4 kHz
            std::memset(filtState, 0, 2 * sizeof(std::int32_t));
            ResamplerDown2(filtState, frame4Fix, frame8Fix, frameLength8kHz);
            Short2FloatArray(frame4kHz, frame4Fix, frameLength4kHz);

            // a low-pass filter
            for (std::int32_t i = frameLength4kHz - 1; i > 0; --i)
                frame4kHz[i] = static_cast<std::int16_t>(Sat16(frame4kHz[i] + frame4kHz[i - 1]));

            // the first stage, at 4 kHz
            float C[PeMaxNbSubfr][(PeMaxLag >> 1) + 5];
            float xcorr[PeMaxLagMs * 4 - PeMinLagMs * 4 + 1];
            std::memset(C, 0, sizeof(float) * nbSubfr * ((PeMaxLag >> 1) + 5));
            const float* targetPtr = &frame4kHz[Lshift(sfLength4kHz, 2)];
            for (std::int32_t k = 0; k < nbSubfr >> 1; ++k) {
                const float* basisPtr = targetPtr - minLag4kHz;
                PitchXcorr(targetPtr, targetPtr - maxLag4kHz, xcorr, sfLength8kHz, maxLag4kHz - minLag4kHz + 1, arch);
                // the first vector products before the loop
                double crossCorr  = xcorr[maxLag4kHz - minLag4kHz];
                double normalizer = EnergyFlp(targetPtr, sfLength8kHz) + EnergyFlp(basisPtr, sfLength8kHz) + sfLength8kHz * 4000.0f;
                C[0][minLag4kHz] += static_cast<float>(2 * crossCorr / normalizer);
                // the normalizer recursively from now on
                for (std::int32_t d = minLag4kHz + 1; d <= maxLag4kHz; ++d) {
                    --basisPtr;
                    crossCorr = xcorr[maxLag4kHz - d];
                    // the new sample added, the oldest removed
                    normalizer += basisPtr[0] * static_cast<double>(basisPtr[0]) - basisPtr[sfLength8kHz] * static_cast<double>(basisPtr[sfLength8kHz]);
                    C[0][d] += static_cast<float>(2 * crossCorr / normalizer);
                }
                targetPtr += sfLength8kHz;
            }

            // a short-lag bias
            for (std::int32_t i = maxLag4kHz; i >= minLag4kHz; --i)
                C[0][i] -= C[0][i] * i / 4096.0f;

            // sorted
            std::int32_t lengthDSrch = 4 + 2 * complexity;
            std::int32_t dSrch[PeDSrchLength];
            InsertionSortDecreasingFlp(&C[0][minLag4kHz], dSrch, maxLag4kHz - minLag4kHz + 1, lengthDSrch);

            // a very low correlation already here: unvoiced
            const float cmax = C[0][minLag4kHz];
            if (cmax < 0.2f) {
                std::memset(pitchOut, 0, nbSubfr * sizeof(std::int32_t));
                *ltpCorr      = 0.0f;
                *lagIndex     = 0;
                *contourIndex = 0;
                return 1;
            }

            // to 8 kHz indices of the sorted correlations above the threshold
            const float threshold = searchThres1 * cmax;
            for (std::int32_t i = 0; i < lengthDSrch; ++i) {
                if (C[0][minLag4kHz + i] > threshold) {
                    dSrch[i] = Lshift(dSrch[i] + minLag4kHz, 1);
                } else {
                    lengthDSrch = i;
                    break;
                }
            }

            std::int16_t dComp[(PeMaxLag >> 1) + 5];
            for (std::int32_t i = minLag8kHz - 5; i < maxLag8kHz + 5; ++i)
                dComp[i] = 0;
            for (std::int32_t i = 0; i < lengthDSrch; ++i)
                dComp[dSrch[i]] = 1;

            // convolution
            for (std::int32_t i = maxLag8kHz + 3; i >= minLag8kHz; --i)
                dComp[i] = static_cast<std::int16_t>(dComp[i] + dComp[i - 1] + dComp[i - 2]);

            lengthDSrch = 0;
            for (std::int32_t i = minLag8kHz; i < maxLag8kHz + 1; ++i) {
                if (dComp[i + 1] > 0)
                    dSrch[lengthDSrch++] = i;
            }

            // convolution
            for (std::int32_t i = maxLag8kHz + 3; i >= minLag8kHz; --i)
                dComp[i] = static_cast<std::int16_t>(dComp[i] + dComp[i - 1] + dComp[i - 2] + dComp[i - 3]);

            std::int32_t lengthDComp = 0;
            for (std::int32_t i = minLag8kHz; i < maxLag8kHz + 4; ++i) {
                if (dComp[i] > 0)
                    dComp[lengthDComp++] = static_cast<std::int16_t>(i - 2);
            }

            // the second stage, at 8 kHz, on lag sections with high correlation: the energy of
            // each sub-frame projected onto its history, for a range of delays
            std::memset(C, 0, PeMaxNbSubfr * ((PeMaxLag >> 1) + 5) * sizeof(float));
            targetPtr = fsKhz == 8 ? &frame[PeLtpMemLengthMs * 8] : &frame8kHz[PeLtpMemLengthMs * 8];
            for (std::int32_t k = 0; k < nbSubfr; ++k) {
                const double energyTmp = EnergyFlp(targetPtr, sfLength8kHz) + 1.0;
                for (std::int32_t j = 0; j < lengthDComp; ++j) {
                    const std::int32_t d         = dComp[j];
                    const float*       basisPtr  = targetPtr - d;
                    const double       crossCorr = InnerProductFlp(basisPtr, targetPtr, sfLength8kHz, arch);
                    if (crossCorr > 0.0f) {
                        const double energy = EnergyFlp(basisPtr, sfLength8kHz);
                        C[k][d]             = static_cast<float>(2 * crossCorr / (energy + energyTmp));
                    } else {
                        C[k][d] = 0.0f;
                    }
                }
                targetPtr += sfLength8kHz;
            }

            // the search over the lag range and the lag codebook
            float        ccmax  = 0.0f; // does not matter
            float        ccmaxB = -1000.0f;
            std::int32_t cbimax = 0;  // no undefined lag values returned
            std::int32_t lag    = -1; // no lag with a strong enough correlation found yet

            float prevLagLog2 = 0;
            if (prevLag > 0) {
                if (fsKhz == 12)
                    prevLag = Lshift(prevLag, 1) / 3;
                else if (fsKhz == 16)
                    prevLag = prevLag >> 1;
                prevLagLog2 = Log2(static_cast<float>(prevLag));
            }

            // the stage 2 codebook of the number of sub-frames
            std::int32_t       cbkSize     = 0;
            const std::int8_t* lagCbPtr    = nullptr;
            std::int32_t       nbCbkSearch = 0;
            if (nbSubfr == PeMaxNbSubfr) {
                cbkSize  = PeNbCbksStage2Ext;
                lagCbPtr = &CB_lags_stage2[0][0];
                // at 8 kHz a larger codebook, as the last stage
                nbCbkSearch = fsKhz == 8 && complexity > PeMinComplex ? PeNbCbksStage2Ext : PeNbCbksStage2;
            } else {
                cbkSize     = PeNbCbksStage2_10Ms;
                lagCbPtr    = &CB_lags_stage2_10_ms[0][0];
                nbCbkSearch = PeNbCbksStage2_10Ms;
            }

            float cc[PeNbCbksStage2Ext];
            for (std::int32_t k = 0; k < lengthDSrch; ++k) {
                const std::int32_t d = dSrch[k];
                // all codebooks
                for (std::int32_t j = 0; j < nbCbkSearch; ++j) {
                    cc[j] = 0.0f;
                    for (std::int32_t i = 0; i < nbSubfr; ++i)
                        cc[j] += C[i][d + MatrixPtr(lagCbPtr, i, j, cbkSize)];
                }
                // the best codebook
                float        ccmaxNew  = -1000.0f;
                std::int32_t cbimaxNew = 0;
                for (std::int32_t i = 0; i < nbCbkSearch; ++i) {
                    if (cc[i] > ccmaxNew) {
                        ccmaxNew  = cc[i];
                        cbimaxNew = i;
                    }
                }

                // a bias towards shorter lags and towards the previous lag
                const float lagLog2   = Log2(static_cast<float>(d));
                float       ccmaxNewB = ccmaxNew - PeShortlagBias * nbSubfr * lagLog2;
                if (prevLag > 0) {
                    float deltaLagLog2Sqr = lagLog2 - prevLagLog2;
                    deltaLagLog2Sqr *= deltaLagLog2Sqr;
                    ccmaxNewB -= PePrevlagBias * nbSubfr * (*ltpCorr) * deltaLagLog2Sqr / (deltaLagLog2Sqr + 0.5f);
                }

                // the maximum biased correlation, high enough to be voiced
                if (ccmaxNewB > ccmaxB && ccmaxNew > nbSubfr * searchThres2) {
                    ccmaxB = ccmaxNewB;
                    ccmax  = ccmaxNew;
                    lag    = d;
                    cbimax = cbimaxNew;
                }
            }

            if (lag == -1) {
                // no suitable candidate
                std::memset(pitchOut, 0, PeMaxNbSubfr * sizeof(std::int32_t));
                *ltpCorr      = 0.0f;
                *lagIndex     = 0;
                *contourIndex = 0;
                return 1;
            }

            // the normalised correlation
            *ltpCorr = static_cast<float>(ccmax / nbSubfr);

            if (fsKhz > 8) {
                // the search in the original signal, compensated for the decimation
                if (fsKhz == 12)
                    lag = RshiftRound(Smulbb(lag, 3), 1);
                else
                    lag = Lshift(lag, 1);
                lag                         = Limit(lag, minLag, maxLag);
                const std::int32_t startLag = MaxInt(lag - 2, minLag);
                const std::int32_t endLag   = MinInt(lag + 2, maxLag);
                std::int32_t       lagNew   = lag; // no undefined lag
                cbimax                      = 0;
                ccmax                       = -1000.0f;

                // the correlations and energies of stage 3
                Stage3Array crossCorrSt3;
                Stage3Array energiesSt3;
                CalcCorrSt3(crossCorrSt3, frame, startLag, sfLength, nbSubfr, complexity, arch);
                CalcEnergySt3(energiesSt3, frame, startLag, sfLength, nbSubfr, complexity);

                std::int32_t lagCounter  = 0;
                const float  contourBias = PeFlatcontourBias / lag;

                // the codebook parameters of the complexity and the frame length
                if (nbSubfr == PeMaxNbSubfr) {
                    nbCbkSearch = nb_cbk_searchs_stage3[complexity];
                    cbkSize     = PeNbCbksStage3Max;
                    lagCbPtr    = &CB_lags_stage3[0][0];
                } else {
                    nbCbkSearch = PeNbCbksStage3_10Ms;
                    cbkSize     = PeNbCbksStage3_10Ms;
                    lagCbPtr    = &CB_lags_stage3_10_ms[0][0];
                }

                targetPtr              = &frame[PeLtpMemLengthMs * fsKhz];
                const double energyTmp = EnergyFlp(targetPtr, nbSubfr * sfLength) + 1.0;
                for (std::int32_t d = startLag; d <= endLag; ++d) {
                    for (std::int32_t j = 0; j < nbCbkSearch; ++j) {
                        double crossCorr = 0.0;
                        double energy    = energyTmp;
                        for (std::int32_t k = 0; k < nbSubfr; ++k) {
                            crossCorr += crossCorrSt3[k][j][lagCounter];
                            energy += energiesSt3[k][j][lagCounter];
                        }
                        float ccmaxNew = 0.0f;
                        if (crossCorr > 0.0) {
                            ccmaxNew = static_cast<float>(2 * crossCorr / energy);
                            // reduced by the flatness of the contour
                            ccmaxNew *= 1.0f - contourBias * j;
                        }
                        if (ccmaxNew > ccmax && (d + static_cast<std::int32_t>(CB_lags_stage3[0][j])) <= maxLag) {
                            ccmax  = ccmaxNew;
                            lagNew = d;
                            cbimax = j;
                        }
                    }
                    ++lagCounter;
                }

                for (std::int32_t k = 0; k < nbSubfr; ++k) {
                    pitchOut[k] = lagNew + MatrixPtr(lagCbPtr, k, cbimax, cbkSize);
                    pitchOut[k] = Limit(pitchOut[k], minLag, PeMaxLagMs * fsKhz);
                }
                *lagIndex     = static_cast<std::int16_t>(lagNew - minLag);
                *contourIndex = static_cast<std::int8_t>(cbimax);
            } else {
                // 8 kHz: the lags
                for (std::int32_t k = 0; k < nbSubfr; ++k) {
                    pitchOut[k] = lag + MatrixPtr(lagCbPtr, k, cbimax, cbkSize);
                    pitchOut[k] = Limit(pitchOut[k], minLag8kHz, PeMaxLagMs * 8);
                }
                *lagIndex     = static_cast<std::int16_t>(lag - minLag8kHz);
                *contourIndex = static_cast<std::int8_t>(cbimax);
            }
            // voiced
            return 0;
        }

    } // namespace

    void FindPitchLags(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, float res[], const float x[], std::int32_t arch) noexcept
    {
        SilkEncoderState* c = &enc->sCmn;

        // the buffer lengths of the sampling rate
        const std::int32_t bufLen = c->la_pitch + c->frame_length + c->ltp_mem_length;
        const float*       xBuf   = x - c->ltp_mem_length;

        // the LPC AR coefficients: the windowed signal (the first la_pitch samples, the middle
        // non-windowed ones, the last la_pitch)
        float        wsig[FindPitchLpcWinMax];
        const float* xBufPtr = xBuf + bufLen - c->pitch_LPC_win_length;
        float*       wsigPtr = wsig;
        ApplySineWindowFlp(wsigPtr, xBufPtr, 1, c->la_pitch);
        wsigPtr += c->la_pitch;
        xBufPtr += c->la_pitch;
        std::memcpy(wsigPtr, xBufPtr, (c->pitch_LPC_win_length - (c->la_pitch << 1)) * sizeof(float));
        wsigPtr += c->pitch_LPC_win_length - (c->la_pitch << 1);
        xBufPtr += c->pitch_LPC_win_length - (c->la_pitch << 1);
        ApplySineWindowFlp(wsigPtr, xBufPtr, 2, c->la_pitch);

        // the autocorrelation, plus white noise as a fraction of the energy
        float autoCorr[MaxFindPitchLpcOrder + 1];
        AutocorrelationFlp(autoCorr, wsig, c->pitch_LPC_win_length, c->pitchEstimationLPCOrder + 1, arch);
        autoCorr[0] += autoCorr[0] * FindPitchWhiteNoiseFraction + 1;

        // the reflection coefficients (Schur), the prediction gain
        float       reflCoef[MaxFindPitchLpcOrder];
        const float resNrg = SchurFlp(reflCoef, autoCorr, c->pitchEstimationLPCOrder);
        ctrl->predGain     = autoCorr[0] / MaxFloat(resNrg, 1.0f);

        // the prediction coefficients, bandwidth expanded
        float a[MaxFindPitchLpcOrder];
        K2aFlp(a, reflCoef, c->pitchEstimationLPCOrder);
        BwexpanderFlp(a, c->pitchEstimationLPCOrder, FindPitchBandwidthExpansion);

        // the LPC analysis filtering
        LpcAnalysisFilterFlp(res, a, xBuf, bufLen, c->pitchEstimationLPCOrder);

        if (c->indices.signalType != TypeNoVoiceActivity && c->first_frame_after_reset == 0) {
            // the threshold of the pitch estimator
            float thrhld = 0.6f;
            thrhld -= 0.004f * c->pitchEstimationLPCOrder;
            thrhld -= 0.1f * c->speech_activity_Q8 * (1.0f / 256.0f);
            thrhld -= 0.15f * (c->prevSignalType >> 1);
            thrhld -= 0.1f * c->input_tilt_Q15 * (1.0f / 32768.0f);

            // the pitch estimator
            if (PitchAnalysisCore(res, ctrl->pitchL, &c->indices.lagIndex, &c->indices.contourIndex, &enc->LTPCorr, c->prevLag,
                                  c->pitchEstimationThreshold_Q16 / 65536.0f, thrhld, c->fs_kHz, c->pitchEstimationComplexity, c->nb_subfr, arch)
                == 0)
                c->indices.signalType = TypeVoiced;
            else
                c->indices.signalType = TypeUnvoiced;
        } else {
            std::memset(ctrl->pitchL, 0, sizeof(ctrl->pitchL));
            c->indices.lagIndex     = 0;
            c->indices.contourIndex = 0;
            enc->LTPCorr            = 0;
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
