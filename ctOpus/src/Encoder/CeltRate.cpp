// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "CeltRate.h"

#include "CeltEnergy.h"
#include "IntMath.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    namespace {

        constexpr std::int32_t NbEBands   = CeltMode::NbEBands;
        constexpr std::int32_t FineOffset = 21;
        constexpr std::int32_t AllocSteps = 6;

        constexpr std::uint8_t Log2FracTable[24] = {0, 8, 13, 16, 19, 21, 23, 24, 26, 27, 28, 29, 30, 31, 32, 32, 33, 34, 34, 35, 36, 36, 37, 37};

        // libopus: celt_udiv() (celt/entcode.h)
        inline std::int32_t Udiv(std::int32_t n, std::int32_t d) noexcept
        {
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(n) / static_cast<std::uint32_t>(d));
        }

        // libopus: interp_bits2pulses() (celt/rate.c)
        std::int32_t InterpBits2Pulses(std::int32_t start, std::int32_t end, std::int32_t skipStart, const std::int32_t* bits1, const std::int32_t* bits2,
                                       const std::int32_t* thresh, const std::int32_t* cap, std::int32_t total, std::int32_t* balanceOut, std::int32_t skipRsv,
                                       std::int32_t* intensity, std::int32_t intensityRsv, std::int32_t* dualStereo, std::int32_t dualStereoRsv,
                                       std::int32_t* bits, std::int32_t* ebits, std::int32_t* finePriority, std::int32_t c, std::int32_t lm, RangeEncoder* ec,
                                       std::int32_t prev, std::int32_t signalBandwidth) noexcept
        {
            const std::int16_t* eBands     = CeltMode::EBands;
            const std::int32_t  allocFloor = c << BitRes;
            const std::int32_t  stereo     = c > 1 ? 1 : 0;
            const std::int32_t  logM       = lm << BitRes;

            std::int32_t lo = 0;
            std::int32_t hi = 1 << AllocSteps;
            for (std::int32_t i = 0; i < AllocSteps; ++i) {
                const std::int32_t mid  = (lo + hi) >> 1;
                std::int32_t       psum = 0;
                bool               done = false;
                for (std::int32_t j = end; j-- > start;) {
                    const std::int32_t tmp = bits1[j] + (mid * bits2[j] >> AllocSteps);
                    if (tmp >= thresh[j] || done) {
                        done = true;
                        // not more than can actually be used
                        psum += Imin(tmp, cap[j]);
                    } else if (tmp >= allocFloor) {
                        psum += allocFloor;
                    }
                }
                if (psum > total)
                    hi = mid;
                else
                    lo = mid;
            }
            std::int32_t psum = 0;
            bool         done = false;
            for (std::int32_t j = end; j-- > start;) {
                std::int32_t tmp = bits1[j] + (lo * bits2[j] >> AllocSteps);
                if (tmp < thresh[j] && !done)
                    tmp = tmp >= allocFloor ? allocFloor : 0;
                else
                    done = true;
                // not more than can actually be used
                tmp     = Imin(tmp, cap[j]);
                bits[j] = tmp;
                psum += tmp;
            }

            // the bands to skip, backwards from the end
            std::int32_t codedBands = end;
            for (;; --codedBands) {
                const std::int32_t j = codedBands - 1;
                // never skip the first band, nor a band boosted by dynalloc (a bit to signal
                // wasting all other bits, or to redistribute the bits just concentrated there)
                if (j <= skipStart) {
                    // the bit reserved to end skipping goes back
                    total += skipRsv;
                    break;
                }
                // the left-over bits this band would get, including bits stolen back from
                // higher, skipped bands
                std::int32_t       left     = total - psum;
                const std::int32_t percoeff = Udiv(left, eBands[codedBands] - eBands[start]);
                left -= (eBands[codedBands] - eBands[start]) * percoeff;
                const std::int32_t rem       = Imax(left - (eBands[j] - eBands[start]), 0);
                const std::int32_t bandWidth = eBands[codedBands] - eBands[j];
                std::int32_t       bandBits  = bits[j] + percoeff * bandWidth + rem;
                // a skip decision only above the threshold of the band (else force-skipped), so
                // there are enough bits for the skip flag
                if (bandBits >= Imax(thresh[j], allocFloor + (1 << BitRes))) {
                    // the only non-mandatory part of the bitstream: skipped bands are signalled.
                    // A threshold with hysteresis against bands fluctuating in and out, but not
                    // folding below a certain point.
                    const std::int32_t depthThreshold = codedBands > 17 ? (j < prev ? 7 : 9) : 0;
                    if (codedBands <= start + 2 || (bandBits > (depthThreshold * bandWidth << lm << BitRes) >> 4 && j <= signalBandwidth)) {
                        ec->EncodeBitLogp(1, 1);
                        break;
                    }
                    ec->EncodeBitLogp(0, 1);
                    // a bit used to skip this band
                    psum += 1 << BitRes;
                    bandBits -= 1 << BitRes;
                }
                // the bits originally allocated to this band come back
                psum -= bits[j] + intensityRsv;
                if (intensityRsv > 0)
                    intensityRsv = Log2FracTable[j - start];
                psum += intensityRsv;
                if (bandBits >= allocFloor) {
                    // enough for a fine energy bit per channel
                    psum += allocFloor;
                    bits[j] = allocFloor;
                } else {
                    // nothing at all
                    bits[j] = 0;
                }
            }

            // the intensity and dual stereo parameters
            if (intensityRsv > 0) {
                *intensity = Imin(*intensity, codedBands);
                ec->EncodeUint(static_cast<std::uint32_t>(*intensity - start), static_cast<std::uint32_t>(codedBands + 1 - start));
            } else {
                *intensity = 0;
            }
            if (*intensity <= start) {
                total += dualStereoRsv;
                dualStereoRsv = 0;
            }
            if (dualStereoRsv > 0)
                ec->EncodeBitLogp(*dualStereo, 1);
            else
                *dualStereo = 0;

            // the remaining bits
            std::int32_t       left     = total - psum;
            const std::int32_t percoeff = Udiv(left, eBands[codedBands] - eBands[start]);
            left -= (eBands[codedBands] - eBands[start]) * percoeff;
            for (std::int32_t j = start; j < codedBands; ++j)
                bits[j] += percoeff * (eBands[j + 1] - eBands[j]);
            for (std::int32_t j = start; j < codedBands; ++j) {
                const std::int32_t tmp = Imin(left, eBands[j + 1] - eBands[j]);
                bits[j] += tmp;
                left -= tmp;
            }

            std::int32_t balance = 0;
            std::int32_t j       = start;
            for (; j < codedBands; ++j) {
                const std::int32_t n0     = eBands[j + 1] - eBands[j];
                const std::int32_t n      = n0 << lm;
                const std::int32_t bit    = bits[j] + balance;
                std::int32_t       excess = 0;

                if (n > 1) {
                    excess  = Imax(bit - cap[j], 0);
                    bits[j] = bit - excess;

                    // the extra degree of freedom in stereo
                    const std::int32_t den    = c * n + ((c == 2 && n > 2 && *dualStereo == 0 && j < *intensity) ? 1 : 0);
                    const std::int32_t nClogN = den * (CeltMode::LogN[j] + logM);

                    // the fine bits offset by log2(N)/2 + FINE_OFFSET from their fair share
                    std::int32_t offset = (nClogN >> 1) - den * FineOffset;
                    // N=2 is the only point off the curve
                    if (n == 2)
                        offset += den << BitRes >> 2;
                    // the offset for the second and third fine energy bit
                    if (bits[j] + offset < den * 2 << BitRes)
                        offset += nClogN >> 2;
                    else if (bits[j] + offset < den * 3 << BitRes)
                        offset += nClogN >> 3;

                    // divide with rounding
                    ebits[j] = Imax(0, (bits[j] + offset + (den << (BitRes - 1))));
                    ebits[j] = static_cast<std::int32_t>(static_cast<std::uint32_t>(ebits[j]) / static_cast<std::uint32_t>(den) >> BitRes);

                    // no bust
                    if (c * ebits[j] > (bits[j] >> BitRes))
                        ebits[j] = bits[j] >> stereo >> BitRes;

                    // more is useless, about as far as PVQ goes
                    ebits[j] = Imin(ebits[j], MaxFineBits);

                    // rounded down or capped: a candidate for the final fine energy pass
                    finePriority[j] = ebits[j] * (den << BitRes) >= bits[j] + offset ? 1 : 0;

                    // the rest goes to PVQ
                    bits[j] -= c * ebits[j] << BitRes;
                } else {
                    // N=1: all bits to fine energy except a single sign bit
                    excess          = Imax(0, bit - (c << BitRes));
                    bits[j]         = bit - excess;
                    ebits[j]        = 0;
                    finePriority[j] = 1;
                }

                // fine energy cannot use the re-balancing of quant_all_bands(): done here
                if (excess > 0) {
                    const std::int32_t extraFine = Imin(excess >> (stereo + BitRes), MaxFineBits - ebits[j]);
                    ebits[j] += extraFine;
                    const std::int32_t extraBits = extraFine * c << BitRes;
                    finePriority[j]              = extraBits >= excess - balance ? 1 : 0;
                    excess -= extraBits;
                }
                balance = excess;
            }
            // the bits over the cap for the re-balancing in quant_all_bands()
            *balanceOut = balance;

            // skipped bands use all their bits for fine energy
            for (; j < end; ++j) {
                ebits[j]        = bits[j] >> stereo >> BitRes;
                bits[j]         = 0;
                finePriority[j] = ebits[j] < 1 ? 1 : 0;
            }
            return codedBands;
        }

    } // namespace

    // libopus: init_caps() (celt/celt.c)
    void InitCaps(std::int32_t* cap, std::int32_t lm, std::int32_t c) noexcept
    {
        for (std::int32_t i = 0; i < NbEBands; ++i) {
            const std::int32_t n = (CeltMode::EBands[i + 1] - CeltMode::EBands[i]) << lm;
            cap[i]               = (CeltMode::CacheCaps[NbEBands * (2 * lm + c - 1) + i] + 64) * c * n >> 2;
        }
    }

    // libopus: clt_compute_allocation() (celt/rate.c)
    std::int32_t ComputeAllocation(std::int32_t start, std::int32_t end, const std::int32_t* offsets, const std::int32_t* cap, std::int32_t allocTrim,
                                   std::int32_t* intensity, std::int32_t* dualStereo, std::int32_t total, std::int32_t* balance, std::int32_t* pulses,
                                   std::int32_t* ebits, std::int32_t* finePriority, std::int32_t c, std::int32_t lm, RangeEncoder* ec, std::int32_t prev,
                                   std::int32_t signalBandwidth) noexcept
    {
        const std::int16_t*    eBands = CeltMode::EBands;
        constexpr std::int32_t len    = NbEBands;
        std::int32_t           bits1[NbEBands];
        std::int32_t           bits2[NbEBands];
        std::int32_t           thresh[NbEBands];
        std::int32_t           trimOffset[NbEBands];

        total                  = Imax(total, 0);
        std::int32_t skipStart = start;
        // a bit to signal the end of manually skipped bands
        const std::int32_t skipRsv = total >= 1 << BitRes ? 1 << BitRes : 0;
        total -= skipRsv;
        // bits for the intensity and dual stereo parameters
        std::int32_t intensityRsv  = 0;
        std::int32_t dualStereoRsv = 0;
        if (c == 2) {
            intensityRsv = Log2FracTable[end - start];
            if (intensityRsv > total) {
                intensityRsv = 0;
            } else {
                total -= intensityRsv;
                dualStereoRsv = total >= 1 << BitRes ? 1 << BitRes : 0;
                total -= dualStereoRsv;
            }
        }

        for (std::int32_t j = start; j < end; ++j) {
            // below this threshold there are surely no PVQ bits
            thresh[j] = Imax(c << BitRes, (3 * (eBands[j + 1] - eBands[j]) << lm << BitRes) >> 4);
            // the tilt of the allocation curve
            trimOffset[j] = c * (eBands[j + 1] - eBands[j]) * (allocTrim - 5 - lm) * (end - j - 1) * (1 << (lm + BitRes)) >> 6;
            // less resolution for single-coefficient bands: one coarse value per coefficient helps them more
            if ((eBands[j + 1] - eBands[j]) << lm == 1)
                trimOffset[j] -= c << BitRes;
        }
        std::int32_t lo = 1;
        std::int32_t hi = CeltMode::NbAllocVectors - 1;
        do {
            bool               done = false;
            std::int32_t       psum = 0;
            const std::int32_t mid  = (lo + hi) >> 1;
            for (std::int32_t j = end; j-- > start;) {
                const std::int32_t n     = eBands[j + 1] - eBands[j];
                std::int32_t       bitsj = c * n * CeltMode::AllocVectors[mid * len + j] << lm >> 2;
                if (bitsj > 0)
                    bitsj = Imax(0, bitsj + trimOffset[j]);
                bitsj += offsets[j];
                if (bitsj >= thresh[j] || done) {
                    done = true;
                    // not more than can actually be used
                    psum += Imin(bitsj, cap[j]);
                } else if (bitsj >= c << BitRes) {
                    psum += c << BitRes;
                }
            }
            if (psum > total)
                hi = mid - 1;
            else
                lo = mid + 1;
        } while (lo <= hi);
        hi = lo--;
        for (std::int32_t j = start; j < end; ++j) {
            const std::int32_t n      = eBands[j + 1] - eBands[j];
            std::int32_t       bits1j = c * n * CeltMode::AllocVectors[lo * len + j] << lm >> 2;
            std::int32_t       bits2j = hi >= CeltMode::NbAllocVectors ? cap[j] : c * n * CeltMode::AllocVectors[hi * len + j] << lm >> 2;
            if (bits1j > 0)
                bits1j = Imax(0, bits1j + trimOffset[j]);
            if (bits2j > 0)
                bits2j = Imax(0, bits2j + trimOffset[j]);
            if (lo > 0)
                bits1j += offsets[j];
            bits2j += offsets[j];
            if (offsets[j] > 0)
                skipStart = j;
            bits2j   = Imax(0, bits2j - bits1j);
            bits1[j] = bits1j;
            bits2[j] = bits2j;
        }
        return InterpBits2Pulses(start, end, skipStart, bits1, bits2, thresh, cap, total, balance, skipRsv, intensity, intensityRsv, dualStereo, dualStereoRsv,
                                 pulses, ebits, finePriority, c, lm, ec, prev, signalBandwidth);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
