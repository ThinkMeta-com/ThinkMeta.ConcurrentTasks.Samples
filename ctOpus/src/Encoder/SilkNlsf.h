// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    // Port of the fixed-point LPC/NLSF functions of SILK (libopus 1.6.1 silk/A2NLSF.c, NLSF2A.c,
    // LPC_fit.c, bwexpander_32.c, LPC_inv_pred_gain.c, interpolate.c, process_NLSFs.c,
    // NLSF_encode.c, NLSF_VQ.c, NLSF_del_dec_quant.c, NLSF_VQ_weights_laroia.c, NLSF_decode.c,
    // NLSF_stabilize.c, sort.c).

    /// <summary>silk_A2NLSF: NLSFs (Q15) of monic whitening filter coefficients (Q16, bandwidth expanded until convergence).</summary>
    void A2Nlsf(std::int16_t* nlsf, std::int32_t* aQ16, std::int32_t d) noexcept;

    /// <summary>silk_NLSF2A: monic whitening filter coefficients (Q12) of NLSFs (Q15), d 10 or 16.</summary>
    void Nlsf2A(std::int16_t* aQ12, const std::int16_t* nlsf, std::int32_t d) noexcept;

    /// <summary>silk_interpolate: x0 + (x1 - x0) * ifactQ2 / 4.</summary>
    void Interpolate(std::int16_t xi[], const std::int16_t x0[], const std::int16_t x1[], std::int32_t ifactQ2, std::int32_t d) noexcept;

    /// <summary>
    /// silk_process_NLSFs: limits, stabilises, quantises the NLSFs (NLSFIndices) and converts them
    /// to the prediction coefficients of both frame halves (Q12).
    /// </summary>
    void ProcessNlsfs(SilkEncoderState* enc, std::int16_t predCoefQ12[2][SilkMaxLpcOrder], std::int16_t nlsfQ15[SilkMaxLpcOrder],
                      const std::int16_t prevNlsfqQ15[SilkMaxLpcOrder]) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
