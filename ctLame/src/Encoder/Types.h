// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>Values per granule and channel of a frame, indexed [granule][channel].</summary>
    template <typename T>
    using GranuleChannel = std::array<std::array<T, 2>, 2>;

    // Port of the enums of include/lame.h and libmp3lame/lame_global_flags.h. The numeric values
    // are the LAME ones: some are printed or used as table indices.

    /// <summary>MPEG_mode.</summary>
    enum MpegMode : std::int32_t { Stereo = 0, JointStereo, DualChannel, Mono, NotSet, MaxIndicator };

    /// <summary>short_block_t.</summary>
    enum ShortBlock : std::int32_t { ShortBlockNotSet = -1, ShortBlockAllowed = 0, ShortBlockCoupled = 1, ShortBlockDispensed = 2, ShortBlockForced = 3 };

    /// <summary>buffer_constraint (MDB_*).</summary>
    enum BufferConstraint : std::int32_t { MdbDefault = 0, MdbStrictIso = 1, MdbMaximum = 2 };

    /// <summary>preset_mode.</summary>
    enum Preset : std::int32_t {
        V9           = 410,
        V8           = 420,
        V7           = 430,
        V6           = 440,
        V5           = 450,
        V4           = 460,
        V3           = 470,
        V2           = 480,
        V1           = 490,
        V0           = 500,
        R3Mix        = 1000,
        Standard     = 1001,
        Extreme      = 1002,
        StandardFast = 1004,
        ExtremeFast  = 1005,
        Medium       = 1006,
        MediumFast   = 1007
    };

    inline constexpr std::int32_t LameDefaultQuality = 3;

    /// <summary>MAX_U_32_NUM: number of samples unknown.</summary>
    inline constexpr std::uint32_t UnknownNumSamples = 0xFFFFFFFFul;

    inline constexpr std::int32_t SBMAX_l = 22;
    inline constexpr std::int32_t SBMAX_s = 13;
    inline constexpr std::int32_t PSFB21  = 6;
    inline constexpr std::int32_t PSFB12  = 6;

    inline constexpr double PI    = 3.14159265358979323846;
    inline constexpr double SQRT2 = 1.41421356237309504880;

    /// <summary>LAME's sample_t: the internal PCM representation.</summary>
    using sample_t = float;

    /// <summary>The samples of both channels of a frame or granule (for mono both point to the same channel).</summary>
    using ChannelSamples = std::array<const sample_t*, 2>;

    // encoder.h and util.h
    inline constexpr std::int32_t ENCDELAY   = 576;  // encoder delay
    inline constexpr std::int32_t POSTDELAY  = 1152; // padding at the end
    inline constexpr std::int32_t MDCTDELAY  = 48;   // delay of the polyphase filterbank and MDCT
    inline constexpr std::int32_t FFTOFFSET  = 224 + MDCTDELAY;
    inline constexpr std::int32_t SBLIMIT    = 32;
    inline constexpr std::int32_t BLKSIZE    = 1024;
    inline constexpr std::int32_t HBLKSIZE   = BLKSIZE / 2 + 1;
    inline constexpr std::int32_t BLKSIZE_s  = 256;
    inline constexpr std::int32_t HBLKSIZE_s = BLKSIZE_s / 2 + 1;
    inline constexpr std::int32_t MFSIZE     = 3 * 1152 + ENCDELAY - MDCTDELAY;
    inline constexpr std::int32_t FRAMESIZE  = 576 * 2; // PCM samples per MPEG-1 frame

    /// <summary>PCM samples per granule and channel, also the MDCT lines of a granule (576 in LAME's code).</summary>
    inline constexpr std::int32_t GranuleSize = FRAMESIZE / 2;

    // block types
    inline constexpr std::int32_t NORM_TYPE  = 0;
    inline constexpr std::int32_t START_TYPE = 1;
    inline constexpr std::int32_t SHORT_TYPE = 2;
    inline constexpr std::int32_t STOP_TYPE  = 3;

    /// <summary>
    /// A float constant written the way LAME writes it: a double literal that its FLOAT
    /// variables or tables round to float (LameFloat(-7.4) is LAME's (FLOAT) -7.4 or a -7.4 in a
    /// FLOAT table). The double literal keeps the bits: -7.4f would be the same here, but not for
    /// every value, and a product like 1.33352 * 1.33352 must be rounded once, after the multiplication.
    /// </summary>
    constexpr float LameFloat(double value) noexcept
    {
        return static_cast<float>(value);
    }

    // LAME's Min/Max macros
    template <typename T>
    constexpr T Min(T a, T b) noexcept
    {
        return a < b ? a : b;
    }

    template <typename T>
    constexpr T Max(T a, T b) noexcept
    {
        return a > b ? a : b;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
