// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    // Port of the linear prediction analysis of the SILK encoder (libopus 1.6.1
    // silk/float/find_pred_coefs_FLP.c, find_LTP_FLP.c, corrMatrix_FLP.c, LTP_scale_ctrl_FLP.c,
    // LTP_analysis_filter_FLP.c, scale_copy_vector_FLP.c, scale_vector_FLP.c, find_LPC_FLP.c,
    // burg_modified_FLP.c, residual_energy_FLP.c, the LPC/LTP wrappers of wrappers_FLP.c;
    // silk/quant_LTP_gains.c, VQ_WMat_EC.c; the SSE4.1 variant of libopus computes the same).

    /// <summary>
    /// silk_find_pred_coefs_FLP: the LTP coefficients (voiced), the LPC coefficients with the
    /// quantised NLSFs and the residual energies of the frame.
    /// </summary>
    void FindPredCoefs(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, const float resPitch[], const float x[], std::int32_t condCoding) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
