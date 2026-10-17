// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Reservoir.h"

#include <cstdint>

#include "SessionConfig.h"
#include "Tables.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    std::int32_t FrameBits(const SessionConfig& config, std::int32_t bitrateIndex) noexcept
    {
        return CalcFrameLength(config, BitrateTable[bitrateIndex], 0);
    }

    std::int32_t ReservoirFrameBegin(const SessionConfig& config, ReservoirState& reservoir, SideInfo& side, std::int32_t bitrateIndex,
                                     std::int32_t& meanBits) noexcept
    {
        const std::int32_t frameLength = FrameBits(config, bitrateIndex);
        const std::int32_t mean        = (frameLength - config.sideinfo_len * 8) / 2;

        // main_data_begin has 9 bits in MPEG-1: at most 511 bytes may come from earlier frames
        const std::int32_t resvLimit = (8 * 256) * 2 - 8;

        // the largest frame buffer the decoder needs; the reservoir must not push a frame past it
        const std::int32_t maxmp3buf = config.buffer_constraint;
        reservoir.max                = maxmp3buf - frameLength;
        if (reservoir.max > resvLimit)
            reservoir.max = resvLimit;
        if (reservoir.max < 0 || config.disable_reservoir)
            reservoir.max = 0;

        std::int32_t fullFrameBits = mean * 2 + Min(reservoir.size, reservoir.max);
        if (fullFrameBits > maxmp3buf)
            fullFrameBits = maxmp3buf;

        side.resvDrain_pre = 0;
        meanBits           = mean;
        return fullFrameBits;
    }

    void ReservoirMaxBits(const SessionConfig& config, const ReservoirState& reservoir, std::int32_t& substepShaping, std::int32_t meanBits,
                          std::int32_t& targetBits, std::int32_t& extraBits) noexcept
    {
        const std::int32_t size = reservoir.size;
        std::int32_t       max  = reservoir.max;
        if (substepShaping & 1)
            max = static_cast<std::int32_t>(max * 0.9);

        std::int32_t target = meanBits;
        std::int32_t addBits;
        if (size * 10 > max * 9) {
            // the reservoir is almost full: spend what is above 90 %
            addBits = size - (max * 9) / 10;
            target += addBits;
            substepShaping |= 0x80;
        } else {
            addBits = 0;
            substepShaping &= 0x7f;
            // build up the reservoir, a little slower than FhG
            if (!config.disable_reservoir && !(substepShaping & 1))
                target = static_cast<std::int32_t>(target - .1 * meanBits);
        }

        // what may come from the reservoir: ISO says 6/10
        std::int32_t extra = size < (reservoir.max * 6) / 10 ? size : (reservoir.max * 6) / 10;
        extra -= addBits;
        if (extra < 0)
            extra = 0;

        targetBits = target;
        extraBits  = extra;
    }

    void ReservoirFrameEnd(ReservoirState& reservoir, SideInfo& side, std::int32_t meanBits) noexcept
    {
        reservoir.size += meanBits * 2;
        side.resvDrain_post = 0;
        side.resvDrain_pre  = 0;

        // the reservoir must be byte aligned and must not exceed its limit
        std::int32_t       stuffingBits = reservoir.size % 8;
        const std::int32_t over         = (reservoir.size - stuffingBits) - reservoir.max;
        if (over > 0)
            stuffingBits += over;

        // drain as much as possible into the ancillary data of the previous frame (main_data_begin
        // shrinks), the rest into this frame's
        const std::int32_t mdbBytes = Min(reservoir.mainDataBegin * 8, stuffingBits) / 8;
        side.resvDrain_pre += 8 * mdbBytes;
        stuffingBits -= 8 * mdbBytes;
        reservoir.size -= 8 * mdbBytes;
        reservoir.mainDataBegin -= mdbBytes;
        side.main_data_begin = reservoir.mainDataBegin;

        side.resvDrain_post += stuffingBits;
        reservoir.size -= stuffingBits;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
