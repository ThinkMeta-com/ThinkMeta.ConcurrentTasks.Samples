// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Tables.h"

#include <array>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    const std::array<std::int32_t, 16> BitrateTable = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1};

    const std::array<std::int32_t, 4> SamplerateTable = {44100, 48000, 32000, -1};

    const std::array<ScalefacStruct, 3> SfBandIndex = {{{// Table B.8.b: 44.1 kHz
                                                         {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576},
                                                         {0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192},
                                                         {0, 0, 0, 0, 0, 0, 0},
                                                         {0, 0, 0, 0, 0, 0, 0}},
                                                        {// Table B.8.c: 48 kHz
                                                         {0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576},
                                                         {0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192},
                                                         {0, 0, 0, 0, 0, 0, 0},
                                                         {0, 0, 0, 0, 0, 0, 0}},
                                                        {// Table B.8.a: 32 kHz
                                                         {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576},
                                                         {0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192},
                                                         {0, 0, 0, 0, 0, 0, 0},
                                                         {0, 0, 0, 0, 0, 0, 0}}}};

    namespace {
        std::int32_t Abs(std::int32_t a) noexcept
        {
            return a > 0 ? a : -a;
        }
    } // namespace

    std::int32_t FindNearestBitrate(std::int32_t bitrateKbps) noexcept
    {
        std::int32_t bitrate = BitrateTable[1];
        for (std::int32_t i = 2; i <= 14; i++) {
            if (BitrateTable[i] > 0) {
                if (Abs(BitrateTable[i] - bitrateKbps) < Abs(bitrate - bitrateKbps))
                    bitrate = BitrateTable[i];
            }
        }
        return bitrate;
    }

    std::int32_t BitrateIndex(std::int32_t bitrateKbps) noexcept
    {
        for (std::int32_t i = 0; i <= 14; i++) {
            if (BitrateTable[i] > 0 && BitrateTable[i] == bitrateKbps)
                return i;
        }
        return -1;
    }

    std::int32_t SmpFrqIndex(std::int32_t sampleRate) noexcept
    {
        switch (sampleRate) {
            case 44100: return 0;
            case 48000: return 1;
            case 32000: return 2;
            default: return -1;
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
