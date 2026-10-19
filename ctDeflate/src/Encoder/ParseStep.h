//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    /// <summary>
    /// New in ctDeflate, not in libdeflate: the record of a speculative parse, replayed by the resolver.
    /// Parser output, one entry per iteration of libdeflate's parse loop (the unit at which a block may end):
    /// length 0: 'literals' iterations of one literal each; length &gt; 0: one iteration of 'literals' literals (lazy
    /// evaluation that found a better match one or two bytes later) followed by a match.
    /// </summary>
    struct ParseStep {
        std::uint32_t literals;
        std::uint16_t length;
        std::uint16_t offset;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
