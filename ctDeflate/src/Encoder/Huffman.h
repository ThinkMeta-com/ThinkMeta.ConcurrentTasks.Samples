//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    /// <summary>
    /// libdeflate: deflate_make_huffman_code() (deflate_compress.c). Builds a length-limited canonical Huffman code:
    /// symbols sorted by frequency then value, a stripped-down tree of non-leaves, lengths clamped to maxCodewordLen,
    /// bit-reversed codewords. Fewer than two used symbols give two codewords of length 1 (symbol 0 and the used one,
    /// or symbol 1).
    /// </summary>
    /// <param name="numSyms">Alphabet size, 2..288.</param>
    /// <param name="freqs">Frequency of each symbol; the sum must stay below 2^22.</param>
    /// <param name="lens">Out: codeword length per symbol (0 for unused symbols).</param>
    /// <param name="codewords">Out: codeword per symbol, bit-reversed for DEFLATE (undefined for unused symbols).</param>
    void MakeHuffmanCode(std::uint32_t numSyms, std::uint32_t maxCodewordLen, const std::uint32_t* freqs, std::uint8_t* lens,
                         std::uint32_t* codewords) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
