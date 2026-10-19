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

// Port of jpegli's Huffman code optimization: entropy_coding.cc (HistogramCost,
// ClusterJpegHistograms, BuildJpegHuffmanTable, OptimizeHuffmanCodes, BuildHuffmanCodeTable) and
// huffman.cc (CreateHuffmanTree, SetDepth). The clustering keeps jpegli's float costs.

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    namespace {

        // DHT slots per table class (Th 0..3): a progressive scan can only refer to 4 tables.
        constexpr std::int32_t TableSlots = 4;

        // Extra bits of a symbol: run/size symbols carry 'size' extra bits, EOB runs (size 0)
        // carry 'run' bits, ZRL (0xF0) none.
        // jpegli: kNumExtraBits (entropy_coding.cc).
        std::int32_t NumExtraBits(std::int32_t symbol) noexcept
        {
            const std::int32_t run = symbol >> 4, size = symbol & 15;
            return size != 0 ? size : (run == 15 ? 0 : run);
        }

        // ------------------------------------------------------------------ CreateHuffmanTree

        // jpegli: HuffmanTree (huffman.cc).
        struct TreeNode {
            std::uint32_t totalCount;
            std::int16_t  indexLeft;
            std::int16_t  indexRightOrValue;
        };

        // jpegli: SetDepth() (huffman.cc).
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
        // jpegli: Compare() (huffman.cc).
        bool TreeLess(const TreeNode& v0, const TreeNode& v1) noexcept
        {
            return v0.totalCount != v1.totalCount ? v0.totalCount < v1.totalCount : v0.indexRightOrValue > v1.indexRightOrValue;
        }

        constexpr std::size_t TreeSymbols = HuffmanSymbols + 1; // 256 symbols + the reserved one

        // Code lengths (depth) of a Huffman code for the symbol counts data, at most treeLimit bits:
        // if the tree is too deep, the small counts are raised and the tree is built again.
        // jpegli: CreateHuffmanTree() (huffman.cc).
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
                // The node layout (jpegli's comment):
                //   [0, n)      the sorted leaves we start with,
                //   [n]         a sentinel,
                //   [n + 1, 2n) the new parent nodes, added from n + 1 on; they come out in
                //               ascending order of their counts,
                //   [2n]        a sentinel at the end as well,
                // 2n + 1 nodes in all. Each step takes the two smallest of the next leaf (i) and the
                // next parent (j); the sentinels (count 0xFFFFFFFF) stop either run at its end.
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
                    // the last sentinel becomes the parent node, a new sentinel follows it
                    const std::size_t jEnd = size - 1;
                    tree[jEnd]   = {tree[left].totalCount + tree[right].totalCount, static_cast<std::int16_t>(left), static_cast<std::int16_t>(right)};
                    tree[size++] = sentinel;
                }
                SetDepth(tree[2 * n - 1], tree, depth, 0);
                // too deep: raise the smallest counts and try again
                std::uint8_t maxDepth = 0;
                for (std::size_t s = 0; s < length; ++s)
                    maxDepth = depth[s] > maxDepth ? depth[s] : maxDepth;
                if (maxDepth <= treeLimit)
                    break;
            }
        }

        // ------------------------------------------------------------------ clustering

        using Histogram = std::array<std::int32_t, HuffmanSymbols>;

        // jpegli: IsEmptyHistogram() (entropy_coding.cc).
        bool IsEmpty(const Histogram& h) noexcept
        {
            for (const std::int32_t c : h)
                if (c)
                    return false;
            return true;
        }

        // jpegli: HistogramCost() (entropy_coding.cc).
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

        // The result of ClusterJpegHistograms (jpegli: JpegClusteredHistograms).
        struct Clusters {
            Histogram    histograms[MaxContexts];
            std::int32_t histogramCount = 0;
            std::int32_t histogramIndexes[MaxContexts]{};
            std::int32_t slotIds[MaxContexts]{};
            std::int32_t slotIdCount = 0;
        };

        // jpegli: ClusterJpegHistograms() (entropy_coding.cc).
        void ClusterJpegHistograms(const Histogram* histograms, std::int32_t num, Clusters& clusters) noexcept
        {
            std::int32_t slotHistograms[TableSlots];
            float        slotCosts[TableSlots];
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
                    if (bestSlot < TableSlots) {
                        slotHistograms[slots] = histogramIndex;
                        slotCosts[slots]      = bestCost;
                        ++slots;
                    } else
                        bestSlot = (clusters.slotIds[clusters.slotIdCount - 1] + 1) % TableSlots;
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

        // jpegli: BuildJpegHuffmanTable() (entropy_coding.cc).
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

        // jpegli: BuildHuffmanCodeTable() (entropy_coding.cc).
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

    } // namespace

    void OptimizeHuffmanCodes(const EncoderSetup& setup, const Histograms& histograms, HuffmanCoding& coding) noexcept
    {
        Clusters dc, ac;
        ClusterJpegHistograms(histograms.count.data(), setup.frame.components, dc);
        ClusterJpegHistograms(histograms.count.data() + DcContexts, setup.contextCount - DcContexts, ac);
        const std::int32_t numDc = dc.histogramCount;
        coding.tableCount        = numDc + ac.histogramCount;
        for (std::int32_t i = 0; i < coding.tableCount; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (i < numDc) {
                coding.slotIds[ui] = static_cast<std::uint8_t>(i);
                BuildJpegHuffmanTable(dc.histograms[i], coding.tables[ui]);
            } else {
                coding.slotIds[ui] = static_cast<std::uint8_t>(AcSlotBase + ac.slotIds[i - numDc]);
                BuildJpegHuffmanTable(ac.histograms[i - numDc], coding.tables[ui]);
            }
            coding.codes[ui] = CodeTable{};
            BuildHuffmanCodeTable(coding.tables[ui], coding.codes[ui]);
        }
        coding.contextMap = {};
        for (std::int32_t i = 0; i < setup.contextCount; ++i) {
            if (i < setup.frame.components)
                coding.contextMap[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(dc.histogramIndexes[i]);
            else if (i >= DcContexts)
                coding.contextMap[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(numDc + ac.histogramIndexes[i - DcContexts]);
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
