// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Fft.h"

#include "FftKernels.h"

#include <cmath>
#include <cstdint>

// Port of fft.c. The arithmetic follows LAME literally: the double constants (SQRT2, the
// windows' cos terms) make parts of it double precision, rounded to float on assignment.

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    const std::array<float, FhtTriSize * 2> FhtCostab
        = {static_cast<float>(9.238795325112867e-01), static_cast<float>(3.826834323650898e-01), static_cast<float>(9.951847266721969e-01),
           static_cast<float>(9.801714032956060e-02), static_cast<float>(9.996988186962042e-01), static_cast<float>(2.454122852291229e-02),
           static_cast<float>(9.999811752826011e-01), static_cast<float>(6.135884649154475e-03)};

    void FhtScalar(float* fz, std::int32_t n) noexcept
    {
        const float* tri = FhtCostab.data();
        std::int32_t k4;
        float *      fi, *gi;
        float const* fn;

        n <<= 1; // to get BLKSIZE, because of 3DNow! ASM routine
        fn = fz + n;
        k4 = 4;
        do {
            float        s1, c1;
            std::int32_t i, k1, k2, k3, kx;
            kx = k4 >> 1;
            k1 = k4;
            k2 = k4 << 1;
            k3 = k2 + k1;
            k4 = k2 << 1;
            fi = fz;
            gi = fi + kx;
            do {
                float f0, f1, f2, f3;
                f1     = fi[0] - fi[k1];
                f0     = fi[0] + fi[k1];
                f3     = fi[k2] - fi[k3];
                f2     = fi[k2] + fi[k3];
                fi[k2] = f0 - f2;
                fi[0]  = f0 + f2;
                fi[k3] = f1 - f3;
                fi[k1] = f1 + f3;
                f1     = gi[0] - gi[k1];
                f0     = gi[0] + gi[k1];
                f3     = static_cast<float>(SQRT2 * gi[k3]);
                f2     = static_cast<float>(SQRT2 * gi[k2]);
                gi[k2] = f0 - f2;
                gi[0]  = f0 + f2;
                gi[k3] = f1 - f3;
                gi[k1] = f1 + f3;
                gi += k4;
                fi += k4;
            } while (fi < fn);
            c1 = tri[0];
            s1 = tri[1];
            for (i = 1; i < kx; i++) {
                float c2, s2;
                c2 = 1 - (2 * s1) * s1;
                s2 = (2 * s1) * c1;
                fi = fz + i;
                gi = fz + k1 - i;
                do {
                    float a, b, g0, f0, f1, g1, f2, g2, f3, g3;
                    b      = s2 * fi[k1] - c2 * gi[k1];
                    a      = c2 * fi[k1] + s2 * gi[k1];
                    f1     = fi[0] - a;
                    f0     = fi[0] + a;
                    g1     = gi[0] - b;
                    g0     = gi[0] + b;
                    b      = s2 * fi[k3] - c2 * gi[k3];
                    a      = c2 * fi[k3] + s2 * gi[k3];
                    f3     = fi[k2] - a;
                    f2     = fi[k2] + a;
                    g3     = gi[k2] - b;
                    g2     = gi[k2] + b;
                    b      = s1 * f2 - c1 * g3;
                    a      = c1 * f2 + s1 * g3;
                    fi[k2] = f0 - a;
                    fi[0]  = f0 + a;
                    gi[k3] = g1 - b;
                    gi[k1] = g1 + b;
                    b      = c1 * g2 - s1 * f3;
                    a      = s1 * g2 + c1 * f3;
                    gi[k2] = g0 - a;
                    gi[0]  = g0 + a;
                    fi[k3] = f1 - b;
                    fi[k1] = f1 + b;
                    gi += k4;
                    fi += k4;
                } while (fi < fn);
                c2 = c1;
                c1 = c2 * tri[0] - s1 * tri[1];
                s1 = c2 * tri[1] + s1 * tri[0];
            }
            tri += 2;
        } while (k4 < n);
    }

    FhtFunction FhtFor(SimdLevel level) noexcept
    {
        switch (level) {
            case SimdLevel::Sse41: return FhtSse41;
            case SimdLevel::Avx2: return FhtAvx2;
            default: return FhtScalar;
        }
    }

    namespace {

        const auto rv_tbl = std::to_array<std::uint8_t>(
            {0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0, 0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8,
             0x68, 0xe8, 0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8, 0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4,
             0x34, 0xb4, 0x74, 0xf4, 0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc, 0x02, 0x82,
             0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2, 0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea,
             0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa, 0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6,
             0x76, 0xf6, 0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee, 0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe});

    } // namespace

#define ch01(index) (buffer[index])

#define ml00(f) (window[i] * f(i))
#define ml10(f) (window[i + 0x200] * f(i + 0x200))
#define ml20(f) (window[i + 0x100] * f(i + 0x100))
#define ml30(f) (window[i + 0x300] * f(i + 0x300))

#define ml01(f) (window[i + 0x001] * f(i + 0x001))
#define ml11(f) (window[i + 0x201] * f(i + 0x201))
#define ml21(f) (window[i + 0x101] * f(i + 0x101))
#define ml31(f) (window[i + 0x301] * f(i + 0x301))

#define ms00(f) (window_s[i] * f(i + k))
#define ms10(f) (window_s[0x7f - i] * f(i + k + 0x80))
#define ms20(f) (window_s[i + 0x40] * f(i + k + 0x40))
#define ms30(f) (window_s[0x3f - i] * f(i + k + 0xc0))

#define ms01(f) (window_s[i + 0x01] * f(i + k + 0x01))
#define ms11(f) (window_s[0x7e - i] * f(i + k + 0x81))
#define ms21(f) (window_s[i + 0x41] * f(i + k + 0x41))
#define ms31(f) (window_s[0x3e - i] * f(i + k + 0xc1))

    void FftShort(const FftWindows& windows, std::array<std::array<float, BLKSIZE_s>, 3>& x_real, const sample_t* buffer) noexcept
    {
        const float* const window_s = windows.window_s.data();
        std::int32_t       i;
        std::int32_t       j;
        std::int32_t       b;

        for (b = 0; b < 3; b++) {
            float*             x = &x_real[b][BLKSIZE_s / 2];
            const std::int16_t k = static_cast<std::int16_t>((576 / 3) * (b + 1));
            j                    = BLKSIZE_s / 8 - 1;
            do {
                float f0, f1, f2, f3, w;

                i = rv_tbl[j << 2];

                f0 = ms00(ch01);
                w  = ms10(ch01);
                f1 = f0 - w;
                f0 = f0 + w;
                f2 = ms20(ch01);
                w  = ms30(ch01);
                f3 = f2 - w;
                f2 = f2 + w;

                x -= 4;
                x[0] = f0 + f2;
                x[2] = f0 - f2;
                x[1] = f1 + f3;
                x[3] = f1 - f3;

                f0 = ms01(ch01);
                w  = ms11(ch01);
                f1 = f0 - w;
                f0 = f0 + w;
                f2 = ms21(ch01);
                w  = ms31(ch01);
                f3 = f2 - w;
                f2 = f2 + w;

                x[BLKSIZE_s / 2 + 0] = f0 + f2;
                x[BLKSIZE_s / 2 + 2] = f0 - f2;
                x[BLKSIZE_s / 2 + 1] = f1 + f3;
                x[BLKSIZE_s / 2 + 3] = f1 - f3;
            } while (--j >= 0);

            windows.fht(x, BLKSIZE_s / 2); // BLKSIZE_s/2 because of 3DNow! ASM routine
        }
    }

    void FftLong(const FftWindows& windows, std::array<float, BLKSIZE>& out, const sample_t* buffer) noexcept
    {
        float*             x      = out.data();
        const float* const window = windows.window.data();
        std::int32_t       i;
        std::int32_t       jj = BLKSIZE / 8 - 1;
        x += BLKSIZE / 2;

        do {
            float f0, f1, f2, f3, w;

            i  = rv_tbl[jj];
            f0 = ml00(ch01);
            w  = ml10(ch01);
            f1 = f0 - w;
            f0 = f0 + w;
            f2 = ml20(ch01);
            w  = ml30(ch01);
            f3 = f2 - w;
            f2 = f2 + w;

            x -= 4;
            x[0] = f0 + f2;
            x[2] = f0 - f2;
            x[1] = f1 + f3;
            x[3] = f1 - f3;

            f0 = ml01(ch01);
            w  = ml11(ch01);
            f1 = f0 - w;
            f0 = f0 + w;
            f2 = ml21(ch01);
            w  = ml31(ch01);
            f3 = f2 - w;
            f2 = f2 + w;

            x[BLKSIZE / 2 + 0] = f0 + f2;
            x[BLKSIZE / 2 + 2] = f0 - f2;
            x[BLKSIZE / 2 + 1] = f1 + f3;
            x[BLKSIZE / 2 + 3] = f1 - f3;
        } while (--jj >= 0);

        windows.fht(x, BLKSIZE / 2); // BLKSIZE/2 because of 3DNow! ASM routine
    }

#undef ch01
#undef ml00
#undef ml10
#undef ml20
#undef ml30
#undef ml01
#undef ml11
#undef ml21
#undef ml31
#undef ms00
#undef ms10
#undef ms20
#undef ms30
#undef ms01
#undef ms11
#undef ms21
#undef ms31

    void FftWindows::Initialize() noexcept
    {
        fht = FhtScalar;

        // The type of window used here will make no real difference, but in the interest of
        // merging nspsytune stuff - switch to blackman window
        for (std::int32_t i = 0; i < BLKSIZE; i++)
            window[i] = static_cast<float>(0.42 - 0.5 * std::cos(2 * PI * (i + .5) / BLKSIZE) + 0.08 * std::cos(4 * PI * (i + .5) / BLKSIZE));

        for (std::int32_t i = 0; i < BLKSIZE_s / 2; i++)
            window_s[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * PI * (i + 0.5) / BLKSIZE_s)));
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
