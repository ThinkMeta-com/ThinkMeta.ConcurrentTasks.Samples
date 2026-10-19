// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "RangeEncoder.h"
#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    // Port of the entropy coding and the gain quantisation of the SILK encoder (libopus 1.6.1
    // silk/encode_indices.c, encode_pulses.c, shell_coder.c, code_signs.c, NLSF_unpack.c,
    // gain_quant.c).

    /// <summary>silk_encode_indices: the side information of the frame (or of LBRR frame frameIndex).</summary>
    void EncodeIndices(SilkEncoderState* enc, RangeEncoder* rangeEnc, std::int32_t frameIndex, bool encodeLbrr, std::int32_t condCoding) noexcept;

    /// <summary>silk_encode_pulses: the quantisation indices of the excitation (pulses has room for 16 more).</summary>
    void EncodePulses(RangeEncoder* rangeEnc, std::int32_t signalType, std::int32_t quantOffsetType, std::int8_t pulses[], std::int32_t frameLength) noexcept;

    /// <summary>silk_NLSF_unpack: the entropy table indices and the predictor of codebook vector cb1Index.</summary>
    void NlsfUnpack(std::int16_t ecIx[], std::uint8_t predQ8[], const SilkNlsfCb* nlsfCb, std::int32_t cb1Index) noexcept;

    /// <summary>silk_gains_quant: scalar gain quantisation with hysteresis, uniform on the log scale.</summary>
    void GainsQuant(std::int8_t ind[SilkMaxNbSubfr], std::int32_t gainQ16[SilkMaxNbSubfr], std::int8_t* prevInd, bool conditional,
                    std::int32_t nbSubfr) noexcept;

    /// <summary>silk_gains_dequant.</summary>
    void GainsDequant(std::int32_t gainQ16[SilkMaxNbSubfr], const std::int8_t ind[SilkMaxNbSubfr], std::int8_t* prevInd, bool conditional,
                      std::int32_t nbSubfr) noexcept;

    /// <summary>silk_gains_ID: a unique identifier of the gain index vector.</summary>
    std::int32_t GainsId(const std::int8_t ind[SilkMaxNbSubfr], std::int32_t nbSubfr) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
