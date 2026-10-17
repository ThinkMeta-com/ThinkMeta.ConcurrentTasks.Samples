// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

#include "Report.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>
    /// Port of lame_global_flags (the user parameters) with the defaults of lame_init and the
    /// set/get functions of set_get.c that the command line and the presets use. The setters
    /// keep LAME's range checks and return codes (0 = accepted, -1 = rejected), because the
    /// frontend and the presets rely on them. InitParams completes and changes these values
    /// the same way lame_init_params changes the global flags.
    /// </summary>
    struct EncoderOptions {
        // input description
        std::uint32_t numSamples    = UnknownNumSamples;
        std::int32_t  numChannels   = 2;
        std::int32_t  samplerateIn  = 44100;
        std::int32_t  samplerateOut = 0;
        float         scale         = 1;
        float         scaleLeft     = 1;
        float         scaleRight    = 1;

        // general control
        std::int32_t writeLameTag   = 1;
        std::int32_t quality        = -1;
        MpegMode     mode           = NotSet;
        std::int32_t forceMs        = 0;
        std::int32_t substepShaping = 0;
        std::int32_t noiseShaping   = 0;
        std::int32_t subblockGain   = -1;
        std::int32_t useBestHuffman = 0;

        // frame
        std::int32_t copyright        = 0;
        std::int32_t original         = 1;
        std::int32_t extension        = 0;
        std::int32_t emphasis         = 0;
        std::int32_t errorProtection  = 0;
        std::int32_t strictIso        = MdbMaximum;
        std::int32_t disableReservoir = 0;

        // quantization/noise shaping
        std::int32_t experimentalY = 0;
        std::int32_t experimentalZ = 0;
        std::int32_t expNspsytune  = 0;
        std::int32_t preset        = 0;

        // VBR
        float        vbrQFrac          = 0;
        std::int32_t vbrQ              = 4;
        std::int32_t vbrMinBitrateKbps = 0;
        std::int32_t vbrMaxBitrateKbps = 0;
        std::int32_t vbrHardMin        = 0;

        // filtering
        std::int32_t lowpassFreq   = 0;
        std::int32_t highpassFreq  = 0;
        std::int32_t lowpassWidth  = -1;
        std::int32_t highpassWidth = -1;

        // psycho acoustics
        float        maskingAdjust      = 0;
        float        maskingAdjustShort = 0;
        std::int32_t athOnly            = 0;
        std::int32_t athShort           = 0;
        std::int32_t noAth              = 0;
        std::int32_t athType            = -1;
        float        athCurve           = -1;
        float        athLowerDb         = 0;
        std::int32_t athaaType          = -1;
        float        athaaSensitivity   = 0;
        ShortBlock   shortBlocks        = ShortBlockNotSet;
        std::int32_t useTemporal        = -1;
        float        interChRatio       = -1;
        float        msfix              = -1;
        float        attackThre         = -1;
        float        attackThreS        = -1;

        // Written by the presets into the session configuration (apply_vbr_preset writes
        // cfg->minval and cfg->ATHfixpoint directly); InitParams copies them over.
        float presetMinval      = 0;
        float presetAthFixpoint = 0;

        ReportFunctions report;

        // --- set/get functions (set_get.c) ---
        std::int32_t SetNumSamples(std::uint32_t value) noexcept;
        std::int32_t SetInSamplerate(std::int32_t value) noexcept;
        std::int32_t SetNumChannels(std::int32_t value) noexcept;
        std::int32_t SetScale(float value) noexcept;
        std::int32_t SetScaleLeft(float value) noexcept;
        std::int32_t SetScaleRight(float value) noexcept;
        std::int32_t SetWriteVbrTag(std::int32_t value) noexcept;
        std::int32_t SetQuality(std::int32_t value) noexcept;
        std::int32_t SetMode(MpegMode value) noexcept;
        std::int32_t SetForceMs(std::int32_t value) noexcept;
        std::int32_t SetCopyright(std::int32_t value) noexcept;
        std::int32_t SetOriginal(std::int32_t value) noexcept;
        std::int32_t SetErrorProtection(std::int32_t value) noexcept;
        std::int32_t SetStrictIso(std::int32_t value) noexcept;
        std::int32_t SetDisableReservoir(std::int32_t value) noexcept;
        std::int32_t SetExperimentalY(std::int32_t value) noexcept;
        std::int32_t SetExperimentalZ(std::int32_t value) noexcept;
        std::int32_t SetExpNspsytune(std::int32_t value) noexcept;
        std::int32_t SetVbrQ(std::int32_t value) noexcept;
        std::int32_t SetVbrQuality(float value) noexcept;
        std::int32_t SetVbrMinBitrateKbps(std::int32_t value) noexcept;
        std::int32_t SetVbrMaxBitrateKbps(std::int32_t value) noexcept;
        std::int32_t SetVbrHardMin(std::int32_t value) noexcept;
        std::int32_t SetLowpassFreq(std::int32_t value) noexcept;
        std::int32_t SetLowpassWidth(std::int32_t value) noexcept;
        std::int32_t SetHighpassFreq(std::int32_t value) noexcept;
        std::int32_t SetHighpassWidth(std::int32_t value) noexcept;
        std::int32_t SetMaskingAdjust(float value) noexcept;
        std::int32_t SetMaskingAdjustShort(float value) noexcept;
        std::int32_t SetAthType(std::int32_t value) noexcept;
        std::int32_t SetAthCurve(float value) noexcept;
        std::int32_t SetAthLower(float value) noexcept;
        std::int32_t SetAthaaSensitivity(float value) noexcept;
        std::int32_t SetInterChRatio(float value) noexcept;
        std::int32_t SetEmphasis(std::int32_t value) noexcept;
        std::int32_t SetShortThresholdLrm(float value) noexcept;
        std::int32_t SetShortThresholdS(float value) noexcept;
        void         SetMsfix(double value) noexcept;
        std::int32_t SetPreset(std::int32_t value) noexcept;

        float GetVbrQuality() const noexcept { return vbrQ + vbrQFrac; }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
