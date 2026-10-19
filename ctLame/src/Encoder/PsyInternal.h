// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

#include "Psy.h"
#include "SessionConfig.h"

// Internal to the psychoacoustic model (PsyInit.cpp, PsyPrepare.cpp, PsyResolve.cpp): what more
// than one of its parts uses.

namespace ThinkMeta::ConcurrentTasks::Samples::Lame::PsyDetail {

    /// <summary>The channels the model analyzes: L and R, for joint stereo also M and S (chn 2 and 3).</summary>
    inline std::int32_t PsyChannels(const SessionConfig& cfg) noexcept
    {
        return (cfg.mode == JointStereo) ? 4 : cfg.channels_out;
    }

    /// <summary>
    /// LAME: vbrpsy_compute_fft_s (psymodel.c): the FFT of short block sblock (for chn 2 and 3
    /// M/S from the L/R transforms in wsampS) and its line energies. No state; PsyResolve runs it
    /// only where the attacks ask for short blocks.
    /// </summary>
    void ComputeFftShort(const PsyConstants& gd, const ChannelSamples& buffer, std::int32_t chn, std::int32_t sblock,
                         std::array<std::array<float, HBLKSIZE_s>, 3>& fftenergyS, std::array<std::array<float, BLKSIZE_s>, 3>* wsampS) noexcept;

    /// <summary>
    /// LAME: vbrpsy_compute_masking_s (psymodel.c), the part without state: energy, tonality and
    /// the spreading of one short block.
    /// </summary>
    void SpreadShort(const PsyPartitions& gds, MaskChainsFunction maskChains, const float* fftenergyS, PsyBandPrep& out) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame::PsyDetail
