// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Presets.h"

#include <array>
#include <cmath>
#include <cstdint>

#include "EncoderOptions.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    namespace {

        struct VbrPreset {
            std::int32_t vbr_q;
            std::int32_t expY;
            float        st_lrm; // short threshold
            float        st_s;
            float        masking_adj;
            float        masking_adj_short;
            float        ath_lower;
            float        ath_curve;
            float        ath_sensitivity;
            float        interch;
            std::int32_t safejoint;
            std::int32_t sfb21mod;
            float        msfix;
            float        minval;
            float        ath_fixpoint;
        };

        // The literals are doubles stored in float fields, as in LAME's table.
#pragma warning(push)
#pragma warning(disable : 4305)
        // clang-format off
        const auto VbrMtPsySwitchMap = std::to_array<VbrPreset>({
        //  vbr_q expY st_lrm  st_s   mask adj_l adj_s  ath_lower ath_curve ath_sens interch  safejoint sfb21mod msfix minval ath_fixpoint
            {0,   0,   4.20,   25.0,  -6.8,  -6.8,   7.1,     1,        0,      0,       2,   31,  1.000,  5, 100},
            {1,   0,   4.20,   25.0,  -4.8,  -4.8,   5.4,     1.4,     -1,      0,       2,   27,  1.122,  5,  98},
            {2,   0,   4.20,   25.0,  -2.6,  -2.6,   3.7,     2.0,     -3,      0,       2,   23,  1.288,  5,  97},
            {3,   1,   4.20,   25.0,  -1.6,  -1.6,   2.0,     2.0,     -5,      0,       2,   18,  1.479,  5,  96},
            {4,   1,   4.20,   25.0,  -0.0,  -0.0,   0.0,     2.0,     -8,      0,       2,   12,  1.698,  5,  95},
            {5,   1,   4.20,   25.0,   1.3,   1.3,  -6,       3.5,    -11,      0,       2,    8,  1.950,  5,  94.2},
            {6,   1,   4.50,  100.0,   2.2,   2.3, -12.0,     6.0,    -14,      0,       2,    4,  2.239,  3,  93.9},
            {7,   1,   4.80,  200.0,   2.7,   2.7, -18.0,     9.0,    -17,      0,       2,    0,  2.570,  1,  93.6},
            {8,   1,   5.30,  300.0,   2.8,   2.8, -21.0,    10.0,    -23,      0.0002,  0,    0,  2.951,  0,  93.3},
            {9,   1,   6.60,  300.0,   2.8,   2.8, -23.0,    11.0,    -25,      0.0006,  0,    0,  3.388,  0,  93.3},
            {10,  1,  25.00,  300.0,   2.8,   2.8, -25.0,    12.0,    -27,      0.0025,  0,    0,  3.500,  0,  93.3}
        });
        // clang-format on
#pragma warning(pop)

        // SET_OPTION: with enforce always, otherwise only if the value is still the default.
        template <typename Setter>
        void SetOption(std::int32_t enforce, float current, std::int32_t def, float value, Setter set) noexcept
        {
            if (enforce)
                set(value);
            else if (!(std::fabs(static_cast<double>(current - def)) > 0))
                set(value);
        }

        // LAME: apply_vbr_preset() (presets.c)
        void ApplyVbrPreset(EncoderOptions& o, std::int32_t a, std::int32_t enforce) noexcept
        {
            const float x = o.vbrQFrac;
            VbrPreset   p = VbrMtPsySwitchMap[a];
            VbrPreset   q = VbrMtPsySwitchMap[a + 1];

            // LERP(m): p.m = p.m + x * (q.m - p.m)
            p.st_lrm            = p.st_lrm + x * (q.st_lrm - p.st_lrm);
            p.st_s              = p.st_s + x * (q.st_s - p.st_s);
            p.masking_adj       = p.masking_adj + x * (q.masking_adj - p.masking_adj);
            p.masking_adj_short = p.masking_adj_short + x * (q.masking_adj_short - p.masking_adj_short);
            p.ath_lower         = p.ath_lower + x * (q.ath_lower - p.ath_lower);
            p.ath_curve         = p.ath_curve + x * (q.ath_curve - p.ath_curve);
            p.ath_sensitivity   = p.ath_sensitivity + x * (q.ath_sensitivity - p.ath_sensitivity);
            p.interch           = p.interch + x * (q.interch - p.interch);
            p.sfb21mod          = static_cast<std::int32_t>(p.sfb21mod + x * (q.sfb21mod - p.sfb21mod));
            p.msfix             = p.msfix + x * (q.msfix - p.msfix);
            p.minval            = p.minval + x * (q.minval - p.minval);
            p.ath_fixpoint      = p.ath_fixpoint + x * (q.ath_fixpoint - p.ath_fixpoint);

            const VbrPreset& set = p;

            (void)o.SetVbrQ(set.vbr_q);
            if (set.expY)
                (void)o.SetExperimentalY(set.expY);
            SetOption(enforce, o.attackThre, -1, set.st_lrm, [&](float v) { o.SetShortThresholdLrm(v); });
            SetOption(enforce, o.attackThreS, -1, set.st_s, [&](float v) { o.SetShortThresholdS(v); });
            SetOption(enforce, o.maskingAdjust, 0, set.masking_adj, [&](float v) { o.SetMaskingAdjust(v); });
            SetOption(enforce, o.maskingAdjustShort, 0, set.masking_adj_short, [&](float v) { o.SetMaskingAdjustShort(v); });
            o.SetAthType(5);
            SetOption(enforce, o.athLowerDb, 0, set.ath_lower, [&](float v) { o.SetAthLower(v); });
            SetOption(enforce, o.athCurve, -1, set.ath_curve, [&](float v) { o.SetAthCurve(v); });
            SetOption(enforce, o.athaaSensitivity, 0, set.ath_sensitivity, [&](float v) { o.SetAthaaSensitivity(v); });
            if (set.interch > 0)
                SetOption(enforce, o.interChRatio, -1, set.interch, [&](float v) { o.SetInterChRatio(v); });

            // parameters for which there is no proper set/get interface
            if (set.safejoint > 0)
                (void)o.SetExpNspsytune(o.expNspsytune | 2);
            if (set.sfb21mod > 0) {
                const std::int32_t nsp = o.expNspsytune;
                const std::int32_t val = (nsp >> 20) & 63;
                if (val == 0) {
                    const std::int32_t sf21mod = (set.sfb21mod << 20) | nsp;
                    (void)o.SetExpNspsytune(sf21mod);
                }
            }
            SetOption(enforce, o.msfix, -1, set.msfix, [&](float v) { o.SetMsfix(v); });

            if (enforce == 0) {
                o.vbrQ     = a;
                o.vbrQFrac = x;
            }
            o.presetMinval = set.minval;
            { // take care of gain adjustments
                const double scaleAbs = std::fabs(static_cast<double>(o.scale));
                const double y        = (scaleAbs > 0.f) ? (10.f * std::log10(scaleAbs)) : 0.f;
                o.presetAthFixpoint   = static_cast<float>(set.ath_fixpoint - y);
            }
        }

    } // namespace

    std::int32_t ApplyPreset(EncoderOptions& options, std::int32_t preset, std::int32_t enforce) noexcept
    {
        // translate legacy presets
        switch (preset) {
            case R3Mix: preset = V3; break;
            case Medium:
            case MediumFast: preset = V4; break;
            case Standard:
            case StandardFast: preset = V2; break;
            case Extreme:
            case ExtremeFast: preset = V0; break;
            default: break;
        }

        options.preset = preset;
        switch (preset) {
            case V9: ApplyVbrPreset(options, 9, enforce); return preset;
            case V8: ApplyVbrPreset(options, 8, enforce); return preset;
            case V7: ApplyVbrPreset(options, 7, enforce); return preset;
            case V6: ApplyVbrPreset(options, 6, enforce); return preset;
            case V5: ApplyVbrPreset(options, 5, enforce); return preset;
            case V4: ApplyVbrPreset(options, 4, enforce); return preset;
            case V3: ApplyVbrPreset(options, 3, enforce); return preset;
            case V2: ApplyVbrPreset(options, 2, enforce); return preset;
            case V1: ApplyVbrPreset(options, 1, enforce); return preset;
            case V0: ApplyVbrPreset(options, 0, enforce); return preset;
            default: break;
        }
        options.preset = 0; // no corresponding preset found
        return preset;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
