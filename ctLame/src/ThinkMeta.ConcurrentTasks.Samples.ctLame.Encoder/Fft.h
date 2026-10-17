// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>

#include "FftKernels.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>The analysis windows of the FFT (cd_psy->window and window_s, init_fft) and its Hartley transform.</summary>
    struct FftWindows {
        std::array<float, BLKSIZE>       window;
        std::array<float, BLKSIZE_s / 2> window_s;
        FhtFunction                      fht; // FhtScalar after Initialize, FhtFor(level) for SIMD

        void Initialize() noexcept;
    };

    /// <summary>
    /// Port of fft.c: windowed FFT of 1024 samples (a Hartley transform, as LAME computes it).
    /// buffer points at the first sample of the block.
    /// </summary>
    void FftLong(const FftWindows& windows, std::array<float, BLKSIZE>& x, const sample_t* buffer) noexcept;

    /// <summary>The three windowed short-block FFTs of 256 samples of a granule.</summary>
    void FftShort(const FftWindows& windows, std::array<std::array<float, BLKSIZE_s>, 3>& x, const sample_t* buffer) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
