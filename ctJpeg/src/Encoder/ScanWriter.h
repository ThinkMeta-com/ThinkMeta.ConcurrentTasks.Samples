// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstddef>
#include <cstdint>

#include "EncoderSetup.h"
#include "Entropy.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>
    /// Writes the tokens of the parts [p0, p1) of a scan with the Huffman codes of coding to bw
    /// (BitWriter for a whole scan, RawBitWriter for a chunk). A refinement scan reads its
    /// correction bits and run lengths from the cursor on (they are streams across the parts).
    /// jpegli: WriteTokens() and WriteACRefinementTokens() (bitstream.cc).
    /// </summary>
    template <class Writer>
    void WriteTokenParts(const EncoderSetup& setup, std::int32_t scanIndex, const ScanTokens& tokens, std::int32_t p0, std::int32_t p1,
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
                const std::int32_t symbol = t.symbol & RefSymbolMask;
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

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
