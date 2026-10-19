// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "Tokenize.h"

#include <cstddef>
#include <cstdint>
#include <new>

#include <immintrin.h>
#include <intrin.h>

// Port of the tokenizers of jpegli's entropy_coding.cc for progressive level 2 without restart
// markers: TokenizeProgressiveDC, TokenizeACProgressiveScan, TokenizeACRefinementScan,
// TokenizeScan, BuildHistograms. The row tokenizers also run on bands for BandStitch.cpp.

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    namespace {

        // ZRL: a run of 16 zero coefficients (run 15, size 0).
        constexpr std::uint8_t ZeroRunSymbol = 0xF0;

        // ------------------------------------------------------------------ block bit masks
        // New in ctJpeg, not in jpegli: bit k (zigzag position) of nonzero is (|coef| >> al) != 0,
        // of one (|coef| >> al) == 1. SSE2, so the tokenizers can jump from one significant
        // coefficient to the next; the run lengths are the distances between the bits.

        struct BlockMasks {
            std::uint64_t nonzero;
            std::uint64_t one;
        };

        BlockMasks ComputeMasks(const std::int16_t* block, std::int32_t al) noexcept
        {
            const __m128i shift = _mm_cvtsi32_si128(al);
            const __m128i zero  = _mm_setzero_si128();
            const __m128i ones  = _mm_set1_epi16(1);
            BlockMasks    m     = {0, 0};
            for (std::int32_t i = 0; i < 4; ++i) {
                __m128i a         = _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16 * i));
                __m128i b         = _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16 * i + 8));
                a                 = _mm_sra_epi16(_mm_max_epi16(a, _mm_sub_epi16(zero, a)), shift);
                b                 = _mm_sra_epi16(_mm_max_epi16(b, _mm_sub_epi16(zero, b)), shift);
                const auto isZero = static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_packs_epi16(_mm_cmpeq_epi16(a, zero), _mm_cmpeq_epi16(b, zero))));
                const auto isOne  = static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_packs_epi16(_mm_cmpeq_epi16(a, ones), _mm_cmpeq_epi16(b, ones))));
                m.nonzero |= static_cast<std::uint64_t>(~isZero & 0xFFFFu) << (16 * i);
                m.one |= static_cast<std::uint64_t>(isOne) << (16 * i);
            }
            return m;
        }

        // The zigzag positions [ss, se] of a scan as a bit mask.
        std::uint64_t RangeMask(std::int32_t ss, std::int32_t se) noexcept
        {
            const std::uint64_t upTo = se >= 63 ? ~0ull : ((1ull << (se + 1)) - 1);
            return upTo & ~((1ull << ss) - 1);
        }

        std::int32_t TrailingZeros(std::uint64_t v) noexcept
        {
            return static_cast<std::int32_t>(_tzcnt_u64(v));
        }

    } // namespace

    void DcScanMcus(const EncoderSetup& s, const ScanInfo& si, std::int32_t& mcusPerRow, std::int32_t& mcuRows) noexcept
    {
        if (si.componentCount == 1) {
            mcusPerRow = s.widthInBlocks[static_cast<std::size_t>(si.components[0])];
            mcuRows    = s.heightInBlocks[static_cast<std::size_t>(si.components[0])];
        } else {
            mcusPerRow = (s.frame.width + DctSize * s.maxH - 1) / (DctSize * s.maxH);
            mcuRows    = (s.frame.height + DctSize * s.maxV - 1) / (DctSize * s.maxV);
        }
    }

    bool TokenizeDcRows(const EncoderSetup& s, const Coefficients& coef, const ScanInfo& si, std::int32_t m0, std::int32_t m1, TokenPart& out) noexcept
    {
        const bool   interleaved = si.componentCount > 1;
        std::int32_t mcusPerRow, mcuRows;
        DcScanMcus(s, si, mcusPerRow, mcuRows);
        std::size_t blocksInMcu = 0;
        for (std::int32_t i = 0; i < si.componentCount; ++i) {
            const ComponentInfo& comp = s.frame.component[static_cast<std::size_t>(si.components[static_cast<std::size_t>(i)])];
            blocksInMcu += interleaved ? static_cast<std::size_t>(comp.hSamp * comp.vSamp) : 1;
        }
        if (!out.tokens.Allocate(static_cast<std::size_t>(mcusPerRow) * static_cast<std::size_t>(m1 - m0) * blocksInMcu + 1))
            return false;
        std::int16_t lastDc[MaxComponents] = {};
        if (m0 > 0) {
            for (std::int32_t i = 0; i < si.componentCount; ++i) {
                const std::int32_t   c    = si.components[static_cast<std::size_t>(i)];
                const ComponentInfo& comp = s.frame.component[static_cast<std::size_t>(c)];
                const std::int32_t   ny   = interleaved ? comp.vSamp : 1;
                const std::int32_t   nx   = interleaved ? comp.hSamp : 1;
                const std::int32_t   by   = m0 * ny - 1;
                const std::int32_t   bx   = mcusPerRow * nx - 1;
                const std::int16_t   dc   = bx >= s.widthInBlocks[static_cast<std::size_t>(c)] || by >= s.heightInBlocks[static_cast<std::size_t>(c)]
                    ? std::int16_t{0}
                    : coef.Dc(c, bx, by);
                lastDc[i]                 = static_cast<std::int16_t>(dc >> si.al);
            }
        }
        std::size_t n = 0;
        for (std::int32_t mcuY = m0; mcuY < m1; ++mcuY) {
            for (std::int32_t mcuX = 0; mcuX < mcusPerRow; ++mcuX) {
                for (std::int32_t i = 0; i < si.componentCount; ++i) {
                    const std::int32_t   c    = si.components[static_cast<std::size_t>(i)];
                    const ComponentInfo& comp = s.frame.component[static_cast<std::size_t>(c)];
                    const std::int32_t   ny   = interleaved ? comp.vSamp : 1;
                    const std::int32_t   nx   = interleaved ? comp.hSamp : 1;
                    for (std::int32_t iy = 0; iy < ny; ++iy) {
                        for (std::int32_t ix = 0; ix < nx; ++ix) {
                            const std::int32_t by = mcuY * ny + iy;
                            const std::int32_t bx = mcuX * nx + ix;
                            const std::int16_t dc = bx >= s.widthInBlocks[static_cast<std::size_t>(c)] || by >= s.heightInBlocks[static_cast<std::size_t>(c)]
                                ? std::int16_t{0}
                                : coef.Dc(c, bx, by);
                            // TokenizeProgressiveDC
                            std::int16_t temp2 = static_cast<std::int16_t>(dc >> si.al);
                            std::int16_t temp  = static_cast<std::int16_t>(temp2 - lastDc[i]);
                            lastDc[i]          = temp2;
                            temp2              = temp;
                            if (temp < 0) {
                                temp = static_cast<std::int16_t>(-temp);
                                temp2--;
                            }
                            const std::int32_t nbits = temp == 0 ? 0 : FloorLog2(static_cast<std::uint32_t>(temp)) + 1;
                            const std::int32_t bits  = temp2 & ((1 << nbits) - 1);
                            out.tokens[n++]          = {static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(nbits), static_cast<std::uint16_t>(bits)};
                        }
                    }
                }
            }
        }
        out.tokenCount = n;
        return true;
    }

    bool TokenizeAcFirstRows(const EncoderSetup& s, const Coefficients& coef, const ScanInfo& si, std::int32_t context, std::int32_t r0, std::int32_t r1,
                             TokenPart& out, BandResult* result) noexcept
    {
        const std::int32_t c = si.components[0];
        const std::int32_t w = s.widthInBlocks[static_cast<std::size_t>(c)];
        // reserved for the worst case, committed block by block as the tokens come
        const std::size_t blockTokens = static_cast<std::size_t>(si.se - si.ss + 2);
        if (!out.tokens.Reserve(blockTokens * static_cast<std::size_t>(w) * static_cast<std::size_t>(r1 - r0) + 1))
            return false;
        Token*              tokens  = out.tokens.Data();
        std::size_t         n       = 0;
        std::int32_t        eobRun  = 0;
        bool                active  = result == nullptr;
        std::int32_t        leading = 0;
        const std::uint64_t range   = RangeMask(si.ss, si.se);
        for (std::int32_t by = r0; by < r1; ++by) {
            for (std::int32_t bx = 0; bx < w; ++bx) {
                if (!out.tokens.Ensure(n + blockTokens + 1))
                    return false;
                const std::int16_t* block   = coef.Block(c, bx, by);
                std::uint64_t       nonzero = ComputeMasks(block, si.al).nonzero & range;
                if (!active) {
                    if (nonzero == 0) {
                        ++leading;
                        continue;
                    }
                    active = true;
                }
                std::int32_t prev = si.ss - 1; // the last significant position
                while (nonzero != 0) {
                    const std::int32_t k = TrailingZeros(nonzero);
                    nonzero &= nonzero - 1;
                    std::int32_t r    = k - prev - 1;
                    prev              = k;
                    std::int16_t temp = block[k];
                    std::int16_t temp2;
                    if (temp < 0) {
                        temp  = static_cast<std::int16_t>(-temp);
                        temp  = static_cast<std::int16_t>(temp >> si.al);
                        temp2 = static_cast<std::int16_t>(~temp);
                    } else {
                        temp  = static_cast<std::int16_t>(temp >> si.al);
                        temp2 = temp;
                    }
                    if (eobRun > 0) {
                        tokens[n++] = EobRunToken(context, eobRun);
                        eobRun      = 0;
                    }
                    while (r > 15) {
                        tokens[n++] = {static_cast<std::uint8_t>(context), ZeroRunSymbol, 0};
                        r -= 16;
                    }
                    const std::int32_t nbits  = FloorLog2(static_cast<std::uint32_t>(temp)) + 1;
                    const std::int32_t symbol = (r << 4) + nbits;
                    tokens[n++]
                        = {static_cast<std::uint8_t>(context), static_cast<std::uint8_t>(symbol), static_cast<std::uint16_t>(temp2 & ((1 << nbits) - 1))};
                }
                if (prev < si.se) { // zeros after the last significant coefficient
                    ++eobRun;
                    if (eobRun == MaxEobRun) {
                        tokens[n++] = EobRunToken(context, eobRun);
                        eobRun      = 0;
                    }
                }
            }
        }
        if (result == nullptr) {
            if (eobRun > 0)
                tokens[n++] = EobRunToken(context, eobRun);
        } else {
            result->active        = active;
            result->leadingBlocks = leading;
            result->exitEobRun    = eobRun;
            result->histogram     = {};
            for (std::size_t i = 0; i < n; ++i)
                ++result->histogram[tokens[i].symbol];
        }
        out.tokenCount = n;
        return true;
    }

    bool TokenizeAcRefinementRows(const EncoderSetup& s, const Coefficients& coef, const ScanInfo& si, std::int32_t r0, std::int32_t r1, TokenPart& out,
                                  BandResult* result) noexcept
    {
        const std::int32_t c         = si.components[0];
        const std::int32_t w         = s.widthInBlocks[static_cast<std::size_t>(c)];
        const std::size_t  numBlocks = static_cast<std::size_t>(w) * static_cast<std::size_t>(r1 - r0);
        const std::size_t  band      = static_cast<std::size_t>(si.se - si.ss + 1);
        // reserved for the worst case, committed block by block as the tokens come
        const std::size_t blockRefTokens = band + 1 + band / 16;
        if (!out.refTokens.Reserve(numBlocks * (band + 1 + band / 16) + 2) || !out.refBits.Reserve(numBlocks * band + 1)
            || !out.eobRuns.Reserve(numBlocks / 2 + 1))
            return false;
        if (result != nullptr && !result->leadingBits.Allocate(numBlocks))
            return false;
        RefToken*           tokens       = out.refTokens.Data();
        std::uint8_t*       refBits      = out.refBits.Data();
        std::uint16_t*      eobRuns      = out.eobRuns.Data();
        RefToken            token        = {};
        std::int32_t        eobRun       = 0;
        std::int32_t        eobRefbits   = 0;
        std::size_t         nextToken    = 0;
        std::size_t         nextEobToken = 0;
        std::size_t         nextRefBit   = 0;
        std::size_t         nextEobRun   = 0;
        bool                active       = result == nullptr;
        std::int32_t        leading      = 0;
        const std::uint64_t range        = RangeMask(si.ss, si.se);
        for (std::int32_t by = r0; by < r1; ++by) {
            for (std::int32_t bx = 0; bx < w; ++bx) {
                if (!out.refTokens.Ensure(nextToken + blockRefTokens + 2) || !out.refBits.Ensure(nextRefBit + band + 1) || !out.eobRuns.Ensure(nextEobRun + 2))
                    return false;
                const std::int16_t* block   = coef.Block(c, bx, by);
                const BlockMasks    masks   = ComputeMasks(block, si.al);
                std::uint64_t       nonzero = masks.nonzero & range;
                if (!active) {
                    if ((masks.one & range) == 0) { // an EOB block: only its correction bits
                        std::int32_t bits = 0;
                        for (std::uint64_t m = nonzero; m != 0; m &= m - 1) {
                            std::int32_t absval = block[TrailingZeros(m)];
                            if (absval < 0)
                                absval = -absval;
                            refBits[nextRefBit++] = static_cast<std::uint8_t>((absval >> si.al) & 1u);
                            ++bits;
                        }
                        result->leadingBits[static_cast<std::size_t>(leading++)] = static_cast<std::uint8_t>(bits);
                        continue;
                    }
                    active = true;
                }
                std::int32_t numEobRefinementBits = 0;
                std::int32_t numRefinementBits    = 0;
                std::int32_t r                    = 0;
                std::int32_t prev                 = si.ss - 1; // the last significant position
                while (nonzero != 0) {
                    const std::int32_t k = TrailingZeros(nonzero);
                    nonzero &= nonzero - 1;
                    r += k - prev - 1; // zeros count, correction-bit coefficients do not
                    prev                      = k;
                    std::int32_t       absval = block[k];
                    const std::int32_t mask   = absval >> (8 * sizeof(std::int32_t) - 1);
                    absval += mask;
                    absval ^= mask;
                    absval >>= si.al;
                    while (r > 15) {
                        token.symbol        = ZeroRunSymbol;
                        token.refbits       = static_cast<std::uint8_t>(numRefinementBits);
                        tokens[nextToken++] = token;
                        r -= 16;
                        numEobRefinementBits += numRefinementBits;
                        numRefinementBits = 0;
                    }
                    if (absval > 1) {
                        refBits[nextRefBit++] = static_cast<std::uint8_t>(absval & 1u);
                        ++numRefinementBits;
                        continue;
                    }
                    const std::int32_t symbol = (r << 4) + 1 + ((mask + 1) << 1);
                    token.symbol              = static_cast<std::uint8_t>(symbol);
                    token.refbits             = static_cast<std::uint8_t>(numRefinementBits);
                    tokens[nextToken++]       = token;
                    numRefinementBits         = 0;
                    numEobRefinementBits      = 0;
                    r                         = 0;
                    nextEobToken              = nextToken;
                    eobRun = eobRefbits = 0;
                }
                r += si.se - prev;
                if (r > 0 || numEobRefinementBits + numRefinementBits > 0) {
                    ++eobRun;
                    eobRefbits += numEobRefinementBits + numRefinementBits;
                    if (eobRefbits > MaxRefBits) {
                        ++nextEobToken;
                        eobRefbits = numEobRefinementBits + numRefinementBits;
                        eobRun     = 1;
                    }
                    nextToken                 = nextEobToken;
                    tokens[nextToken].refbits = static_cast<std::uint8_t>(eobRefbits);
                    if (eobRun == 1)
                        tokens[nextToken].symbol = 0;
                    else if (eobRun == 2) {
                        tokens[nextToken].symbol = 16;
                        eobRuns[nextEobRun++]    = 0;
                    } else if ((eobRun & (eobRun - 1)) == 0) {
                        tokens[nextToken].symbol = static_cast<std::uint8_t>(tokens[nextToken].symbol + 16);
                        eobRuns[nextEobRun - 1]  = 0;
                    } else
                        ++eobRuns[nextEobRun - 1];
                    ++nextToken;
                    if (eobRun == MaxEobRun) {
                        nextEobToken = nextToken;
                        eobRun = eobRefbits = 0;
                    }
                }
            }
        }
        out.refTokenCount = nextToken;
        out.refBitCount   = nextRefBit;
        out.eobRunCount   = nextEobRun;
        if (result != nullptr) {
            result->active         = active;
            result->leadingBlocks  = leading;
            result->exitEobRun     = eobRun;
            result->exitEobRefbits = eobRefbits;
            // an open EOB run ends in the band's last token; the stitch counts it once it is final
            const std::size_t counted = eobRun > 0 ? nextToken - 1 : nextToken;
            result->histogram         = {};
            for (std::size_t i = 0; i < counted; ++i)
                ++result->histogram[tokens[i].symbol & RefSymbolMask];
        }
        return true;
    }

    bool ScanTokens::AllocateParts(std::int32_t count) noexcept
    {
        parts.reset(new (std::nothrow) TokenPart[static_cast<std::size_t>(count)]);
        partCount = parts ? count : 0;
        return static_cast<bool>(parts);
    }

    bool TokenizeScan(const EncoderSetup& setup, const Coefficients& coefficients, std::int32_t scanIndex, ScanTokens& tokens) noexcept
    {
        const ScanInfo& si = setup.scans[static_cast<std::size_t>(scanIndex)];
        if (!tokens.AllocateParts(1))
            return false;
        TokenPart& part = tokens.parts[0];
        if (si.ss > 0) {
            const std::int32_t rows = setup.heightInBlocks[static_cast<std::size_t>(si.components[0])];
            if (si.ah == 0)
                return TokenizeAcFirstRows(setup, coefficients, si, setup.acContextOffset[static_cast<std::size_t>(scanIndex)], 0, rows, part, nullptr);
            return TokenizeAcRefinementRows(setup, coefficients, si, 0, rows, part, nullptr);
        }
        std::int32_t mcusPerRow, mcuRows;
        DcScanMcus(setup, si, mcusPerRow, mcuRows);
        return si.ah == 0 && TokenizeDcRows(setup, coefficients, si, 0, mcuRows, part); // no DC refinement in level 2
    }

    void AddScanHistograms(const EncoderSetup& setup, std::int32_t scanIndex, const ScanTokens& tokens, Histograms& histograms) noexcept
    {
        const ScanInfo& si = setup.scans[static_cast<std::size_t>(scanIndex)];
        for (std::int32_t p = 0; p < tokens.partCount; ++p) {
            const TokenPart& part = tokens.parts[p];
            if (si.ss > 0 && si.ah > 0) {
                auto& acHisto = histograms.count[static_cast<std::size_t>(setup.acContextOffset[static_cast<std::size_t>(scanIndex)])];
                for (std::size_t j = 0; j < part.refTokenCount; ++j)
                    ++acHisto[part.refTokens[j].symbol & RefSymbolMask];
            } else {
                for (std::size_t j = 0; j < part.tokenCount; ++j) {
                    const Token t = part.tokens[j];
                    ++histograms.count[t.context][t.symbol];
                }
            }
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
