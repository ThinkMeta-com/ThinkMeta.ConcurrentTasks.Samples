// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>
    /// The static 48 kHz CELT mode of libopus 1.6.1 (mode48000_960_120, static_modes_float.h):
    /// 21 bands, 120-sample overlap, MDCT sizes 240 to 1920 (LM 0..3).
    /// </summary>
    namespace CeltMode {

        constexpr std::int32_t Fs             = 48000;
        constexpr std::int32_t Overlap        = 120;
        constexpr std::int32_t NbEBands       = 21;
        constexpr std::int32_t EffEBands      = 21;
        constexpr std::int32_t MaxLM          = 3;
        constexpr std::int32_t NbShortMdcts   = 8;
        constexpr std::int32_t ShortMdctSize  = 120;
        constexpr std::int32_t NbAllocVectors = 11;
        constexpr std::int32_t MdctSize       = 1920; // mdct.n
        constexpr std::int32_t CacheSize      = 392;  // cache.size

        constexpr float Preemph[4] = {0.85000610f, 0.0000000f, 1.0000000f, 1.0000000f};

        extern const std::int16_t EBands[NbEBands + 1]; // eband5ms
        extern const std::uint8_t AllocVectors[NbAllocVectors * NbEBands];
        extern const std::int16_t LogN[NbEBands];
        extern const float        Window[Overlap];
        extern const float        MdctTrig[1800];
        extern const std::int16_t CacheIndex[(MaxLM + 2) * NbEBands];
        extern const std::uint8_t CacheBits[CacheSize];
        extern const std::uint8_t CacheCaps[(MaxLM + 1) * 2 * NbEBands];

        /// <summary>The scalar parameters and tables of the mode as one value (the fields of CELTMode the encoder uses).</summary>
        struct View {
            std::int32_t        fs;
            std::int32_t        overlap;
            std::int32_t        nbEBands;
            std::int32_t        effEBands;
            float               preemph[4];
            const std::int16_t* eBands;
            std::int32_t        maxLM;
            std::int32_t        shortMdctSize;
            const std::int16_t* logN;
            const float*        window;
        };

        inline const View Mode
            = {Fs, Overlap, NbEBands, EffEBands, {Preemph[0], Preemph[1], Preemph[2], Preemph[3]}, EBands, MaxLM, ShortMdctSize, LogN, Window};

    } // namespace CeltMode

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
