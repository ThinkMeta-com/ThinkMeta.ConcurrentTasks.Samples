// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "VbrQuantize.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "BitCount.h"
#include "SessionConfig.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        // constant tables taken over from LAME (vbrquantize.c) as data: the largest scalefactor
        // each band can store (4 bits up to band 10 of long blocks and the first 6 short bands,
        // 3 bits above, nothing in band 21/12)
        const std::array<std::uint8_t, SBMAX_s * 3> max_range_short
            = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 0, 0, 0};
        const std::array<std::uint8_t, SBMAX_l> max_range_long = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 0};

        struct NoiseCache {
            std::int32_t valid;
            float        value;
        };

        // algo_t: one granule and channel during the scalefactor search
        class Granule
        {
        public:
            Granule(const SessionConfig& config, const QuantizeConstants& constants, GrInfo& gi, const float* xr34orig) noexcept
                : config_(config),
                  k_(constants),
                  gi_(gi),
                  xr34_(xr34orig),
                  short_(gi.block_type == SHORT_TYPE),
                  guess_(config.full_outer_loop < 0)
            {}

            GrInfo& Info() noexcept { return gi_; }
            bool    Failed() const noexcept { return failed_; }

            // the state the search leaves for the out-of-bits strategy
            void Save(GranuleSearch& search) const noexcept
            {
                search.mingainL = mingainL_;
                for (std::int32_t i = 0; i < 3; ++i)
                    search.mingainS[i] = mingainS_[i];
                search.failed = failed_;
            }
            void Restore(const GranuleSearch& search) noexcept
            {
                mingainL_ = search.mingainL;
                for (std::int32_t i = 0; i < 3; ++i)
                    mingainS_[i] = search.mingainS[i];
                failed_ = search.failed;
            }

            std::int32_t BlockSf(const std::array<float, SFBMAX>& xmin, SfbValues& vbrsf, SfbValues& vbrsfmin) noexcept;
            void         Constrain(const SfbValues& vbrsf, const SfbValues& vbrsfmin, std::int32_t vbrmax) noexcept
            {
                if (short_)
                    ShortBlockConstrain(vbrsf, vbrsfmin, vbrmax);
                else
                    LongBlockConstrain(vbrsf, vbrsfmin, vbrmax);
            }
            void Bitcount() noexcept
            {
                // the scalefactors are chosen so that they can always be stored
                if (ScaleBitcount(gi_))
                    failed_ = true;
            }
            std::int32_t QuantizeAndCountBits() noexcept;
            std::int32_t TryGlobalStepsize(const SfbValues& sfwork, const SfbValues& vbrsfmin, std::int32_t delta) noexcept;
            void         SearchGlobalStepsizeMax(const SfbValues& sfwork, const SfbValues& vbrsfmin, std::int32_t target) noexcept;
            std::int32_t TryThatOne(const SfbValues& sftemp, const SfbValues& vbrsfmin, std::int32_t vbrmax) noexcept;
            void         OutOfBitsStrategy(const SfbValues& sfwork, const SfbValues& vbrsfmin, std::int32_t target) noexcept;

        private:
            std::uint8_t FindLowestScalefac(float xr34) const noexcept;
            float        SfbNoise(const float* xr, const float* xr34, std::uint32_t bw, std::uint8_t sf) const noexcept;
            bool         NoiseAbove(const float* xr, const float* xr34, float xmin, std::uint32_t bw, std::uint8_t sf, NoiseCache* cache) const noexcept;
            std::uint8_t FindScalefac(const float* xr, const float* xr34, float xmin, std::uint32_t bw, std::uint8_t sfMin) const noexcept;
            std::uint8_t GuessScalefac(float xmin, std::uint32_t bw, std::uint8_t sfMin) const noexcept;
            void         QuantizeX34() noexcept;
            void         SetSubblockGain(SfbValues& sf) noexcept;
            void         SetScalefacs(const SfbValues& vbrsfmin, SfbValues& sf, const std::uint8_t* maxRange) noexcept;
            void         ShortBlockConstrain(const SfbValues& vbrsf, const SfbValues& vbrsfmin, std::int32_t vbrmax) noexcept;
            void         LongBlockConstrain(const SfbValues& vbrsf, const SfbValues& vbrsfmin, std::int32_t vbrmax) noexcept;

            const SessionConfig&        config_;
            const QuantizeConstants&    k_;
            GrInfo&                     gi_;
            const float*                xr34_;
            bool                        short_;
            bool                        guess_;
            bool                        failed_   = false;
            std::int32_t                mingainL_ = 0;
            std::array<std::int32_t, 3> mingainS_ = {0, 0, 0};
        };

        // the smallest step that keeps the largest value of the band quantizable (binary search)
        std::uint8_t Granule::FindLowestScalefac(float xr34) const noexcept
        {
            std::uint8_t sfOk  = 255;
            std::uint8_t sf    = 128;
            std::uint8_t delta = 64;
            const float  limit = IXMAX_VAL;
            for (std::int32_t i = 0; i < 8; ++i) {
                const float xfsf = k_.ipow20[sf] * xr34;
                if (xfsf <= limit) {
                    sfOk = sf;
                    sf   = static_cast<std::uint8_t>(sf - delta);
                } else {
                    sf = static_cast<std::uint8_t>(sf + delta);
                }
                delta >>= 1;
            }
            return sfOk;
        }

        // calc_sfb_noise_x34: quantization noise of a band at step sf (SfbNoiseScalar and its
        // SIMD versions); only for sf where the band stays quantizable
        float Granule::SfbNoise(const float* xr, const float* xr34, std::uint32_t bw, std::uint8_t sf) const noexcept
        {
            return k_.kernels.sfbNoise(xr, xr34, static_cast<std::int32_t>(bw), k_.pow20[sf + Q_MAX2], k_.ipow20[sf], k_.pow43.data(), k_.adj43.data());
        }

        // tri_calc_sfb_noise_x34: is the noise at sf or one of its neighbours above xmin?
        bool Granule::NoiseAbove(const float* xr, const float* xr34, float xmin, std::uint32_t bw, std::uint8_t sf, NoiseCache* cache) const noexcept
        {
            auto noise = [&](std::uint8_t s) {
                if (cache[s].valid == 0) {
                    cache[s].valid = 1;
                    cache[s].value = SfbNoise(xr, xr34, bw, s);
                }
                return cache[s].value;
            };
            if (xmin < noise(sf))
                return true;
            if (sf < 255 && xmin < noise(static_cast<std::uint8_t>(sf + 1)))
                return true;
            if (sf > 0 && xmin < noise(static_cast<std::uint8_t>(sf - 1)))
                return true;
            return false;
        }

        // find_scalefac_x34: the largest step whose noise (and that of its neighbours) stays
        // within xmin, by binary search
        std::uint8_t Granule::FindScalefac(const float* xr, const float* xr34, float xmin, std::uint32_t bw, std::uint8_t sfMin) const noexcept
        {
            std::array<NoiseCache, 256> cache;
            std::memset(cache.data(), 0, sizeof(cache));
            std::uint8_t sf       = 128;
            std::uint8_t sfOk     = 255;
            std::uint8_t delta    = 128;
            bool         seenGood = false;
            for (std::int32_t i = 0; i < 8; ++i) {
                delta >>= 1;
                if (sf <= sfMin) {
                    sf = static_cast<std::uint8_t>(sf + delta);
                } else if (NoiseAbove(xr, xr34, xmin, bw, sf, cache.data())) {
                    sf = static_cast<std::uint8_t>(sf - delta); // distortion: try a smaller step
                } else {
                    sfOk     = sf;
                    sf       = static_cast<std::uint8_t>(sf + delta);
                    seenGood = true;
                }
            }
            // a step without distortion, if there is one
            if (seenGood)
                sf = sfOk;
            if (sf <= sfMin)
                sf = sfMin;
            return sf;
        }

        // guess_scalefac_x34 with calc_scalefac: the step estimated from the allowed masking
        std::uint8_t Granule::GuessScalefac(float xmin, std::uint32_t bw, std::uint8_t sfMin) const noexcept
        {
            const float        c     = static_cast<float>(5.799142446); // 10 * 10^(2/3) * log10(4/3)
            const std::int32_t guess = 210 + static_cast<std::int32_t>(c * log10f(xmin / static_cast<std::int32_t>(bw)) - .5f);
            if (guess < sfMin)
                return sfMin;
            if (guess >= 255)
                return 255;
            return static_cast<std::uint8_t>(guess);
        }

        // block_sf: the step for each band (vbrsf), the smallest possible step (vbrsfmin) and the
        // largest step of the granule
        std::int32_t Granule::BlockSf(const std::array<float, SFBMAX>& xmin, SfbValues& vbrsf, SfbValues& vbrsfmin) noexcept
        {
            const float* const        xr          = gi_.xr.data();
            const std::int32_t* const width       = gi_.width.data();
            const std::uint32_t       maxNonzero  = static_cast<std::uint32_t>(gi_.max_nonzero_coeff);
            const std::int32_t        psymax      = gi_.psymax;
            std::uint8_t              maxsf       = 0;
            std::int32_t              sfb         = 0;
            std::int32_t              largestStep = -1; // m_o: the largest step below 255
            std::uint32_t             j           = 0;
            std::uint32_t             window      = 0;

            mingainL_    = 0;
            mingainS_[0] = 0;
            mingainS_[1] = 0;
            mingainS_[2] = 0;
            while (j <= maxNonzero) {
                const std::uint32_t w = static_cast<std::uint32_t>(width[sfb]);
                const std::uint32_t m = maxNonzero - j + 1;
                const std::uint32_t l = w > m ? m : w;

                const std::uint8_t lowest = FindLowestScalefac(k_.kernels.vecMax(&xr34_[j], static_cast<std::int32_t>(l)));
                vbrsfmin[sfb]             = lowest;
                if (mingainL_ < lowest)
                    mingainL_ = lowest;
                if (mingainS_[window] < lowest)
                    mingainS_[window] = lowest;
                if (++window > 2)
                    window = 0;

                std::uint8_t step;
                if (sfb < psymax && w > 2) {
                    if (gi_.energy_above_cutoff[sfb]) {
                        step = guess_ ? GuessScalefac(xmin[sfb], l, lowest) : FindScalefac(&xr[j], &xr34_[j], xmin[sfb], l, lowest);
                        if (maxsf < step)
                            maxsf = step;
                        if (largestStep < step && step < 255)
                            largestStep = step;
                    } else {
                        step  = 255;
                        maxsf = 255;
                    }
                } else {
                    if (maxsf < lowest)
                        maxsf = lowest;
                    step = maxsf;
                }
                vbrsf[sfb] = step;
                ++sfb;
                j += w;
            }
            for (; sfb < SFBMAX; ++sfb) {
                vbrsf[sfb]    = maxsf;
                vbrsfmin[sfb] = 0;
            }
            if (largestStep > -1) {
                maxsf = static_cast<std::uint8_t>(largestStep);
                for (sfb = 0; sfb < SFBMAX; ++sfb) {
                    if (vbrsf[sfb] == 255)
                        vbrsf[sfb] = largestStep;
                }
            }
            return maxsf;
        }

        // quantize_x34: quantizes the granule with its scalefactors
        void Granule::QuantizeX34() noexcept
        {
            const float*        xr34       = xr34_;
            const std::int32_t  ifqstep    = gi_.scalefac_scale == 0 ? 2 : 4;
            std::int32_t*       l3         = gi_.l3_enc.data();
            std::uint32_t       j          = 0;
            std::uint32_t       sfb        = 0;
            const std::uint32_t maxNonzero = static_cast<std::uint32_t>(gi_.max_nonzero_coeff);

            while (j <= maxNonzero) {
                const std::int32_t  s      = (gi_.scalefac[sfb] + (gi_.preflag ? pretab[sfb] : 0)) * ifqstep + gi_.subblock_gain[gi_.window[sfb]] * 8;
                const std::uint8_t  sfac   = static_cast<std::uint8_t>(gi_.global_gain - s);
                const float         step34 = k_.ipow20[sfac];
                const std::uint32_t w      = static_cast<std::uint32_t>(gi_.width[sfb]);
                const std::uint32_t m      = maxNonzero - j + 1;
                j += w;
                ++sfb;

                const std::uint32_t count = w <= m ? w : m;
                k_.kernels.quantizeLines(xr34, l3, static_cast<std::int32_t>(count), step34, k_.adj43.data());
                l3 += count;
                xr34 += count;
            }
        }

        // set_subblock_gain: subblock gains that let the short windows reach their steps with
        // the scalefactors; sf are the steps relative to the global gain
        void Granule::SetSubblockGain(SfbValues& sf) noexcept
        {
            const std::int32_t  maxrange1    = 15;
            const std::int32_t  maxrange2    = 7;
            const std::int32_t  ifqstepShift = gi_.scalefac_scale == 0 ? 1 : 2;
            std::int32_t* const sbg          = gi_.subblock_gain.data();
            const std::uint32_t psymax       = static_cast<std::uint32_t>(gi_.psymax);
            std::uint32_t       psydiv       = 18;
            std::int32_t        minSbg       = 7;

            if (psydiv > psymax)
                psydiv = psymax;
            for (std::uint32_t i = 0; i < 3; ++i) {
                std::int32_t  maxsf1 = 0, maxsf2 = 0, minsf = 1000;
                std::uint32_t sfb = i;
                for (; sfb < psydiv; sfb += 3) { // part 1: 4 bit scalefactors
                    const std::int32_t v = -sf[sfb];
                    if (maxsf1 < v)
                        maxsf1 = v;
                    if (minsf > v)
                        minsf = v;
                }
                for (; sfb < SFBMAX; sfb += 3) { // part 2: 3 bit scalefactors
                    const std::int32_t v = -sf[sfb];
                    if (maxsf2 < v)
                        maxsf2 = v;
                    if (minsf > v)
                        minsf = v;
                }

                // boost the subblock gain as little as possible so the scalefactors reach maxsf1
                {
                    const std::int32_t m1 = maxsf1 - (maxrange1 << ifqstepShift);
                    const std::int32_t m2 = maxsf2 - (maxrange2 << ifqstepShift);
                    maxsf1                = Max(m1, m2);
                }
                sbg[i] = minsf > 0 ? minsf >> 3 : 0;
                if (maxsf1 > 0)
                    sbg[i] = Max(sbg[i], (maxsf1 + 7) >> 3);
                if (sbg[i] > 0 && mingainS_[i] > (gi_.global_gain - sbg[i] * 8))
                    sbg[i] = (gi_.global_gain - mingainS_[i]) >> 3;
                if (sbg[i] > 7)
                    sbg[i] = 7;
                if (minSbg > sbg[i])
                    minSbg = sbg[i];
            }

            const std::int32_t sbg0 = sbg[0] * 8;
            const std::int32_t sbg1 = sbg[1] * 8;
            const std::int32_t sbg2 = sbg[2] * 8;
            for (std::uint32_t sfb = 0; sfb < SFBMAX; sfb += 3) {
                sf[sfb + 0] += sbg0;
                sf[sfb + 1] += sbg1;
                sf[sfb + 2] += sbg2;
            }
            // a gain common to all three windows goes into the global gain
            if (minSbg > 0) {
                for (std::int32_t i = 0; i < 3; ++i)
                    sbg[i] -= minSbg;
                gi_.global_gain -= minSbg * 8;
            }
        }

        // set_scalefacs: scalefactors from the steps relative to the global gain
        void Granule::SetScalefacs(const SfbValues& vbrsfmin, SfbValues& sf, const std::uint8_t* maxRange) noexcept
        {
            const std::int32_t        ifqstep      = gi_.scalefac_scale == 0 ? 2 : 4;
            const std::int32_t        ifqstepShift = gi_.scalefac_scale == 0 ? 1 : 2;
            std::int32_t* const       scalefac     = gi_.scalefac.data();
            const std::int32_t        sfbmax       = gi_.sfbmax;
            const std::int32_t* const sbg          = gi_.subblock_gain.data();
            const std::int32_t* const window       = gi_.window.data();
            const std::int32_t        preflag      = gi_.preflag;

            if (preflag) {
                for (std::int32_t sfb = 11; sfb < sfbmax; ++sfb)
                    sf[sfb] += pretab[sfb] * ifqstep;
            }
            std::int32_t sfb = 0;
            for (; sfb < sfbmax; ++sfb) {
                const std::int32_t gain = gi_.global_gain - (sbg[window[sfb]] * 8) - ((preflag ? pretab[sfb] : 0) * ifqstep);
                if (sf[sfb] < 0) {
                    const std::int32_t m = gain - vbrsfmin[sfb];
                    // ifqstep * scalefac >= -sf[sfb], so round up
                    scalefac[sfb] = (ifqstep - 1 - sf[sfb]) >> ifqstepShift;
                    if (scalefac[sfb] > maxRange[sfb])
                        scalefac[sfb] = maxRange[sfb];
                    if (scalefac[sfb] > 0 && (scalefac[sfb] << ifqstepShift) > m)
                        scalefac[sfb] = m >> ifqstepShift;
                } else {
                    scalefac[sfb] = 0;
                }
            }
            for (; sfb < SFBMAX; ++sfb)
                scalefac[sfb] = 0; // sfb21
        }

        // short_block_constrain: global gain, scalefac_scale, subblock gains and scalefactors
        // for a short block
        void Granule::ShortBlockConstrain(const SfbValues& vbrsf, const SfbValues& vbrsfmin, std::int32_t vbrmax) noexcept
        {
            const std::int32_t maxminsfb = mingainL_;
            std::int32_t       maxover0  = 0;
            std::int32_t       maxover1  = 0;
            std::int32_t       delta     = 0;
            const std::int32_t psymax    = gi_.psymax;

            for (std::int32_t sfb = 0; sfb < psymax; ++sfb) {
                const std::int32_t v = vbrmax - vbrsf[sfb];
                if (delta < v)
                    delta = v;
                const std::int32_t v0 = v - (4 * 14 + 2 * max_range_short[sfb]);
                const std::int32_t v1 = v - (4 * 14 + 4 * max_range_short[sfb]);
                if (maxover0 < v0)
                    maxover0 = v0;
                if (maxover1 < v1)
                    maxover1 = v1;
            }
            const std::int32_t mover = config_.noise_shaping == 2 ? Min(maxover0, maxover1) : maxover0; // 2 allows scalefac_scale = 1
            if (delta > mover)
                delta = mover;
            vbrmax -= delta;
            maxover0 -= mover;
            maxover1 -= mover;

            if (maxover0 == 0)
                gi_.scalefac_scale = 0;
            else if (maxover1 == 0)
                gi_.scalefac_scale = 1;
            if (vbrmax < maxminsfb)
                vbrmax = maxminsfb;
            gi_.global_gain = vbrmax;
            if (gi_.global_gain < 0)
                gi_.global_gain = 0;
            else if (gi_.global_gain > 255)
                gi_.global_gain = 255;

            std::array<std::int32_t, SFBMAX> sfTemp;
            for (std::int32_t sfb = 0; sfb < SFBMAX; ++sfb)
                sfTemp[sfb] = vbrsf[sfb] - vbrmax;
            SetSubblockGain(sfTemp);
            SetScalefacs(vbrsfmin, sfTemp, max_range_short.data());
        }

        // long_block_constrain: global gain, scalefac_scale, preflag and scalefactors for a long
        // block
        void Granule::LongBlockConstrain(const SfbValues& vbrsf, const SfbValues& vbrsfmin, std::int32_t vbrmax) noexcept
        {
            const std::uint8_t* maxRangeP = max_range_long.data();
            const std::int32_t  maxminsfb = mingainL_;
            const std::int32_t  psymax    = gi_.psymax;
            std::int32_t        maxover0 = 0, maxover1 = 0;
            std::int32_t        maxover0p = 0, maxover1p = 0; // with pretab
            std::int32_t        delta = 0;
            std::int32_t        vm0p = 1, vm1p = 1;

            for (std::int32_t sfb = 0; sfb < psymax; ++sfb) {
                const std::int32_t v = vbrmax - vbrsf[sfb];
                if (delta < v)
                    delta = v;
                const std::int32_t v0  = v - 2 * max_range_long[sfb];
                const std::int32_t v1  = v - 4 * max_range_long[sfb];
                const std::int32_t v0p = v - 2 * (maxRangeP[sfb] + pretab[sfb]);
                const std::int32_t v1p = v - 4 * (maxRangeP[sfb] + pretab[sfb]);
                if (maxover0 < v0)
                    maxover0 = v0;
                if (maxover1 < v1)
                    maxover1 = v1;
                if (maxover0p < v0p)
                    maxover0p = v0p;
                if (maxover1p < v1p)
                    maxover1p = v1p;
            }
            if (vm0p == 1) {
                std::int32_t gain = vbrmax - maxover0p;
                if (gain < maxminsfb)
                    gain = maxminsfb;
                for (std::int32_t sfb = 0; sfb < psymax; ++sfb) {
                    if ((gain - vbrsfmin[sfb]) - 2 * pretab[sfb] <= 0) {
                        vm0p = 0;
                        vm1p = 0;
                        break;
                    }
                }
            }
            if (vm1p == 1) {
                std::int32_t gain = vbrmax - maxover1p;
                if (gain < maxminsfb)
                    gain = maxminsfb;
                for (std::int32_t sfb = 0; sfb < psymax; ++sfb) {
                    if ((gain - vbrsfmin[sfb]) - 4 * pretab[sfb] <= 0) {
                        vm1p = 0;
                        break;
                    }
                }
            }
            if (vm0p == 0)
                maxover0p = maxover0;
            if (vm1p == 0)
                maxover1p = maxover1;
            if (config_.noise_shaping != 2) {
                maxover1  = maxover0;
                maxover1p = maxover0p;
            }
            std::int32_t mover = Min(maxover0, maxover0p);
            mover              = Min(mover, maxover1);
            mover              = Min(mover, maxover1p);

            if (delta > mover)
                delta = mover;
            vbrmax -= delta;
            if (vbrmax < maxminsfb)
                vbrmax = maxminsfb;
            maxover0 -= mover;
            maxover0p -= mover;
            maxover1 -= mover;
            maxover1p -= mover;

            if (maxover0 == 0) {
                gi_.scalefac_scale = 0;
                gi_.preflag        = 0;
                maxRangeP          = max_range_long.data();
            } else if (maxover0p == 0) {
                gi_.scalefac_scale = 0;
                gi_.preflag        = 1;
            } else if (maxover1 == 0) {
                gi_.scalefac_scale = 1;
                gi_.preflag        = 0;
                maxRangeP          = max_range_long.data();
            } else if (maxover1p == 0) {
                gi_.scalefac_scale = 1;
                gi_.preflag        = 1;
            }
            gi_.global_gain = vbrmax;
            if (gi_.global_gain < 0)
                gi_.global_gain = 0;
            else if (gi_.global_gain > 255)
                gi_.global_gain = 255;

            std::array<std::int32_t, SFBMAX> sfTemp;
            for (std::int32_t sfb = 0; sfb < SFBMAX; ++sfb)
                sfTemp[sfb] = vbrsf[sfb] - vbrmax;
            SetScalefacs(vbrsfmin, sfTemp, maxRangeP);
        }

        // LAME: quantizeAndCountBits() (vbrquantize.c)
        std::int32_t Granule::QuantizeAndCountBits() noexcept
        {
            QuantizeX34();
            gi_.part2_3_length = CountBits(config_, k_, gi_);
            return gi_.part2_3_length;
        }

        // tryGlobalStepsize: all steps moved by delta, within their limits
        std::int32_t Granule::TryGlobalStepsize(const SfbValues& sfwork, const SfbValues& vbrsfmin, std::int32_t delta) noexcept
        {
            const float                      xrpowMax = gi_.xrpow_max;
            std::array<std::int32_t, SFBMAX> sftemp;
            std::int32_t                     vbrmax = 0;
            for (std::int32_t i = 0; i < SFBMAX; ++i) {
                std::int32_t gain = sfwork[i] + delta;
                if (gain < vbrsfmin[i])
                    gain = vbrsfmin[i];
                if (gain > 255)
                    gain = 255;
                if (vbrmax < gain)
                    vbrmax = gain;
                sftemp[i] = gain;
            }
            Constrain(sftemp, vbrsfmin, vbrmax);
            Bitcount();
            const std::int32_t bits = QuantizeAndCountBits();
            gi_.xrpow_max           = xrpowMax;
            return bits;
        }

        // searchGlobalStepsizeMax: the smallest global step whose bits stay below target
        void Granule::SearchGlobalStepsizeMax(const SfbValues& sfwork, const SfbValues& vbrsfmin, std::int32_t target) noexcept
        {
            const std::int32_t gain   = gi_.global_gain;
            std::int32_t       curr   = gain;
            std::int32_t       gainOk = 1024;
            std::int32_t       l      = gain;
            std::int32_t       r      = 512;

            while (l <= r) {
                curr                    = (l + r) >> 1;
                const std::int32_t bits = TryGlobalStepsize(sfwork, vbrsfmin, curr - gain);
                if (bits == 0 || (bits + gi_.part2_length) < target) {
                    r      = curr - 1;
                    gainOk = curr;
                } else {
                    l = curr + 1;
                    if (gainOk == 1024)
                        gainOk = curr;
                }
            }
            if (gainOk != curr) {
                curr = gainOk;
                (void)TryGlobalStepsize(sfwork, vbrsfmin, curr - gain);
            }
        }

        // LAME: tryThatOne() (vbrquantize.c)
        std::int32_t Granule::TryThatOne(const SfbValues& sftemp, const SfbValues& vbrsfmin, std::int32_t vbrmax) noexcept
        {
            const float xrpowMax = gi_.xrpow_max;
            Constrain(sftemp, vbrsfmin, vbrmax);
            Bitcount();
            std::int32_t bits = QuantizeAndCountBits();
            bits += gi_.part2_length;
            gi_.xrpow_max = xrpowMax;
            return bits;
        }

        // sfDepth: how far the steps reach below 255
        std::int32_t SfDepth(const SfbValues& sfwork) noexcept
        {
            std::int32_t m = 0;
            for (std::int32_t i = 0; i < SFBMAX; ++i) {
                const std::int32_t di = 255 - sfwork[i];
                if (m < di)
                    m = di;
            }
            return m;
        }

        // cutDistribution: no step above cut
        void CutDistribution(const SfbValues& sfwork, SfbValues& sfOut, std::int32_t cut) noexcept
        {
            for (std::int32_t i = 0; i < SFBMAX; ++i) {
                const std::int32_t x = sfwork[i];
                sfOut[i]             = x < cut ? x : cut;
            }
        }

        // flattenDistribution: moves every step k/dm of the way towards p; returns the largest
        std::int32_t FlattenDistribution(const SfbValues& sfwork, SfbValues& sfOut, std::int32_t dm, std::int32_t k, std::int32_t p) noexcept
        {
            std::int32_t sfmax = 0;
            for (std::int32_t i = 0; i < SFBMAX; ++i) {
                std::int32_t x = sfwork[i];
                if (dm > 0) {
                    x += (k * (p - sfwork[i])) / dm;
                    if (x < 0)
                        x = 0;
                    else if (x > 255)
                        x = 255;
                }
                sfOut[i] = x;
                if (sfmax < x)
                    sfmax = x;
            }
            return sfmax;
        }

        // outOfBitsStrategy: flattens the step distribution (part 1), then raises the target
        // step (part 2) by binary search until the granule fits into target bits
        void Granule::OutOfBitsStrategy(const SfbValues& sfwork, const SfbValues& vbrsfmin, std::int32_t target) noexcept
        {
            std::array<std::int32_t, SFBMAX> wrk;
            const std::int32_t               dm = SfDepth(sfwork);
            const std::int32_t               p  = gi_.global_gain;

            // part 1
            {
                std::int32_t bi   = dm / 2;
                std::int32_t biOk = -1;
                std::int32_t bu   = 0;
                std::int32_t bo   = dm;
                for (;;) {
                    const std::int32_t sfmax = FlattenDistribution(sfwork, wrk, dm, bi, p);
                    if (TryThatOne(wrk, vbrsfmin, sfmax) <= target) {
                        biOk = bi;
                        bo   = bi - 1;
                    } else {
                        bu = bi + 1;
                    }
                    if (bu <= bo)
                        bi = (bu + bo) / 2;
                    else
                        break;
                }
                if (biOk >= 0) {
                    if (bi != biOk) {
                        const std::int32_t sfmax = FlattenDistribution(sfwork, wrk, dm, biOk, p);
                        (void)TryThatOne(wrk, vbrsfmin, sfmax);
                    }
                    return;
                }
            }

            // part 2
            {
                std::int32_t bi   = (255 + p) / 2;
                std::int32_t biOk = -1;
                std::int32_t bu   = p;
                std::int32_t bo   = 255;
                for (;;) {
                    const std::int32_t sfmax = FlattenDistribution(sfwork, wrk, dm, dm, bi);
                    if (TryThatOne(wrk, vbrsfmin, sfmax) <= target) {
                        biOk = bi;
                        bo   = bi - 1;
                    } else {
                        bu = bi + 1;
                    }
                    if (bu <= bo)
                        bi = (bu + bo) / 2;
                    else
                        break;
                }
                if (biOk >= 0) {
                    if (bi != biOk) {
                        const std::int32_t sfmax = FlattenDistribution(sfwork, wrk, dm, dm, biOk);
                        (void)TryThatOne(wrk, vbrsfmin, sfmax);
                    }
                    return;
                }
            }

            // fall back to the old code, likely never called
            SearchGlobalStepsizeMax(wrk, vbrsfmin, target);
        }

        // reduce_bit_usage: cheaper scalefactor storage and Huffman split
        std::int32_t ReduceBitUsage(const SessionConfig& config, const QuantizeConstants& constants, SideInfo& side, std::int32_t gr, std::int32_t ch) noexcept
        {
            GrInfo& gi = side.tt[gr][ch];
            BestScalefacStore(gr, ch, side);
            if (config.use_best_huffman == 1)
                BestHuffmanDivide(config, constants, gi);
            return gi.part2_3_length + gi.part2_length;
        }

        // distributes total over the entries by weight (f[i] / s), truncating like LAME
        std::int32_t Share(std::int32_t total, float f, float s) noexcept
        {
            return s > 0 ? static_cast<std::int32_t>(total * f / s) : 0;
        }

        // moves bits from one channel to the other, so that neither gets more than 32 bits more
        // than it used ('slack' bits for granules: 125)
        void Rebalance(std::array<std::int32_t, 2>& maxBits, const std::array<std::int32_t, 2>& used, std::int32_t slack) noexcept
        {
            if (maxBits[0] > used[0] + slack) {
                maxBits[1] += maxBits[0];
                maxBits[1] -= used[0] + slack;
                maxBits[0] = used[0] + slack;
            }
            if (maxBits[1] > used[1] + slack) {
                maxBits[0] += maxBits[1];
                maxBits[0] -= used[1] + slack;
                maxBits[1] = used[1] + slack;
            }
        }

        // the algo_t of every granule and channel of a frame
        using GranuleGrid = std::array<std::array<Granule, 2>, 2>;

        GranuleGrid MakeGranules(const SessionConfig& config, const QuantizeConstants& constants, SideInfo& side,
                                 const GranuleChannel<std::array<float, 576>>& xr34orig) noexcept
        {
            return {{{{Granule(config, constants, side.tt[0][0], xr34orig[0][0].data()), Granule(config, constants, side.tt[0][1], xr34orig[0][1].data())}},
                     {{Granule(config, constants, side.tt[1][0], xr34orig[1][0].data()), Granule(config, constants, side.tt[1][1], xr34orig[1][1].data())}}}};
        }

        // the bit limits of VBR_encode_frame when the frame does not fit 'as is': per granule and
        // channel from what each used, scaled down (by sqrt of sqrt within a granule, by sqrt
        // between the granules) until granules and frame fit; the limits from on_pe (maxBits)
        // if that fails
        __forceinline void DistributeBudget(std::int32_t nch, const GranuleChannel<std::int32_t>& maxBits, std::int32_t maxNbitsFr,
                                            const GranuleChannel<std::int32_t>& useNbitsCh, const std::array<std::int32_t, 2>& useNbitsGr,
                                            GranuleChannel<std::int32_t>& maxNbitsCh) noexcept
        {
            const std::int32_t          ngr        = 2;
            std::array<std::int32_t, 2> maxNbitsGr = {0, 0};
            bool                        ok         = true;
            std::int32_t                sumFr      = 0;
            for (std::int32_t gr = 0; gr < ngr; ++gr) {
                maxNbitsGr[gr] = 0;
                for (std::int32_t ch = 0; ch < nch; ++ch) {
                    maxNbitsCh[gr][ch] = useNbitsCh[gr][ch] > MAX_BITS_PER_CHANNEL ? MAX_BITS_PER_CHANNEL : useNbitsCh[gr][ch];
                    maxNbitsGr[gr] += maxNbitsCh[gr][ch];
                }
                if (maxNbitsGr[gr] > MAX_BITS_PER_GRANULE) {
                    std::array<float, 2> f = {0.0f, 0.0f};
                    float                s = 0.0f;
                    for (std::int32_t ch = 0; ch < nch; ++ch) {
                        if (maxNbitsCh[gr][ch] > 0) {
                            f[ch] = static_cast<float>(std::sqrt(std::sqrt(static_cast<double>(maxNbitsCh[gr][ch]))));
                            s += f[ch];
                        } else {
                            f[ch] = 0;
                        }
                    }
                    for (std::int32_t ch = 0; ch < nch; ++ch)
                        maxNbitsCh[gr][ch] = Share(MAX_BITS_PER_GRANULE, f[ch], s);
                    if (nch > 1) {
                        Rebalance(maxNbitsCh[gr], useNbitsCh[gr], 32);
                        if (maxNbitsCh[gr][0] > MAX_BITS_PER_CHANNEL)
                            maxNbitsCh[gr][0] = MAX_BITS_PER_CHANNEL;
                        if (maxNbitsCh[gr][1] > MAX_BITS_PER_CHANNEL)
                            maxNbitsCh[gr][1] = MAX_BITS_PER_CHANNEL;
                    }
                    maxNbitsGr[gr] = 0;
                    for (std::int32_t ch = 0; ch < nch; ++ch)
                        maxNbitsGr[gr] += maxNbitsCh[gr][ch];
                }
                sumFr += maxNbitsGr[gr];
            }
            if (sumFr > maxNbitsFr) {
                {
                    std::array<float, 2> f = {0.0f, 0.0f};
                    float                s = 0.0f;
                    for (std::int32_t gr = 0; gr < ngr; ++gr) {
                        if (maxNbitsGr[gr] > 0) {
                            f[gr] = static_cast<float>(std::sqrt(static_cast<double>(maxNbitsGr[gr])));
                            s += f[gr];
                        } else {
                            f[gr] = 0;
                        }
                    }
                    for (std::int32_t gr = 0; gr < ngr; ++gr)
                        maxNbitsGr[gr] = Share(maxNbitsFr, f[gr], s);
                }
                if (ngr > 1) {
                    Rebalance(maxNbitsGr, useNbitsGr, 125);
                    for (std::int32_t gr = 0; gr < ngr; ++gr) {
                        if (maxNbitsGr[gr] > MAX_BITS_PER_GRANULE)
                            maxNbitsGr[gr] = MAX_BITS_PER_GRANULE;
                    }
                }
                for (std::int32_t gr = 0; gr < ngr; ++gr) {
                    std::array<float, 2> f = {0.0f, 0.0f};
                    float                s = 0.0f;
                    for (std::int32_t ch = 0; ch < nch; ++ch) {
                        if (maxNbitsCh[gr][ch] > 0) {
                            f[ch] = static_cast<float>(std::sqrt(static_cast<double>(maxNbitsCh[gr][ch])));
                            s += f[ch];
                        } else {
                            f[ch] = 0;
                        }
                    }
                    for (std::int32_t ch = 0; ch < nch; ++ch)
                        maxNbitsCh[gr][ch] = Share(maxNbitsGr[gr], f[ch], s);
                    if (nch > 1) {
                        Rebalance(maxNbitsCh[gr], useNbitsCh[gr], 32);
                        for (std::int32_t ch = 0; ch < nch; ++ch) {
                            if (maxNbitsCh[gr][ch] > MAX_BITS_PER_CHANNEL)
                                maxNbitsCh[gr][ch] = MAX_BITS_PER_CHANNEL;
                        }
                    }
                }
            }

            // sanity check
            sumFr = 0;
            for (std::int32_t gr = 0; gr < ngr; ++gr) {
                std::int32_t sumGr = 0;
                for (std::int32_t ch = 0; ch < nch; ++ch) {
                    sumGr += maxNbitsCh[gr][ch];
                    if (maxNbitsCh[gr][ch] > MAX_BITS_PER_CHANNEL)
                        ok = false;
                }
                sumFr += sumGr;
                if (sumGr > MAX_BITS_PER_GRANULE)
                    ok = false;
            }
            if (sumFr > maxNbitsFr)
                ok = false;
            if (!ok) {
                // something went wrong: fall back to the limits from on_pe
                for (std::int32_t gr = 0; gr < ngr; ++gr)
                    for (std::int32_t ch = 0; ch < nch; ++ch)
                        maxNbitsCh[gr][ch] = maxBits[gr][ch];
            }
        }

        // the second loop of VBR_encode_frame: the out-of-bits strategy for every granule that
        // exceeds its limit, then reduce_bit_usage. Returns the bits of the frame, or -1 if a
        // granule's scalefactors could not be stored
        __forceinline std::int32_t FitGranules(const SessionConfig& config, const QuantizeConstants& constants, SideInfo& side, GranuleGrid& granules,
                                               GranuleChannel<GranuleSearch>& search, const GranuleChannel<std::int32_t>& maxBits,
                                               const GranuleChannel<std::int32_t>& maxNbitsCh, std::int32_t maxNbitsFr) noexcept
        {
            const std::int32_t ngr = 2;
            const std::int32_t nch = config.channels_out;

            // best_scalefac_store ran already; reset what it set before it runs again
            for (std::int32_t ch = 0; ch < nch; ++ch) {
                side.scfsi[ch][0] = 0;
                side.scfsi[ch][1] = 0;
                side.scfsi[ch][2] = 0;
                side.scfsi[ch][3] = 0;
            }
            for (std::int32_t gr = 0; gr < ngr; ++gr)
                for (std::int32_t ch = 0; ch < nch; ++ch)
                    side.tt[gr][ch].scalefac_compress = 0;

            // change the encoded data until it fits
            std::int32_t useNbitsFr = 0;
            for (std::int32_t gr = 0; gr < ngr; ++gr) {
                for (std::int32_t ch = 0; ch < nch; ++ch) {
                    if (maxBits[gr][ch] > 0) {
                        Granule&       g = granules[gr][ch];
                        GranuleSearch& s = search[gr][ch];
                        CutDistribution(s.sfwork, s.sfwork, g.Info().global_gain);
                        g.OutOfBitsStrategy(s.sfwork, s.vbrsfmin, maxNbitsCh[gr][ch]);
                    }
                    useNbitsFr += ReduceBitUsage(config, constants, side, gr, ch);
                }
            }

            for (std::int32_t gr = 0; gr < ngr; ++gr)
                for (std::int32_t ch = 0; ch < nch; ++ch)
                    if (granules[gr][ch].Failed())
                        return -1;

            // should always be within the limits, unless there are bugs
            if (useNbitsFr <= maxNbitsFr)
                return useNbitsFr;
            return -1;
        }

    } // namespace

    namespace {

        // k_34_4: quantize four values: truncate, add the rounding adjustment, truncate again
        void QuantizeQuad(std::array<float, 4>& x, std::int32_t* l3, const float* adj43) noexcept
        {
            for (std::int32_t i = 0; i < 4; ++i)
                l3[i] = static_cast<std::int32_t>(x[i]);
            for (std::int32_t i = 0; i < 4; ++i)
                x[i] += adj43[l3[i]];
            for (std::int32_t i = 0; i < 4; ++i)
                l3[i] = static_cast<std::int32_t>(x[i]);
        }

    } // namespace

    // the band loop of quantize_x34: k_34_4 on each quad, the last lines as a quad padded with zeros
    void QuantizeLinesScalar(const float* xr34, std::int32_t* l3, std::int32_t count, float step34, const float* adj43) noexcept
    {
        std::array<float, 4> x;
        const std::uint32_t  rest  = static_cast<std::uint32_t>(count) & 0x03u;
        std::uint32_t        quads = static_cast<std::uint32_t>(count) >> 2u;
        while (quads-- > 0) {
            for (std::int32_t i = 0; i < 4; ++i)
                x[i] = step34 * xr34[i];
            QuantizeQuad(x, l3, adj43);
            l3 += 4;
            xr34 += 4;
        }
        if (rest) {
            std::array<std::int32_t, 4> quantized;
            x[0] = x[1] = x[2] = x[3] = 0;
            for (std::uint32_t i = 0; i < rest; ++i)
                x[i] = step34 * xr34[i];
            QuantizeQuad(x, quantized.data(), adj43);
            for (std::uint32_t i = 0; i < rest; ++i)
                l3[i] = quantized[i];
        }
    }

    // calc_sfb_noise_x34: step = 2^(sf/4), step34 = step^(-3/4)
    float SfbNoiseScalar(const float* xr, const float* xr34, std::int32_t bw, float step, float step34, const float* pow43, const float* adj43) noexcept
    {
        float                       noise = 0;
        std::uint32_t               quads = static_cast<std::uint32_t>(bw) >> 2u;
        const std::uint32_t         rest  = static_cast<std::uint32_t>(bw) & 0x03u;
        std::array<float, 4>        x;
        std::array<std::int32_t, 4> l3;

        while (quads-- > 0) {
            for (std::int32_t i = 0; i < 4; ++i)
                x[i] = step34 * xr34[i];
            QuantizeQuad(x, l3.data(), adj43);
            for (std::int32_t i = 0; i < 4; ++i)
                x[i] = std::fabs(xr[i]) - step * pow43[l3[i]];
            noise += (x[0] * x[0] + x[1] * x[1]) + (x[2] * x[2] + x[3] * x[3]);
            xr += 4;
            xr34 += 4;
        }
        if (rest) {
            x[0] = x[1] = x[2] = x[3] = 0;
            for (std::uint32_t i = 0; i < rest; ++i)
                x[i] = step34 * xr34[i];
            QuantizeQuad(x, l3.data(), adj43);
            x[0] = x[1] = x[2] = x[3] = 0;
            for (std::uint32_t i = 0; i < rest; ++i)
                x[i] = std::fabs(xr[i]) - step * pow43[l3[i]];
            noise += (x[0] * x[0] + x[1] * x[1]) + (x[2] * x[2] + x[3] * x[3]);
        }
        return noise;
    }

    // LAME: vec_max_c() (vbrquantize.c)
    float VecMaxScalar(const float* xr34, std::int32_t bw) noexcept
    {
        float max = 0;
        for (std::int32_t i = 0; i < bw; ++i) {
            if (max < xr34[i])
                max = xr34[i];
        }
        return max;
    }

    void VbrSearchFrame(const SessionConfig& config, const QuantizeConstants& constants, SideInfo& side, const GranuleChannel<std::array<float, 576>>& xr34orig,
                        const GranuleChannel<std::array<float, SFBMAX>>& xmin, GranuleChannel<GranuleSearch>& search) noexcept
    {
        const std::int32_t ngr      = 2;
        const std::int32_t nch      = config.channels_out;
        GranuleGrid        granules = MakeGranules(config, constants, side, xr34orig);

        // search the scalefactors
        for (std::int32_t gr = 0; gr < ngr; ++gr) {
            for (std::int32_t ch = 0; ch < nch; ++ch) {
                GranuleSearch& s = search[gr][ch];
                if (s.active) {
                    Granule&           g      = granules[gr][ch];
                    const std::int32_t vbrmax = g.BlockSf(xmin[gr][ch], s.sfwork, s.vbrsfmin);
                    g.Constrain(s.sfwork, s.vbrsfmin, vbrmax);
                    g.Bitcount();
                }
                // else: no energy, l3_enc is all zero
            }
        }

        // encode 'as is'
        for (std::int32_t gr = 0; gr < ngr; ++gr) {
            for (std::int32_t ch = 0; ch < nch; ++ch) {
                GranuleSearch& s = search[gr][ch];
                if (s.active) {
                    side.tt[gr][ch].l3_enc.fill(0);
                    (void)granules[gr][ch].QuantizeAndCountBits();
                }
                s.usedBits = ReduceBitUsage(config, constants, side, gr, ch);
                granules[gr][ch].Save(s);
            }
        }
    }

    std::int32_t VbrFitFrame(const SessionConfig& config, const QuantizeConstants& constants, SideInfo& side,
                             const GranuleChannel<std::array<float, 576>>& xr34orig, const GranuleChannel<std::int32_t>& maxBits,
                             GranuleChannel<GranuleSearch>& search, bool& reworked) noexcept
    {
        reworked                                              = false;
        const std::int32_t                         ngr        = 2;
        const std::int32_t                         nch        = config.channels_out;
        std::array<std::array<std::int32_t, 2>, 2> maxNbitsCh = {};
        std::int32_t                               maxNbitsFr = 0;
        std::array<std::array<std::int32_t, 2>, 2> useNbitsCh
            = {{{MAX_BITS_PER_CHANNEL + 1, MAX_BITS_PER_CHANNEL + 1}, {MAX_BITS_PER_CHANNEL + 1, MAX_BITS_PER_CHANNEL + 1}}};
        std::array<std::int32_t, 2> useNbitsGr = {MAX_BITS_PER_GRANULE + 1, MAX_BITS_PER_GRANULE + 1};
        std::int32_t                useNbitsFr;
        GranuleGrid                 granules = MakeGranules(config, constants, side, xr34orig);

        for (std::int32_t gr = 0; gr < ngr; ++gr) {
            for (std::int32_t ch = 0; ch < nch; ++ch) {
                // the search ran where LAME's max_bits > 0; the two must agree
                if ((maxBits[gr][ch] > 0) != search[gr][ch].active)
                    return -1;
                granules[gr][ch].Restore(search[gr][ch]);
                maxNbitsCh[gr][ch] = maxBits[gr][ch];
                maxNbitsFr += maxBits[gr][ch];
            }
        }

        // the bits 'as is'
        useNbitsFr = 0;
        for (std::int32_t gr = 0; gr < ngr; ++gr) {
            useNbitsGr[gr] = 0;
            for (std::int32_t ch = 0; ch < nch; ++ch) {
                useNbitsCh[gr][ch] = search[gr][ch].usedBits;
                useNbitsGr[gr] += useNbitsCh[gr][ch];
            }
            useNbitsFr += useNbitsGr[gr];
        }

        // within all limits?
        if (useNbitsFr <= maxNbitsFr) {
            bool ok = true;
            for (std::int32_t gr = 0; gr < ngr; ++gr) {
                // every granule must stay within MAX_BITS_PER_GRANULE, every granule and channel
                // within MAX_BITS_PER_CHANNEL (part2_3_length has 12 bits)
                if (useNbitsGr[gr] > MAX_BITS_PER_GRANULE)
                    ok = false;
                for (std::int32_t ch = 0; ch < nch; ++ch) {
                    if (useNbitsCh[gr][ch] > MAX_BITS_PER_CHANNEL)
                        ok = false;
                }
            }
            if (ok) {
                for (std::int32_t gr = 0; gr < ngr; ++gr)
                    for (std::int32_t ch = 0; ch < nch; ++ch)
                        if (granules[gr][ch].Failed())
                            return -1;
                return useNbitsFr;
            }
        }

        // too many bits: decide how many each granule and channel may use
        reworked = true;
        DistributeBudget(nch, maxBits, maxNbitsFr, useNbitsCh, useNbitsGr, maxNbitsCh);
        return FitGranules(config, constants, side, granules, search, maxBits, maxNbitsCh, maxNbitsFr);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
