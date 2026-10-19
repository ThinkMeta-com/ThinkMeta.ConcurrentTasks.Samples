// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

#include "FrameData.h"
#include "Psy.h"
#include "QuantizeTypes.h"
#include "Reservoir.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    struct SessionConfig;

    /// <summary>
    /// What the quantizer carries from frame to frame. sv_qnt.masking_lower is not here: VBR_new
    /// sets it to the same value every frame, so the psychoacoustic model, the only reader, knows
    /// it itself.
    /// </summary>
    struct QuantizeState {
        ReservoirState reservoir;
        std::int32_t   substepShaping = 0; // sv_qnt.substep_shaping
    };

    /// <summary>
    /// iteration_init: the ATH per scalefactor band (compute_ath), the power tables, the region
    /// split and the packed Huffman code lengths; also initializes the quantizer state.
    /// </summary>
    void QuantizeInit(const SessionConfig& config, QuantizeConstants& constants, AthTables& ath, QuantizeState& state) noexcept;

    /// <summary>
    /// calc_xmin: the allowed distortion per scalefactor band from the masking ratio and the
    /// ATH, lowered by athAdjustFactor. Also sets gi.max_nonzero_coeff and gi.energy_above_cutoff.
    /// Returns the number of bands with energy above the ATH.
    /// </summary>
    std::int32_t CalcXmin(const SessionConfig& config, const AthTables& ath, float athAdjustFactor, float psyDecay, const PsyRatio& ratio, GrInfo& gi,
                          float* xmin) noexcept;

    // VBR_new_iteration_loop in two parts: QuantSearch does what does not depend
    // on the reservoir and may run for several frames at once; QuantCommit takes the frames in
    // order.

    /// <summary>
    /// The first part of VBR_new_iteration_loop: M/S conversion (frame.modeExt), init_outer_loop,
    /// calc_xmin with the M/S or L/R masking, xrpow, the scalefactor search and the quantization
    /// "as is". psyDecay is the psychoacoustic model's decay for temporal masking. Reads only
    /// constants, so it is safe to run for several frames at once.
    /// </summary>
    void QuantSearch(const SessionConfig& config, const QuantizeConstants& constants, const AthTables& ath, float psyDecay, FrameData& frame) noexcept;

    /// <summary>
    /// The second part of VBR_new_iteration_loop, for the frames in order: the reservoir limits
    /// (ResvFrameBegin, on_pe), the out-of-bits strategy if the frame does not fit, the choice
    /// of the bitrate (frame.bitrateIndex) and the reservoir update. Returns false on LAME's
    /// "INTERNAL ERROR IN VBR NEW CODE".
    /// </summary>
    bool QuantCommit(const SessionConfig& config, const QuantizeConstants& constants, QuantizeState& state, FrameData& frame) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
