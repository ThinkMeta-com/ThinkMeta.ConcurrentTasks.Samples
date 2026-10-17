//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#include "Huffman.h"

#include "DeflateTables.h"

#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    namespace {

        constexpr std::uint32_t NumSymbolBits = 10;
        constexpr std::uint32_t SymbolMask    = (1u << NumSymbolBits) - 1;
        constexpr std::uint32_t FreqMask      = ~SymbolMask;

        /// <summary>Sifts a[subtreeIdx] down until the max-heap property holds below it (1-based indices).</summary>
        void HeapifySubtree(std::uint32_t* a, std::uint32_t length, std::uint32_t subtreeIdx) noexcept
        {
            const std::uint32_t v         = a[subtreeIdx];
            std::uint32_t       parentIdx = subtreeIdx;
            std::uint32_t       childIdx;
            while ((childIdx = parentIdx * 2) <= length) {
                if (childIdx < length && a[childIdx + 1] > a[childIdx])
                    ++childIdx;
                if (v >= a[childIdx])
                    break;
                a[parentIdx] = a[childIdx];
                parentIdx    = childIdx;
            }
            a[parentIdx] = v;
        }

        void HeapSort(std::uint32_t* a, std::uint32_t length) noexcept
        {
            --a; // 1-based indices
            for (std::uint32_t i = length / 2; i >= 1; --i)
                HeapifySubtree(a, length, i);
            while (length >= 2) {
                const std::uint32_t tmp = a[length];
                a[length]               = a[1];
                a[1]                    = tmp;
                --length;
                HeapifySubtree(a, length, 1);
            }
        }

        /// <summary>
        /// Writes the used symbols as (frequency << 10 | symbol) to symOut, sorted by frequency then symbol, and sets the
        /// lengths of unused symbols to 0. A counting sort over numSyms counters handles the low frequencies; the last
        /// counter collects the high ones, which are heap-sorted. Returns the number of used symbols.
        /// </summary>
        std::uint32_t SortSymbols(std::uint32_t numSyms, const std::uint32_t* freqs, std::uint8_t* lens, std::uint32_t* symOut) noexcept
        {
            std::uint32_t       counters[MaxNumSyms];
            const std::uint32_t numCounters = numSyms;
            std::memset(counters, 0, numCounters * sizeof(counters[0]));

            for (std::uint32_t sym = 0; sym < numSyms; ++sym)
                ++counters[freqs[sym] < numCounters - 1 ? freqs[sym] : numCounters - 1];

            std::uint32_t numUsed = 0;
            for (std::uint32_t i = 1; i < numCounters; ++i) {
                const std::uint32_t count = counters[i];
                counters[i]               = numUsed;
                numUsed += count;
            }

            for (std::uint32_t sym = 0; sym < numSyms; ++sym) {
                const std::uint32_t freq = freqs[sym];
                if (freq != 0)
                    symOut[counters[freq < numCounters - 1 ? freq : numCounters - 1]++] = sym | (freq << NumSymbolBits);
                else
                    lens[sym] = 0;
            }

            HeapSort(symOut + counters[numCounters - 2], counters[numCounters - 1] - counters[numCounters - 2]);
            return numUsed;
        }

        /// <summary>
        /// Builds the non-leaf nodes of the Huffman tree in place: a[0..symCount-2] receive the parent index (<< 10) of
        /// each non-leaf, a[symCount-2] is the root. Input: frequencies (<< 10) sorted ascending. The low 10 bits are kept.
        /// </summary>
        void BuildTree(std::uint32_t* a, std::uint32_t symCount) noexcept
        {
            const std::uint32_t lastIdx = symCount - 1;
            std::uint32_t       i       = 0; // next leaf without parent
            std::uint32_t       b       = 0; // next non-leaf without parent (b == e: none)
            std::uint32_t       e       = 0; // next free slot for a non-leaf
            do {
                std::uint32_t newFreq;
                if (i + 1 <= lastIdx && (b == e || (a[i + 1] & FreqMask) <= (a[b] & FreqMask))) {
                    newFreq = (a[i] & FreqMask) + (a[i + 1] & FreqMask); // two leaves
                    i += 2;
                } else if (b + 2 <= e && (i > lastIdx || (a[b + 1] & FreqMask) < (a[i] & FreqMask))) {
                    newFreq  = (a[b] & FreqMask) + (a[b + 1] & FreqMask); // two non-leaves
                    a[b]     = (e << NumSymbolBits) | (a[b] & SymbolMask);
                    a[b + 1] = (e << NumSymbolBits) | (a[b + 1] & SymbolMask);
                    b += 2;
                } else {
                    newFreq = (a[i] & FreqMask) + (a[b] & FreqMask); // one leaf, one non-leaf
                    a[b]    = (e << NumSymbolBits) | (a[b] & SymbolMask);
                    ++i;
                    ++b;
                }
                a[e] = newFreq | (a[e] & SymbolMask);
            } while (++e < lastIdx);
        }

        /// <summary>
        /// Converts the parent links to depths (walking the non-leaves from the root down) and counts the codewords per
        /// length. Depths beyond maxCodewordLen take the longest shorter length that still has codewords.
        /// </summary>
        void ComputeLengthCounts(std::uint32_t* a, std::uint32_t rootIdx, std::uint32_t* lenCounts, std::uint32_t maxCodewordLen) noexcept
        {
            for (std::uint32_t len = 0; len <= maxCodewordLen; ++len)
                lenCounts[len] = 0;
            lenCounts[1] = 2;

            a[rootIdx] &= SymbolMask;
            for (auto node = static_cast<std::int32_t>(rootIdx) - 1; node >= 0; --node) {
                const std::uint32_t parent      = a[node] >> NumSymbolBits;
                const std::uint32_t parentDepth = a[parent] >> NumSymbolBits;
                std::uint32_t       depth       = parentDepth + 1;

                a[node] = (a[node] & SymbolMask) | (depth << NumSymbolBits);

                if (depth >= maxCodewordLen) {
                    depth = maxCodewordLen;
                    do {
                        --depth;
                    } while (lenCounts[depth] == 0);
                }
                --lenCounts[depth];
                lenCounts[depth + 1] += 2;
            }
        }

        std::uint32_t ReverseCodeword(std::uint32_t codeword, std::uint32_t len) noexcept
        {
            std::uint32_t r = 0;
            for (std::uint32_t i = 0; i < len; ++i) {
                r = (r << 1) | (codeword & 1);
                codeword >>= 1;
            }
            return r;
        }

        /// <summary>
        /// Assigns the lengths in decreasing order to the symbols sorted by increasing frequency (a[i] low bits), then
        /// the canonical codewords in symbol order.
        /// </summary>
        void GenCodewords(std::uint32_t* a, std::uint8_t* lens, const std::uint32_t* lenCounts, std::uint32_t maxCodewordLen,
                          std::uint32_t numSyms) noexcept
        {
            std::uint32_t i = 0;
            for (std::uint32_t len = maxCodewordLen; len >= 1; --len) {
                std::uint32_t count = lenCounts[len];
                while (count-- != 0)
                    lens[a[i++] & SymbolMask] = static_cast<std::uint8_t>(len);
            }

            std::uint32_t nextCodewords[MaxCodewordLen + 1];
            nextCodewords[0] = 0;
            nextCodewords[1] = 0;
            for (std::uint32_t len = 2; len <= maxCodewordLen; ++len)
                nextCodewords[len] = (nextCodewords[len - 1] + lenCounts[len - 1]) << 1;

            for (std::uint32_t sym = 0; sym < numSyms; ++sym)
                a[sym] = ReverseCodeword(nextCodewords[lens[sym]]++, lens[sym]);
        }

    } // namespace

    void MakeHuffmanCode(std::uint32_t numSyms, std::uint32_t maxCodewordLen, const std::uint32_t* freqs, std::uint8_t* lens,
                         std::uint32_t* codewords) noexcept
    {
        std::uint32_t* a       = codewords; // shares the storage of the result
        const auto     numUsed = SortSymbols(numSyms, freqs, lens, a);

        if (numUsed < 2) {
            const std::uint32_t sym        = numUsed != 0 ? (a[0] & SymbolMask) : 0;
            const std::uint32_t nonzeroIdx = sym != 0 ? sym : 1;
            codewords[0]                   = 0;
            lens[0]                        = 1;
            codewords[nonzeroIdx]          = 1;
            lens[nonzeroIdx]               = 1;
            return;
        }

        BuildTree(a, numUsed);
        std::uint32_t lenCounts[MaxCodewordLen + 1];
        ComputeLengthCounts(a, numUsed - 2, lenCounts, maxCodewordLen);
        GenCodewords(a, lens, lenCounts, maxCodewordLen, numSyms);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
