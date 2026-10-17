// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "Entropy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

#include <immintrin.h>
#include <intrin.h>

#include "BitWriter.h"
#include "Markers.h"

// Port of jpegli's entropy coding for progressive level 2 without restart markers:
// entropy_coding.cc (TokenizeProgressiveDC, TokenizeACProgressiveScan, TokenizeACRefinementScan,
// TokenizeScan, BuildHistograms, HistogramCost, ClusterJpegHistograms, BuildJpegHuffmanTable,
// OptimizeHuffmanCodes, BuildHuffmanCodeTable), huffman.cc (CreateHuffmanTree) and bitstream.cc
// (WriteScanHeader, WriteTokens, WriteACRefinementTokens). Integer arithmetic keeps jpegli's
// coeff_t (int16) types; the clustering keeps its float costs.

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    namespace {

        std::int32_t FloorLog2(std::uint32_t v) noexcept
        {
            std::int32_t n = -1;
            while (v != 0) {
                v >>= 1;
                ++n;
            }
            return n;
        }

        // kNumExtraBits: run/size symbols carry 'size' extra bits, EOB runs (size 0) carry 'run'
        // bits, ZRL (0xF0) none.
        std::int32_t NumExtraBits(std::int32_t symbol) noexcept
        {
            const std::int32_t run = symbol >> 4, size = symbol & 15;
            return size != 0 ? size : (run == 15 ? 0 : run);
        }

        // ------------------------------------------------------------------ CreateHuffmanTree

        struct TreeNode {
            std::uint32_t totalCount;
            std::int16_t  indexLeft;
            std::int16_t  indexRightOrValue;
        };

        void SetDepth(const TreeNode& p, const TreeNode* pool, std::uint8_t* depth, std::uint8_t level) noexcept
        {
            if (p.indexLeft >= 0) {
                ++level;
                SetDepth(pool[p.indexLeft], pool, depth, level);
                SetDepth(pool[p.indexRightOrValue], pool, depth, level);
            } else
                depth[p.indexRightOrValue] = level;
        }

        // Least popular first, ties by decreasing symbol: a strict total order on leaves, so any
        // correct sort gives jpegli's (std::sort) order.
        bool TreeLess(const TreeNode& v0, const TreeNode& v1) noexcept
        {
            return v0.totalCount != v1.totalCount ? v0.totalCount < v1.totalCount : v0.indexRightOrValue > v1.indexRightOrValue;
        }

        constexpr std::size_t TreeSymbols = HuffmanSymbols + 1; // 256 symbols + the reserved one

        void CreateHuffmanTree(const std::uint32_t* data, std::size_t length, std::int32_t treeLimit, std::uint8_t* depth) noexcept
        {
            for (std::uint32_t countLimit = 1;; countLimit *= 2) {
                TreeNode    tree[2 * TreeSymbols + 1];
                std::size_t size = 0;
                for (std::size_t i = length; i != 0;) {
                    --i;
                    if (data[i]) {
                        const std::uint32_t count = data[i] < countLimit - 1 ? countLimit - 1 : data[i];
                        tree[size++]              = {count, -1, static_cast<std::int16_t>(i)};
                    }
                }
                const std::size_t n = size;
                if (n == 1) {
                    depth[tree[0].indexRightOrValue] = 1; // fake value, fixed on the upper level
                    break;
                }
                for (std::size_t a = 1; a < n; ++a) { // insertion sort
                    const TreeNode v = tree[a];
                    std::size_t    b = a;
                    for (; b > 0 && TreeLess(v, tree[b - 1]); --b)
                        tree[b] = tree[b - 1];
                    tree[b] = v;
                }
                const TreeNode sentinel = {0xFFFFFFFFu, -1, -1};
                tree[size++]            = sentinel;
                tree[size++]            = sentinel;
                std::size_t i           = 0;     // next leaf
                std::size_t j           = n + 1; // next inner node
                for (std::size_t k = n - 1; k != 0; --k) {
                    std::size_t left, right;
                    if (tree[i].totalCount <= tree[j].totalCount)
                        left = i++;
                    else
                        left = j++;
                    if (tree[i].totalCount <= tree[j].totalCount)
                        right = i++;
                    else
                        right = j++;
                    const std::size_t jEnd = size - 1;
                    tree[jEnd]   = {tree[left].totalCount + tree[right].totalCount, static_cast<std::int16_t>(left), static_cast<std::int16_t>(right)};
                    tree[size++] = sentinel;
                }
                SetDepth(tree[2 * n - 1], tree, depth, 0);
                std::uint8_t maxDepth = 0;
                for (std::size_t s = 0; s < length; ++s)
                    maxDepth = depth[s] > maxDepth ? depth[s] : maxDepth;
                if (maxDepth <= treeLimit)
                    break;
            }
        }

        // ------------------------------------------------------------------ clustering

        using Histogram = std::array<std::int32_t, HuffmanSymbols>;

        bool IsEmpty(const Histogram& h) noexcept
        {
            for (const std::int32_t c : h)
                if (c)
                    return false;
            return true;
        }

        float HistogramCost(const Histogram& histo) noexcept
        {
            std::uint32_t counts[TreeSymbols];
            std::uint8_t  depths[TreeSymbols] = {};
            for (std::size_t i = 0; i < HuffmanSymbols; ++i)
                counts[i] = static_cast<std::uint32_t>(histo[i]);
            counts[HuffmanSymbols] = 1;
            CreateHuffmanTree(counts, TreeSymbols, HuffmanMaxBits, depths);
            std::size_t headerBits = (1 + HuffmanMaxBits) * 8;
            std::size_t dataBits   = 0;
            for (std::size_t i = 0; i < HuffmanSymbols; ++i) {
                if (depths[i] > 0) {
                    headerBits += 8;
                    dataBits += static_cast<std::size_t>(counts[i]) * depths[i];
                }
            }
            return static_cast<float>(headerBits + dataBits); // jpegli returns size_t as float
        }

        struct Clusters {
            Histogram    histograms[MaxContexts];
            std::int32_t histogramCount = 0;
            std::int32_t histogramIndexes[MaxContexts]{};
            std::int32_t slotIds[MaxContexts]{};
            std::int32_t slotIdCount = 0;
        };

        void ClusterJpegHistograms(const Histogram* histograms, std::int32_t num, Clusters& clusters) noexcept
        {
            std::int32_t slotHistograms[4];
            float        slotCosts[4];
            std::int32_t slots = 0;
            for (std::int32_t i = 0; i < num; ++i) {
                const Histogram& cur = histograms[i];
                if (IsEmpty(cur))
                    continue;
                std::int32_t bestSlot = slots;
                float        bestCost = HistogramCost(cur); // progressive: no baseline limit
                for (std::int32_t j = 0; j < slots; ++j) {
                    const Histogram& prev = clusters.histograms[slotHistograms[j]];
                    Histogram        combined;
                    for (std::size_t s = 0; s < HuffmanSymbols; ++s)
                        combined[s] = prev[s] + cur[s];
                    const float combinedCost = HistogramCost(combined);
                    const float cost         = combinedCost - slotCosts[j];
                    if (cost < bestCost) {
                        bestCost = cost;
                        bestSlot = j;
                    }
                }
                if (bestSlot == slots) {
                    const std::int32_t histogramIndex              = clusters.histogramCount;
                    clusters.histograms[clusters.histogramCount++] = cur;
                    clusters.histogramIndexes[i]                   = histogramIndex;
                    if (bestSlot < 4) {
                        slotHistograms[slots] = histogramIndex;
                        slotCosts[slots]      = bestCost;
                        ++slots;
                    } else
                        bestSlot = (clusters.slotIds[clusters.slotIdCount - 1] + 1) % 4;
                    slotHistograms[bestSlot]                 = histogramIndex;
                    slotCosts[bestSlot]                      = bestCost;
                    clusters.slotIds[clusters.slotIdCount++] = bestSlot;
                } else {
                    const std::int32_t histogramIndex = slotHistograms[bestSlot];
                    Histogram&         target         = clusters.histograms[histogramIndex];
                    for (std::size_t s = 0; s < HuffmanSymbols; ++s)
                        target[s] = target[s] + cur[s];
                    clusters.histogramIndexes[i] = histogramIndex;
                    slotCosts[bestSlot] += bestCost;
                }
            }
        }

        void BuildJpegHuffmanTable(const Histogram& histo, HuffmanTable& table) noexcept
        {
            std::uint32_t counts[TreeSymbols];
            std::uint8_t  depths[TreeSymbols] = {};
            for (std::size_t j = 0; j < HuffmanSymbols; ++j)
                counts[j] = static_cast<std::uint32_t>(histo[j]);
            counts[HuffmanSymbols] = 1;
            CreateHuffmanTree(counts, TreeSymbols, HuffmanMaxBits, depths);
            table = HuffmanTable{};
            for (std::size_t i = 0; i < HuffmanSymbols; ++i)
                if (depths[i] > 0)
                    ++table.bits[depths[i]];
            std::int32_t offset[HuffmanMaxBits + 1] = {0};
            for (std::size_t i = 1; i <= HuffmanMaxBits; ++i)
                offset[i] = offset[i - 1] + table.bits[i - 1];
            for (std::size_t i = 0; i < HuffmanSymbols; ++i)
                if (depths[i] > 0)
                    table.values[static_cast<std::size_t>(offset[depths[i]]++)] = static_cast<std::uint8_t>(i);
        }

        void BuildHuffmanCodeTable(const HuffmanTable& table, CodeTable& code) noexcept
        {
            std::int32_t  huffCode[HuffmanSymbols];
            std::uint32_t huffSize[HuffmanSymbols + 1];
            std::int32_t  p = 0;
            for (std::uint32_t l = 1; l <= HuffmanMaxBits; ++l) {
                std::int32_t i = table.bits[l];
                while (i--)
                    huffSize[p++] = l;
            }
            const std::int32_t lastP = p;
            huffSize[lastP]          = 0;
            std::int32_t  nextCode   = 0;
            std::uint32_t si         = huffSize[0];
            p                        = 0;
            while (huffSize[p]) {
                while (huffSize[p] == si) {
                    huffCode[p++] = nextCode;
                    nextCode++;
                }
                nextCode <<= 1;
                si++;
            }
            for (p = 0; p < lastP; p++) {
                const std::int32_t i                    = table.values[static_cast<std::size_t>(p)];
                const std::int32_t nbits                = NumExtraBits(i);
                code.depth[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(huffSize[p]) + nbits;
                code.code[static_cast<std::size_t>(i)]  = huffCode[p] << nbits;
            }
        }

        // ------------------------------------------------------------------ block bit masks
        // Bit k (zigzag position) of nonzero: (|coef| >> al) != 0; of one: (|coef| >> al) == 1.
        // SSE2, so the tokenizers can jump from one significant coefficient to the next; the run
        // lengths are the distances between the bits.

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
            for (int i = 0; i < 4; ++i) {
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

        std::uint64_t RangeMask(std::int32_t ss, std::int32_t se) noexcept
        {
            const std::uint64_t upTo = se >= 63 ? ~0ull : ((1ull << (se + 1)) - 1);
            return upTo & ~((1ull << ss) - 1);
        }

        std::int32_t TrailingZeros(std::uint64_t v) noexcept
        {
            return static_cast<std::int32_t>(_tzcnt_u64(v));
        }

        // ------------------------------------------------------------------ tokenization

        // MCUs per row and MCU rows of a DC scan.
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

        // TokenizeProgressiveDC over the MCU rows [m0, m1). The DC predictor of each component
        // starts with the last block of that component before MCU row m0 (0 at the scan start), so
        // the bands of a DC scan need no stitch.
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
                                const std::int16_t dc
                                    = bx >= s.widthInBlocks[static_cast<std::size_t>(c)] || by >= s.heightInBlocks[static_cast<std::size_t>(c)]
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

        // The EOB-run token of TokenizeACProgressiveScan (emit_eob_run).
        Token EobRunToken(std::int32_t context, std::int32_t eobRun) noexcept
        {
            const std::int32_t nbits = FloorLog2(static_cast<std::uint32_t>(eobRun));
            return {static_cast<std::uint8_t>(context), static_cast<std::uint8_t>(nbits << 4), static_cast<std::uint16_t>(eobRun & ((1 << nbits) - 1))};
        }

        // TokenizeACProgressiveScan over the block rows [r0, r1). As a whole scan (result null) it is
        // jpegli's loop. As a band, the leading zero blocks are only counted and the pending run at
        // the first active block and at the end is left to the stitch.
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
                            tokens[n++] = {static_cast<std::uint8_t>(context), 0xF0, 0};
                            r -= 16;
                        }
                        const std::int32_t nbits  = FloorLog2(static_cast<std::uint32_t>(temp)) + 1;
                        const std::int32_t symbol = (r << 4) + nbits;
                        tokens[n++]
                            = {static_cast<std::uint8_t>(context), static_cast<std::uint8_t>(symbol), static_cast<std::uint16_t>(temp2 & ((1 << nbits) - 1))};
                    }
                    if (prev < si.se) { // zeros after the last significant coefficient
                        ++eobRun;
                        if (eobRun == 0x7FFF) {
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

        // TokenizeACRefinementScan over the block rows [r0, r1). As a band, the blocks before the
        // first newly significant coefficient only record their correction bits (they are EOB
        // blocks that extend the open run of the preceding band); from that block on the band runs
        // jpegli's loop with the state of a fresh scan.
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
                    if (!out.refTokens.Ensure(nextToken + blockRefTokens + 2) || !out.refBits.Ensure(nextRefBit + band + 1)
                        || !out.eobRuns.Ensure(nextEobRun + 2))
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
                            token.symbol        = 0xF0;
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
                        if (eobRefbits > 255) {
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
                        if (eobRun == 0x7FFF) {
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
                    ++result->histogram[tokens[i].symbol & 253];
            }
            return true;
        }

        // ------------------------------------------------------------------ writing

        // Non-zero if and only if one of the 8 bytes of x is zero.
        std::uint64_t HasZeroByte(std::uint64_t x) noexcept
        {
            return (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
        }

        // BitWriter without byte stuffing and padding: the plain bit stream of a chunk, packed
        // big-endian, for SpliceScanChunk.
        class RawBitWriter
        {
        public:
            explicit RawBitWriter(ByteBuffer& output) noexcept : output_(output) {}

            void WriteBits(std::int32_t nbits, std::uint64_t bits) noexcept
            {
                if (nbits == 0) {
                    healthy_ = false;
                    return;
                }
                freeBits_ -= nbits;
                if (freeBits_ < 0) {
                    putBuffer_ <<= (freeBits_ + nbits);
                    putBuffer_ |= (bits >> -freeBits_);
                    Store(putBuffer_);
                    freeBits_ += 64;
                    putBuffer_ = static_cast<std::uint64_t>(nbits);
                }
                putBuffer_ <<= nbits;
                putBuffer_ |= bits;
            }

            // Writes the pending bits (left-aligned in a last word) and returns the bit count.
            std::int64_t Finish() noexcept
            {
                const std::int64_t full  = static_cast<std::int64_t>(output_.Size()) * 8;
                const std::int32_t valid = 64 - freeBits_;
                if (valid > 0)
                    Store(putBuffer_ << freeBits_);
                if (!output_.Reserve(16)) // readable slack for the 64-bit loads of the splice
                    healthy_ = false;
                return full + valid;
            }

            bool Healthy() const noexcept { return healthy_; }

        private:
            void Store(std::uint64_t word) noexcept
            {
                if (!output_.Reserve(8)) {
                    healthy_ = false;
                    return;
                }
                std::uint8_t* out = output_.Data() + output_.Size();
                for (std::int32_t i = 0; i < 8; ++i)
                    out[i] = static_cast<std::uint8_t>(word >> (56 - 8 * i));
                output_.SetSize(output_.Size() + 8);
            }

            ByteBuffer&   output_;
            std::uint64_t putBuffer_ = 0;
            std::int32_t  freeBits_  = 64;
            bool          healthy_   = true;
        };

        // WriteTokens / WriteACRefinementTokens for the parts [p0, p1) of a scan; a refinement
        // scan reads its correction bits and run lengths from the cursor on (they are streams
        // across the parts).
        template <class Writer>
        void WriteParts(const EncoderSetup& setup, std::int32_t scanIndex, const ScanTokens& tokens, std::int32_t p0, std::int32_t p1,
                        const ChunkCursor& cursor, const HuffmanCoding& coding, Writer& bw) noexcept
        {
            const ScanInfo& si = setup.scans[static_cast<std::size_t>(scanIndex)];
            if (si.ah == 0) { // WriteTokens
                for (std::int32_t p = p0; p < p1; ++p) {
                    const TokenPart& part = tokens.parts[p];
                    for (std::size_t i = 0; i < part.tokenCount; ++i) {
                        const Token      t    = part.tokens[i];
                        const CodeTable& code = coding.codes[coding.contextMap[t.context]];
                        bw.WriteBits(code.depth[t.symbol], static_cast<std::uint64_t>(code.code[t.symbol] | t.bits));
                    }
                }
                return;
            }
            // WriteACRefinementTokens
            const CodeTable& code    = coding.codes[coding.contextMap[static_cast<std::size_t>(setup.acContextOffset[static_cast<std::size_t>(scanIndex)])]];
            std::int32_t     refPart = cursor.refPart, runPart = cursor.runPart;
            std::size_t      refbitIdx = cursor.refIndex, eobrunIdx = cursor.runIndex;
            for (std::int32_t p = p0; p < p1; ++p) {
                const TokenPart& part = tokens.parts[p];
                for (std::size_t i = 0; i < part.refTokenCount; ++i) {
                    const RefToken     t      = part.refTokens[i];
                    const std::int32_t symbol = t.symbol & 253;
                    std::uint16_t      bits   = 0;
                    if ((symbol & 1) == 0) {
                        const std::int32_t r = symbol >> 4;
                        if (r > 0 && r < 15) {
                            while (eobrunIdx >= tokens.parts[runPart].eobRunCount) {
                                ++runPart;
                                eobrunIdx = 0;
                            }
                            bits = tokens.parts[runPart].eobRuns[eobrunIdx++];
                        }
                    } else
                        bits = static_cast<std::uint16_t>((t.symbol >> 1) & 1);
                    bw.WriteBits(code.depth[static_cast<std::size_t>(symbol)], static_cast<std::uint64_t>(code.code[static_cast<std::size_t>(symbol)] | bits));
                    for (std::int32_t j = 0; j < t.refbits; ++j) {
                        while (refbitIdx >= tokens.parts[refPart].refBitCount) {
                            ++refPart;
                            refbitIdx = 0;
                        }
                        bw.WriteBits(1, tokens.parts[refPart].refBits[refbitIdx++]);
                    }
                }
            }
        }

    } // namespace

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

    std::int32_t DcScanMcuRows(const EncoderSetup& setup, std::int32_t scanIndex) noexcept
    {
        std::int32_t mcusPerRow, mcuRows;
        DcScanMcus(setup, setup.scans[static_cast<std::size_t>(scanIndex)], mcusPerRow, mcuRows);
        return mcuRows;
    }

    bool TokenizeDcBand(const EncoderSetup& setup, const Coefficients& coefficients, std::int32_t scanIndex, std::int32_t mcuRowBegin, std::int32_t mcuRowEnd,
                        TokenPart& part, Histograms& histograms) noexcept
    {
        const ScanInfo& si = setup.scans[static_cast<std::size_t>(scanIndex)];
        if (si.ah != 0 || !TokenizeDcRows(setup, coefficients, si, mcuRowBegin, mcuRowEnd, part))
            return false;
        for (std::size_t j = 0; j < part.tokenCount; ++j) {
            const Token t = part.tokens[j];
            ++histograms.count[t.context][t.symbol];
        }
        return true;
    }

    bool CanSplitScan(const EncoderSetup& setup, std::int32_t scanIndex) noexcept
    {
        const ScanInfo& si = setup.scans[static_cast<std::size_t>(scanIndex)];
        return si.ss > 0 && si.componentCount == 1;
    }

    std::int32_t ScanBlockRows(const EncoderSetup& setup, std::int32_t scanIndex) noexcept
    {
        return setup.heightInBlocks[static_cast<std::size_t>(setup.scans[static_cast<std::size_t>(scanIndex)].components[0])];
    }

    bool TokenizeScanBand(const EncoderSetup& setup, const Coefficients& coefficients, std::int32_t scanIndex, std::int32_t rowBegin, std::int32_t rowEnd,
                          TokenPart& part, BandResult& result) noexcept
    {
        const ScanInfo& si = setup.scans[static_cast<std::size_t>(scanIndex)];
        if (si.ah == 0)
            return TokenizeAcFirstRows(setup, coefficients, si, setup.acContextOffset[static_cast<std::size_t>(scanIndex)], rowBegin, rowEnd, part, &result);
        return TokenizeAcRefinementRows(setup, coefficients, si, rowBegin, rowEnd, part, &result);
    }

    bool StitchScanBands(const EncoderSetup& setup, std::int32_t scanIndex, std::int32_t bands, ScanTokens& tokens, const BandResult* results,
                         Histograms& histograms) noexcept
    {
        const ScanInfo&    si      = setup.scans[static_cast<std::size_t>(scanIndex)];
        const std::int32_t context = setup.acContextOffset[static_cast<std::size_t>(scanIndex)];
        auto&              histo   = histograms.count[static_cast<std::size_t>(context)];
        TokenPart&         final   = tokens.parts[2 * bands];
        if (si.ah == 0) {
            // AC first scan: leading zero blocks extend the run (forced out at 0x7FFF), the first
            // active block flushes it, then the band's own run continues.
            std::int32_t eobRun = 0;
            for (std::int32_t b = 0; b < bands; ++b) {
                const BandResult& r      = results[b];
                TokenPart&        prefix = tokens.parts[2 * b];
                if (!prefix.tokens.Allocate(static_cast<std::size_t>(r.leadingBlocks) / 0x7FFF + 2))
                    return false;
                const std::int64_t total = static_cast<std::int64_t>(eobRun) + r.leadingBlocks;
                for (std::int64_t e = total / 0x7FFF; e > 0; --e)
                    prefix.tokens[prefix.tokenCount++] = EobRunToken(context, 0x7FFF);
                eobRun = static_cast<std::int32_t>(total % 0x7FFF);
                if (r.active) {
                    if (eobRun > 0)
                        prefix.tokens[prefix.tokenCount++] = EobRunToken(context, eobRun);
                    eobRun = r.exitEobRun;
                    for (std::size_t sym = 0; sym < HuffmanSymbols; ++sym)
                        histo[sym] += r.histogram[sym];
                }
                for (std::size_t i = 0; i < prefix.tokenCount; ++i)
                    ++histo[prefix.tokens[i].symbol];
            }
            if (!final.tokens.Allocate(1))
                return false;
            if (eobRun > 0) {
                final.tokens[final.tokenCount++] = EobRunToken(context, eobRun);
                ++histo[final.tokens[0].symbol];
            }
            return true;
        }

        // AC refinement scan: replay jpegli's EOB state machine for the leading EOB blocks of
        // every band; it may extend the open token of the preceding band, split it after 255
        // correction bits, or start new tokens in the band's prefix part.
        std::int32_t   eobRun     = 0;
        std::int32_t   eobRefbits = 0;
        RefToken*      open       = nullptr; // the token of the open run
        std::uint16_t* lastRun    = nullptr; // its extra bits, once the run is 2 or longer
        auto           closeOpen  = [&]() noexcept {
            if (open != nullptr)
                ++histo[open->symbol & 253];
            open = nullptr;
        };
        for (std::int32_t b = 0; b < bands; ++b) {
            const BandResult& r      = results[b];
            TokenPart&        prefix = tokens.parts[2 * b];
            TokenPart&        own    = tokens.parts[2 * b + 1];
            if (!prefix.refTokens.Allocate(static_cast<std::size_t>(r.leadingBlocks) + 1)
                || !prefix.eobRuns.Allocate(static_cast<std::size_t>(r.leadingBlocks) / 2 + 2))
                return false;
            for (std::int32_t i = 0; i < r.leadingBlocks; ++i) {
                const std::int32_t bits = r.leadingBits[static_cast<std::size_t>(i)];
                ++eobRun;
                eobRefbits += bits;
                if (eobRefbits > 255) {
                    closeOpen();
                    eobRefbits = bits;
                    eobRun     = 1;
                }
                if (eobRun == 1)
                    open = &prefix.refTokens[prefix.refTokenCount++];
                open->refbits = static_cast<std::uint8_t>(eobRefbits);
                if (eobRun == 1)
                    open->symbol = 0;
                else if (eobRun == 2) {
                    open->symbol = 16;
                    lastRun      = &prefix.eobRuns[prefix.eobRunCount++];
                    *lastRun     = 0;
                } else if ((eobRun & (eobRun - 1)) == 0) {
                    open->symbol = static_cast<std::uint8_t>(open->symbol + 16);
                    *lastRun     = 0;
                } else
                    ++*lastRun;
                if (eobRun == 0x7FFF) {
                    closeOpen();
                    eobRun = eobRefbits = 0;
                }
            }
            if (r.active) {
                // the first newly significant coefficient ends the open run for good
                closeOpen();
                eobRun     = r.exitEobRun;
                eobRefbits = r.exitEobRefbits;
                if (eobRun > 0)
                    open = &own.refTokens[own.refTokenCount - 1];
                if (eobRun > 1)
                    lastRun = &own.eobRuns[own.eobRunCount - 1];
                for (std::size_t sym = 0; sym < HuffmanSymbols; ++sym)
                    histo[sym] += r.histogram[sym];
            }
        }
        closeOpen();
        return true;
    }

    void AddScanHistograms(const EncoderSetup& setup, std::int32_t scanIndex, const ScanTokens& tokens, Histograms& histograms) noexcept
    {
        const ScanInfo& si = setup.scans[static_cast<std::size_t>(scanIndex)];
        for (std::int32_t p = 0; p < tokens.partCount; ++p) {
            const TokenPart& part = tokens.parts[p];
            if (si.ss > 0 && si.ah > 0) {
                auto& acHisto = histograms.count[static_cast<std::size_t>(setup.acContextOffset[static_cast<std::size_t>(scanIndex)])];
                for (std::size_t j = 0; j < part.refTokenCount; ++j)
                    ++acHisto[part.refTokens[j].symbol & 253];
            } else {
                for (std::size_t j = 0; j < part.tokenCount; ++j) {
                    const Token t = part.tokens[j];
                    ++histograms.count[t.context][t.symbol];
                }
            }
        }
    }

    void OptimizeHuffmanCodes(const EncoderSetup& setup, const Histograms& histograms, HuffmanCoding& coding) noexcept
    {
        Clusters dc, ac;
        ClusterJpegHistograms(histograms.count.data(), setup.frame.components, dc);
        ClusterJpegHistograms(histograms.count.data() + 4, setup.contextCount - 4, ac);
        const std::int32_t numDc = dc.histogramCount;
        coding.tableCount        = numDc + ac.histogramCount;
        for (std::int32_t i = 0; i < coding.tableCount; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (i < numDc) {
                coding.slotIds[ui] = static_cast<std::uint8_t>(i);
                BuildJpegHuffmanTable(dc.histograms[i], coding.tables[ui]);
            } else {
                coding.slotIds[ui] = static_cast<std::uint8_t>(16 + ac.slotIds[i - numDc]);
                BuildJpegHuffmanTable(ac.histograms[i - numDc], coding.tables[ui]);
            }
            coding.codes[ui] = CodeTable{};
            BuildHuffmanCodeTable(coding.tables[ui], coding.codes[ui]);
        }
        coding.contextMap = {};
        for (std::int32_t i = 0; i < setup.contextCount; ++i) {
            if (i < setup.frame.components)
                coding.contextMap[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(dc.histogramIndexes[i]);
            else if (i >= 4)
                coding.contextMap[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(numDc + ac.histogramIndexes[i - 4]);
        }
    }

    bool WriteScanData(const EncoderSetup& setup, std::int32_t scanIndex, const ScanTokens& tokens, const HuffmanCoding& coding, ByteBuffer& output) noexcept
    {
        BitWriter bw(output);
        WriteParts(setup, scanIndex, tokens, 0, tokens.partCount, ChunkCursor{}, coding, bw);
        bw.JumpToByteBoundary();
        return bw.Healthy();
    }

    void CountRefinementStreams(const ScanTokens& tokens, std::int32_t partBegin, std::int32_t partEnd, std::int64_t& refBits, std::int64_t& runs) noexcept
    {
        std::int64_t bits = 0, count = 0;
        for (std::int32_t p = partBegin; p < partEnd; ++p) {
            const TokenPart& part = tokens.parts[p];
            for (std::size_t i = 0; i < part.refTokenCount; ++i) {
                const RefToken     t      = part.refTokens[i];
                const std::int32_t symbol = t.symbol & 253;
                const std::int32_t r      = symbol >> 4;
                bits += t.refbits;
                count += (symbol & 1) == 0 && r > 0 && r < 15 ? 1 : 0;
            }
        }
        refBits = bits;
        runs    = count;
    }

    ChunkCursor LocateRefinementStreams(const ScanTokens& tokens, std::int64_t refBits, std::int64_t runs) noexcept
    {
        ChunkCursor cursor;
        while (cursor.refPart < tokens.partCount - 1 && refBits >= static_cast<std::int64_t>(tokens.parts[cursor.refPart].refBitCount))
            refBits -= static_cast<std::int64_t>(tokens.parts[cursor.refPart++].refBitCount);
        while (cursor.runPart < tokens.partCount - 1 && runs >= static_cast<std::int64_t>(tokens.parts[cursor.runPart].eobRunCount))
            runs -= static_cast<std::int64_t>(tokens.parts[cursor.runPart++].eobRunCount);
        cursor.refIndex = static_cast<std::size_t>(refBits);
        cursor.runIndex = static_cast<std::size_t>(runs);
        return cursor;
    }

    bool WriteScanChunk(const EncoderSetup& setup, std::int32_t scanIndex, const ScanTokens& tokens, std::int32_t partBegin, std::int32_t partEnd,
                        const ChunkCursor& cursor, const HuffmanCoding& coding, ChunkBits& out) noexcept
    {
        out.bytes.Clear();
        RawBitWriter bw(out.bytes);
        WriteParts(setup, scanIndex, tokens, partBegin, partEnd, cursor, coding, bw);
        out.bitCount = bw.Finish();
        return bw.Healthy();
    }

    bool SpliceScanChunk(const ChunkBits* chunks, std::int32_t chunkCount, std::int32_t chunk, ByteBuffer& output) noexcept
    {
        std::int64_t start = 0, total = 0;
        for (std::int32_t i = 0; i < chunkCount; ++i) {
            if (i < chunk)
                start += chunks[i].bitCount;
            total += chunks[i].bitCount;
        }
        const std::int64_t length = chunks[chunk].bitCount;
        const std::int64_t end    = start + length;
        // the bytes whose first bit lies in this chunk; the last chunk also pads with 1 bits
        const std::int64_t j0 = (start + 7) / 8;
        const std::int64_t j1 = (end + 7) / 8;
        output.Clear();
        if (j1 <= j0)
            return true;
        if (!output.Reserve(static_cast<std::size_t>(j1 - j0) * 2))
            return false;
        // one bit of the scan's stream, 1 after its end (JumpToByteBoundary pads with 1 bits)
        auto bitAt = [&](std::int64_t g) noexcept -> std::uint32_t {
            if (g >= total)
                return 1;
            std::int32_t c  = chunk;
            std::int64_t cs = start;
            while (g >= cs + chunks[c].bitCount) {
                cs += chunks[c].bitCount;
                ++c;
            }
            const std::int64_t  local = g - cs;
            const std::uint8_t* bytes = chunks[c].bytes.Data();
            return (bytes[local >> 3] >> (7 - (local & 7))) & 1u;
        };
        const std::uint8_t* bytes = chunks[chunk].bytes.Data();
        std::uint8_t*       out   = output.Data();
        std::size_t         n     = 0;
        auto                emit  = [&](std::uint8_t byte) noexcept {
            out[n++] = byte;
            if (byte == 0xFF)
                out[n++] = 0;
        };
        std::int64_t       j     = j0;
        const std::int32_t shift = static_cast<std::int32_t>((8 * j0 - start) & 7); // bit offset of every byte in the chunk
        // whole words inside the chunk: 8 bytes from a 64-bit window (and the top bits of the next byte)
        for (; 8 * (j + 8) <= end; j += 8) {
            const std::size_t   b    = static_cast<std::size_t>((8 * j - start) >> 3);
            const std::uint64_t word = _byteswap_uint64(*reinterpret_cast<const std::uint64_t*>(bytes + b));
            const std::uint64_t v    = shift == 0 ? word : (word << shift) | (static_cast<std::uint64_t>(bytes[b + 8]) >> (8 - shift));
            if (HasZeroByte(~v)) {
                for (std::int32_t s = 56; s >= 0; s -= 8)
                    emit(static_cast<std::uint8_t>(v >> s));
            } else {
                *reinterpret_cast<std::uint64_t*>(out + n) = _byteswap_uint64(v);
                n += 8;
            }
        }
        for (; j < j1; ++j) { // the rest bit by bit, across the following chunks and the padding
            std::uint32_t byte = 0;
            for (std::int64_t g = 8 * j; g < 8 * j + 8; ++g)
                byte = (byte << 1) | bitAt(g);
            emit(static_cast<std::uint8_t>(byte));
        }
        output.SetSize(n);
        return true;
    }

    bool WriteScanHeader(const EncoderSetup& setup, std::int32_t scanIndex, const HuffmanCoding& coding, std::int32_t& nextDhtIndex,
                         ByteBuffer& output) noexcept
    {
        const ScanInfo& si     = setup.scans[static_cast<std::size_t>(scanIndex)];
        std::int32_t    numDht = 0;
        if (scanIndex == 0) {
            // all DC tables and at most 4 AC tables before the first scan
            for (std::int32_t i = 0, numAc = 0; i < coding.tableCount; ++i) {
                if (coding.slotIds[static_cast<std::size_t>(i)] >= 16 && numAc++ >= 4)
                    break;
                ++numDht;
            }
        } else if (si.ss > 0) {
            // progressive AC scans need at most one new table
            if (coding.contextMap[static_cast<std::size_t>(setup.acContextOffset[static_cast<std::size_t>(scanIndex)])] == nextDhtIndex)
                numDht = 1;
        }
        if (numDht > 0) {
            if (!WriteDht(output, coding.tables.data() + nextDhtIndex, coding.slotIds.data() + nextDhtIndex, numDht))
                return false;
            nextDhtIndex += numDht;
        }
        std::int32_t dcSlot[MaxComponents], acSlot[MaxComponents];
        for (std::int32_t i = 0; i < si.componentCount; ++i) {
            const std::int32_t c         = si.components[static_cast<std::size_t>(i)];
            const std::int32_t acContext = setup.acContextOffset[static_cast<std::size_t>(scanIndex)] + i;
            dcSlot[i]                    = coding.slotIds[coding.contextMap[static_cast<std::size_t>(c)]];
            acSlot[i]                    = coding.slotIds[coding.contextMap[static_cast<std::size_t>(acContext)]] - 16;
        }
        return WriteSos(output, setup.frame, si, dcSlot, acSlot);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
