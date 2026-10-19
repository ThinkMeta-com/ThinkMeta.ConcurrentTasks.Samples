// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>
    /// clt_mdct_forward of libopus 1.6.1 (celt/mdct.c) for the static 48 kHz mode: the windowed
    /// forward MDCT of size 1920 >> shift over in (with the overlap of the window), out written
    /// with the given stride (interleaving short blocks).
    /// </summary>
    void MdctForward(const float* in, float* out, std::int32_t shift, std::int32_t stride) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
