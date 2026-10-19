//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include "BitWriter.h"
#include "DeflateTables.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>libdeflate: struct deflate_freqs (deflate_compress.c). Symbol frequencies of one block.</summary>
    struct DeflateFreqs {
        std::uint32_t litlen[NumLitlenSyms];
        std::uint32_t offset[NumOffsetSyms];
    };

    /// <summary>libdeflate: struct deflate_codes (deflate_compress.c). Huffman codes of one block. The lengths are
    /// contiguous (litlen, then offset) for the precode.</summary>
    struct DeflateCodes {
        std::uint32_t litlenCodewords[NumLitlenSyms];
        std::uint32_t offsetCodewords[NumOffsetSyms];
        std::uint8_t  lens[NumLitlenSyms + NumOffsetSyms];

        std::uint8_t*       LitlenLens() noexcept { return lens; }
        const std::uint8_t* LitlenLens() const noexcept { return lens; }
        std::uint8_t*       OffsetLens() noexcept { return lens + NumLitlenSyms; }
        const std::uint8_t* OffsetLens() const noexcept { return lens + NumLitlenSyms; }
    };

    // libdeflate: SEQ_LENGTH_SHIFT, SEQ_LITRUNLEN_MASK (deflate_compress.c)
    constexpr std::uint32_t SeqLengthShift     = 23;
    constexpr std::uint32_t SeqLitrunlenMask   = (1u << SeqLengthShift) - 1;

    /// <summary>libdeflate: struct deflate_sequence (deflate_compress.c). A run of literals followed by a match (length
    /// 0: the last run of the block, no match).</summary>
    struct DeflateSequence {
        std::uint32_t litrunlenAndLength; // bits 0..22 literal count, bits 23..31 match length
        std::uint16_t offset;
        std::uint16_t offsetSlot;
    };

    /// <summary>
    /// New in ctDeflate, not in libdeflate (parallel encoding). A block encoded apart from the bitstream: the
    /// compressed form from bit 0 (unless it is never used) and for each start bit offset 0..7 whether the uncompressed
    /// form wins there (its alignment bits depend on that offset).
    /// </summary>
    struct EncodedBlock {
        std::vector<std::uint8_t> bits;
        std::uint64_t             bitCount   = 0;
        std::uint8_t              storedMask = 0;
    };

    /// <summary>libdeflate: the costs dynamic_cost, static_cost, uncompressed_cost of deflate_flush_block()
    /// (deflate_compress.c). Bit costs of the block types.</summary>
    struct BlockCosts {
        std::uint32_t dynamicCost;
        std::uint32_t staticCost;
        std::uint32_t uncompressedBaseCost; // without the alignment bits

        std::uint32_t UncompressedCost(std::uint32_t bitOffset) const noexcept { return uncompressedBaseCost + ((0u - (bitOffset + 3)) & 7); }
    };

    /// <summary>
    /// libdeflate: deflate_finish_block(), deflate_flush_block() (deflate_compress.c). Chooses the cheapest block type
    /// (dynamic, static, uncompressed; ties prefer uncompressed, then static) and writes the block. Holds the static
    /// codes and scratch space (libdeflate: the codes and c->o.precode, c->o.length of struct libdeflate_compressor).
    /// </summary>
    class BlockWriter
    {
    public:
        BlockWriter() noexcept;

        /// <summary>
        /// libdeflate: deflate_finish_block() (deflate_compress.c). Adds the end-of-block symbol to freqs, builds the
        /// dynamic codes and writes the block covering blockBegin[0..blockLength) with the given sequences (terminated by a
        /// sequence of match length 0).
        /// </summary>
        void FinishBlock(BitWriter& os, const std::uint8_t* blockBegin, std::uint32_t blockLength, const DeflateSequence* sequences,
                         DeflateFreqs& freqs, bool isFinalBlock) noexcept;

        /// <summary>
        /// New in ctDeflate (parallel encoding); follows libdeflate: deflate_finish_block() (deflate_compress.c).
        /// Like FinishBlock, but writes into out instead of a bitstream; the choice of an uncompressed block is left to
        /// the caller (EncodedBlock::storedMask) because it depends on the bit offset where the block starts.
        /// </summary>
        void Encode(const std::uint8_t* blockBegin, std::uint32_t blockLength, const DeflateSequence* sequences, DeflateFreqs& freqs,
                    bool isFinalBlock, EncodedBlock& out);

        /// <summary>libdeflate: deflate_flush_block(), uncompressed part (deflate_compress.c). Writes
        /// blockBegin[0..blockLength) as uncompressed block(s).</summary>
        static void WriteStored(BitWriter& os, const std::uint8_t* blockBegin, std::size_t blockLength, bool isFinalBlock) noexcept;
        /// <summary>New in ctDeflate, not in libdeflate (parallel encoding). Appends bitCount bits (LSB first) to the
        /// bitstream.</summary>
        static void AppendBits(BitWriter& os, const std::uint8_t* bits, std::uint64_t bitCount) noexcept;

    private:
        void ComputeCosts(const DeflateFreqs& freqs, std::uint32_t blockLength, BlockCosts& costs) noexcept;
        void WriteCompressed(BitWriter& os, const std::uint8_t* blockBegin, const DeflateSequence* sequences, bool isFinalBlock,
                             bool useStatic) noexcept;
        void PrecomputeHuffmanHeader() noexcept;
        void ComputeFullLenCodewords(const DeflateCodes& codes) noexcept;
        void FlushBlock(BitWriter& os, const std::uint8_t* blockBegin, std::uint32_t blockLength, const DeflateSequence* sequences,
                        const DeflateFreqs& freqs, bool isFinalBlock) noexcept;

        DeflateCodes codes_;
        DeflateCodes staticCodes_;

        struct {
            std::uint32_t freqs[NumPrecodeSyms];
            std::uint32_t codewords[NumPrecodeSyms];
            std::uint8_t  lens[NumPrecodeSyms];
            std::uint32_t items[NumLitlenSyms + NumOffsetSyms];
            std::uint32_t numLitlenSyms;
            std::uint32_t numOffsetSyms;
            std::uint32_t numExplicitLens;
            std::uint32_t numItems;
        } precode_;

        std::uint32_t lengthCodewords_[MaxMatchLen + 1];
        std::uint8_t  lengthLens_[MaxMatchLen + 1];
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
