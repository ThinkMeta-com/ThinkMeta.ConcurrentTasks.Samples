// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "PsyMath.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "SessionConfig.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        constexpr std::int32_t Log2SizeL2 = FastLog2SizeL2;
        constexpr std::int32_t Log2Size   = 1 << Log2SizeL2;

        struct LogTable {
            std::array<float, Log2Size + 1> values;

            // init_log_table: log2 over [1, 2]; C computes log() of the float argument in double
            LogTable() noexcept
            {
                for (std::int32_t j = 0; j < Log2Size + 1; j++)
                    values[j] = static_cast<float>(std::log(static_cast<double>(1.0f + j / static_cast<float>(Log2Size))) / std::log(2.0));
            }
        };

        const LogTable logTable;

        // ATHformula_GB: the ATH curve by Painter & Spanias, modified by Gabriel Bouvigne
        float AthFormulaGb(float f, float value, float fMin, float fMax) noexcept
        {
            // the following hack allows to ask for the lowest value
            if (f < -.3)
                f = 3410;

            f /= 1000; // convert to khz
            f = Max(fMin, f);
            f = Min(fMax, f);

            const double fd  = f;
            const double ath = 3.640 * std::pow(fd, -0.8) - 6.800 * std::exp(-0.6 * std::pow(fd - 3.4, 2.0)) + 6.000 * std::exp(-0.15 * std::pow(fd - 8.7, 2.0))
                + (0.6 + 0.04 * value) * 0.001 * std::pow(fd, 4.0);
            return static_cast<float>(ath);
        }

    } // namespace

    float FastLog2(float x) noexcept
    {
        std::int32_t bits;
        std::memcpy(&bits, &x, sizeof(bits));
        std::int32_t mantissa = bits & 0x7fffff;
        float        log2val  = static_cast<float>(((bits >> 23) & 0xFF) - 0x7f);
        float        partial  = static_cast<float>(mantissa & ((1 << (23 - Log2SizeL2)) - 1));
        partial *= 1.0f / ((1 << (23 - Log2SizeL2)));

        mantissa >>= (23 - Log2SizeL2);

        // without interpolation the results are not good
        log2val += logTable.values[mantissa] * (1.0f - partial) + logTable.values[mantissa + 1] * partial;
        return log2val;
    }

    const float* FastLog2Table() noexcept
    {
        return logTable.values.data();
    }

    // see for example "Zwicker: Psychoakustik, 1982; ISBN 3-540-11401-7
    float FreqToBark(float freq) noexcept
    {
        if (freq < 0)
            freq = 0;
        freq = static_cast<float>(freq * 0.001);
        return static_cast<float>(13.0 * std::atan(.76 * freq) + 3.5 * std::atan(freq * freq / (7.5 * 7.5)));
    }

    float AthFormula(const SessionConfig& config, float freq) noexcept
    {
        switch (config.ATHtype) {
            case 0: return AthFormulaGb(freq, 9, 0.1f, 24.0f);
            case 1: return AthFormulaGb(freq, -1, 0.1f, 24.0f); // over sensitive, should probably be removed
            case 2: return AthFormulaGb(freq, 0, 0.1f, 24.0f);
            case 3: return AthFormulaGb(freq, 1, 0.1f, 24.0f) + 6; // modification of GB formula by Roel
            case 4: return AthFormulaGb(freq, config.ATHcurve, 0.1f, 24.0f);
            case 5: return AthFormulaGb(freq, config.ATHcurve, 3.41f, 16.1f);
            default: return AthFormulaGb(freq, 0, 0.1f, 24.0f);
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
