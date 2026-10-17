// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

#include "QuantizeTypes.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    struct SessionConfig;

    // VBR_encode_frame (vbrquantize.c) in two parts. The search does not depend
    // on the bit reservoir: LAME gates it with max_bits > 0, which holds exactly for the granules
    // that are not silent. Only the check whether the bits fit and the out-of-bits strategy
    // need the limits from the reservoir.

    /// <summary>
    /// The first part of VBR_encode_frame: for each active (not silent) granule and channel,
    /// finds the scalefactors that keep the noise of each band below xmin, quantizes "as is"
    /// and counts the bits; reduce_bit_usage for all. xr34orig is |xr|^(3/4). Fills search
    /// for VbrFitFrame; search[gr][ch].active must be set.
    /// </summary>
    void VbrSearchFrame(const SessionConfig& config, const QuantizeConstants& constants, SideInfo& side, const GranuleChannel<std::array<float, 576>>& xr34orig,
                        const GranuleChannel<std::array<float, SFBMAX>>& xmin, GranuleChannel<GranuleSearch>& search) noexcept;

    /// <summary>
    /// The second part of VBR_encode_frame: if the frame or a granule is too large for maxBits
    /// (the bits each granule and channel may use, 0 for silence), flattens the scalefactor
    /// distribution until the bits fit; reworked tells whether it did. Returns the bits used, or
    /// -1 on LAME's "INTERNAL ERROR IN VBR NEW CODE".
    /// </summary>
    std::int32_t VbrFitFrame(const SessionConfig& config, const QuantizeConstants& constants, SideInfo& side,
                             const GranuleChannel<std::array<float, 576>>& xr34orig, const GranuleChannel<std::int32_t>& maxBits,
                             GranuleChannel<GranuleSearch>& search, bool& reworked) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
