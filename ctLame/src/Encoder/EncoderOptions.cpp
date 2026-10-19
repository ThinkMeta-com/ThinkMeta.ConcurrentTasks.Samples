// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "EncoderOptions.h"

#include <cstdint>

#include "Presets.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    std::int32_t EncoderOptions::SetNumSamples(std::uint32_t value) noexcept
    {
        numSamples = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetInSamplerate(std::int32_t value) noexcept
    {
        if (value < 1)
            return -1;
        samplerateIn = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetNumChannels(std::int32_t value) noexcept
    {
        if (2 < value || 0 >= value)
            return -1;
        numChannels = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetScale(float value) noexcept
    {
        scale = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetScaleLeft(float value) noexcept
    {
        scaleLeft = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetScaleRight(float value) noexcept
    {
        scaleRight = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetWriteVbrTag(std::int32_t value) noexcept
    {
        if (0 > value || 1 < value)
            return -1;
        writeLameTag = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetQuality(std::int32_t value) noexcept
    {
        if (value < 0)
            quality = 0;
        else if (value > 9)
            quality = 9;
        else
            quality = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetMode(MpegMode value) noexcept
    {
        const std::int32_t mpgMode = value;
        if (mpgMode < 0 || MaxIndicator <= mpgMode)
            return -1;
        mode = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetForceMs(std::int32_t value) noexcept
    {
        if (0 > value || 1 < value)
            return -1;
        forceMs = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetCopyright(std::int32_t value) noexcept
    {
        if (0 > value || 1 < value)
            return -1;
        copyright = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetOriginal(std::int32_t value) noexcept
    {
        if (0 > value || 1 < value)
            return -1;
        original = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetErrorProtection(std::int32_t value) noexcept
    {
        if (0 > value || 1 < value)
            return -1;
        errorProtection = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetStrictIso(std::int32_t value) noexcept
    {
        if (value < MdbDefault || MdbMaximum < value)
            return -1;
        strictIso = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetDisableReservoir(std::int32_t value) noexcept
    {
        if (0 > value || 1 < value)
            return -1;
        disableReservoir = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetExperimentalY(std::int32_t value) noexcept
    {
        experimentalY = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetExperimentalZ(std::int32_t value) noexcept
    {
        experimentalZ = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetExpNspsytune(std::int32_t value) noexcept
    {
        expNspsytune = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetVbrQ(std::int32_t value) noexcept
    {
        std::int32_t ret = 0;
        if (0 > value) {
            ret   = -1;
            value = 0;
        }
        if (9 < value) {
            ret   = -1;
            value = 9;
        }
        vbrQ     = value;
        vbrQFrac = 0;
        return ret;
    }

    std::int32_t EncoderOptions::SetVbrQuality(float value) noexcept
    {
        std::int32_t ret = 0;
        if (0 > value) {
            ret   = -1;
            value = 0;
        }
        if (9.999 < value) {
            ret   = -1;
            value = static_cast<float>(9.999);
        }
        vbrQ     = static_cast<std::int32_t>(value);
        vbrQFrac = value - vbrQ;
        return ret;
    }

    std::int32_t EncoderOptions::SetVbrMinBitrateKbps(std::int32_t value) noexcept
    {
        vbrMinBitrateKbps = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetVbrMaxBitrateKbps(std::int32_t value) noexcept
    {
        vbrMaxBitrateKbps = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetVbrHardMin(std::int32_t value) noexcept
    {
        if (0 > value || 1 < value)
            return -1;
        vbrHardMin = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetLowpassFreq(std::int32_t value) noexcept
    {
        lowpassFreq = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetLowpassWidth(std::int32_t value) noexcept
    {
        lowpassWidth = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetHighpassFreq(std::int32_t value) noexcept
    {
        highpassFreq = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetHighpassWidth(std::int32_t value) noexcept
    {
        highpassWidth = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetMaskingAdjust(float value) noexcept
    {
        maskingAdjust = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetMaskingAdjustShort(float value) noexcept
    {
        maskingAdjustShort = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetAthType(std::int32_t value) noexcept
    {
        athType = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetAthCurve(float value) noexcept
    {
        athCurve = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetAthLower(float value) noexcept
    {
        athLowerDb = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetAthaaSensitivity(float value) noexcept
    {
        athaaSensitivity = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetInterChRatio(float value) noexcept
    {
        if (0 <= value && value <= 1.0) {
            interChRatio = value;
            return 0;
        }
        return -1;
    }

    std::int32_t EncoderOptions::SetEmphasis(std::int32_t value) noexcept
    {
        if (0 <= value && value < 4) {
            emphasis = value;
            return 0;
        }
        return -1;
    }

    std::int32_t EncoderOptions::SetShortThresholdLrm(float value) noexcept
    {
        attackThre = value;
        return 0;
    }

    std::int32_t EncoderOptions::SetShortThresholdS(float value) noexcept
    {
        attackThreS = value;
        return 0;
    }

    void EncoderOptions::SetMsfix(double value) noexcept
    {
        msfix = static_cast<float>(value);
    }

    std::int32_t EncoderOptions::SetPreset(std::int32_t value) noexcept
    {
        preset = value;
        return ApplyPreset(*this, value, 1);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
