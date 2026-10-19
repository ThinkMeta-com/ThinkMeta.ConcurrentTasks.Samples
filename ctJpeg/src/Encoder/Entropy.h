// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "ByteBuffer.h"
#include "Coefficients.h"
#include "EncoderSetup.h"
#include "HeapArray.h"
#include "JpegTypes.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    // Port of jpegli's entropy coding (entropy_coding.cc, huffman.cc, bitstream.cc) for the
    // progressive scans of level 2 without restart markers, plus the parallel parts of the encoder.
    // Implemented by jpegli's file:
    //   Tokenize.cpp      entropy_coding.cc: TokenizeScan and the tokenizers, BuildHistograms
    //   HuffmanCodes.cpp  entropy_coding.cc: OptimizeHuffmanCodes, clustering, code tables; huffman.cc
    //   ScanWriter.cpp    bitstream.cc: WriteScanHeader, WriteScanData (WriteTokens, WriteACRefinementTokens)
    // and new in ctJpeg, not in jpegli:
    //   BandStitch.cpp    a scan tokenized in row bands that a serial stitch joins to exactly jpegli's tokens
    //   ChunkSplice.cpp   a scan written in chunks of plain bits that a splice joins to jpegli's bytes

    /// <summary>The DC contexts 0..3 (one per component); the AC contexts follow (jpegli's ac_ctx_offset starts at 4).</summary>
    inline constexpr std::int32_t DcContexts = 4;

    /// <summary>Upper bound of entropy contexts: 4 DC + one AC context per component and AC scan.</summary>
    inline constexpr std::int32_t MaxContexts = DcContexts + 4 * MaxComponents;

    /// <summary>The longest EOB run of one token: the extra bits of EOB14 hold at most 0x7FFF blocks.</summary>
    inline constexpr std::int32_t MaxEobRun = 0x7FFF;

    /// <summary>The most correction bits after one refinement token (RefToken::refbits is a byte); a longer EOB run starts a new token.</summary>
    inline constexpr std::int32_t MaxRefBits = 255;

    /// <summary>Clears the sign (bit 1) of a refinement symbol, leaving the Huffman symbol.</summary>
    inline constexpr std::int32_t RefSymbolMask = 253;

    /// <summary>The DHT class/index byte of an AC table: class 1 in the high nibble.</summary>
    inline constexpr std::int32_t AcSlotBase = 16;

    /// <summary>A Huffman symbol with its extra bits (jpegli Token).</summary>
    struct Token {
        std::uint8_t  context;
        std::uint8_t  symbol;
        std::uint16_t bits;
    };

    /// <summary>An AC refinement symbol: sign in bit 1, number of correction bits after it (RefToken).</summary>
    struct RefToken {
        std::uint8_t symbol;
        std::uint8_t refbits;
    };

    /// <summary>One segment of a scan's token stream; the segments of a scan are read in order.</summary>
    struct TokenPart {
        HeapArray<Token>         tokens; // DC and AC first scans
        std::size_t              tokenCount = 0;
        HeapArray<RefToken>      refTokens; // AC refinement scans
        std::size_t              refTokenCount = 0;
        HeapArray<std::uint8_t>  refBits;
        std::size_t              refBitCount = 0;
        HeapArray<std::uint16_t> eobRuns;
        std::size_t              eobRunCount = 0;
    };

    /// <summary>The tokens of one scan: one part, or 2 * bands + 1 parts (prefix, band, ..., final).</summary>
    struct ScanTokens {
        std::unique_ptr<TokenPart[]> parts;
        std::int32_t                 partCount = 0;

        /// <summary>Allocates count empty parts; false if out of memory.</summary>
        bool AllocateParts(std::int32_t count) noexcept;
    };

    /// <summary>Symbol counts per context (jpegli Histogram).</summary>
    struct Histograms {
        std::array<std::array<std::int32_t, HuffmanSymbols>, MaxContexts> count{};
    };

    /// <summary>What a band of an AC scan reports to the stitch.</summary>
    struct BandResult {
        std::array<std::int32_t, HuffmanSymbols> histogram{};           // symbols of the band part (refinement: without an open EOB token)
        bool                                     active        = false; // AC first: a block with a symbol; refinement: a newly significant coefficient
        std::int32_t                             leadingBlocks = 0;     // blocks before that (zero blocks, or refinement EOB blocks)
        HeapArray<std::uint8_t>                  leadingBits;           // refinement: correction bits of each leading block
        std::int32_t                             exitEobRun     = 0;    // pending run at the band end (after the first active block)
        std::int32_t                             exitEobRefbits = 0;    // refinement: its correction bits
    };

    /// <summary>Code and length (including the extra bits) per symbol (HuffmanCodeTable).</summary>
    struct CodeTable {
        std::array<std::int32_t, HuffmanSymbols> depth{};
        std::array<std::int32_t, HuffmanSymbols> code{};
    };

    /// <summary>The optimized Huffman tables (OptimizeHuffmanCodes, InitEntropyCoder).</summary>
    struct HuffmanCoding {
        std::int32_t                          tableCount = 0;
        std::array<HuffmanTable, MaxContexts> tables{};
        std::array<std::uint8_t, MaxContexts> slotIds{};    // DHT class/index byte
        std::array<std::uint8_t, MaxContexts> contextMap{}; // context -> table
        std::array<CodeTable, MaxContexts>    codes{};
    };

    // ------------------------------------------------------------------ Tokenize.cpp

    /// <summary>TokenizeScan for the whole scan scanIndex, as one part; false if out of memory. jpegli: TokenizeScan() (entropy_coding.cc).</summary>
    bool TokenizeScan(const EncoderSetup& setup, const Coefficients& coefficients, std::int32_t scanIndex, ScanTokens& tokens) noexcept;

    /// <summary>BuildHistograms for the tokens of a scan tokenized as a whole, added to histograms. jpegli: BuildHistograms() (entropy_coding.cc).</summary>
    void AddScanHistograms(const EncoderSetup& setup, std::int32_t scanIndex, const ScanTokens& tokens, Histograms& histograms) noexcept;

    // ------------------------------------------------------------------ HuffmanCodes.cpp

    /// <summary>ClusterJpegHistograms, BuildJpegHuffmanTable, BuildHuffmanCodeTable. jpegli: OptimizeHuffmanCodes() (entropy_coding.cc).</summary>
    void OptimizeHuffmanCodes(const EncoderSetup& setup, const Histograms& histograms, HuffmanCoding& coding) noexcept;

    // ------------------------------------------------------------------ ScanWriter.cpp

    /// <summary>The entropy-coded data of one scan, byte-aligned; false on a missing code or out of memory. jpegli: WriteScanData() (bitstream.cc).</summary>
    bool WriteScanData(const EncoderSetup& setup, std::int32_t scanIndex, const ScanTokens& tokens, const HuffmanCoding& coding, ByteBuffer& output) noexcept;

    /// <summary>
    /// The scan header: the DHT tables this scan introduces and the SOS.
    /// nextDhtIndex is jpegli's next_dht_index and must start at 0 for scan 0.
    /// jpegli: WriteScanHeader() (bitstream.cc).
    /// </summary>
    bool WriteScanHeader(const EncoderSetup& setup, std::int32_t scanIndex, const HuffmanCoding& coding, std::int32_t& nextDhtIndex,
                         ByteBuffer& output) noexcept;

    // ------------------------------------------------------------------ BandStitch.cpp

    /// <summary>Whether the scan can be tokenized in bands (the single-component AC scans).</summary>
    bool CanSplitScan(const EncoderSetup& setup, std::int32_t scanIndex) noexcept;

    /// <summary>MCU rows of a DC scan, the unit of its bands.</summary>
    std::int32_t DcScanMcuRows(const EncoderSetup& setup, std::int32_t scanIndex) noexcept;

    /// <summary>
    /// Tokenizes the MCU rows [mcuRowBegin, mcuRowEnd) of a DC scan into part and adds its symbols
    /// to histograms. The bands of a DC scan are independent: each predicts from the last DC value
    /// of the rows before it, so the parts in order are the scan's tokens. False if out of memory.
    /// </summary>
    bool TokenizeDcBand(const EncoderSetup& setup, const Coefficients& coefficients, std::int32_t scanIndex, std::int32_t mcuRowBegin, std::int32_t mcuRowEnd,
                        TokenPart& part, Histograms& histograms) noexcept;

    /// <summary>Block rows of the scan's component, the unit of its bands.</summary>
    std::int32_t ScanBlockRows(const EncoderSetup& setup, std::int32_t scanIndex) noexcept;

    /// <summary>
    /// Tokenizes the block rows [rowBegin, rowEnd) of an AC scan into part, with the state of a
    /// fresh scan from the band's first active block on; result describes the rest for the stitch.
    /// </summary>
    bool TokenizeScanBand(const EncoderSetup& setup, const Coefficients& coefficients, std::int32_t scanIndex, std::int32_t rowBegin, std::int32_t rowEnd,
                          TokenPart& part, BandResult& result) noexcept;

    /// <summary>
    /// Joins the bands of an AC scan: tokens.parts holds 2 * bands + 1 parts, the band parts at the
    /// odd indices already filled; the stitch fills the prefix parts (even indices) and the final
    /// part with the EOB tokens that depend on the preceding bands and adds the scan's symbols to
    /// histograms. False if out of memory.
    /// </summary>
    bool StitchScanBands(const EncoderSetup& setup, std::int32_t scanIndex, std::int32_t bands, ScanTokens& tokens, const BandResult* results,
                         Histograms& histograms) noexcept;

    // ------------------------------------------------------------------ ChunkSplice.cpp

    /// <summary>Where a chunk of a refinement scan starts reading the correction-bit and run-length streams.</summary>
    struct ChunkCursor {
        std::int32_t refPart  = 0;
        std::size_t  refIndex = 0;
        std::int32_t runPart  = 0;
        std::size_t  runIndex = 0;
    };

    /// <summary>The plain bit stream of a chunk of a scan (no byte stuffing, no padding).</summary>
    struct ChunkBits {
        ByteBuffer   bytes; // big-endian, with readable slack after the last word
        std::int64_t bitCount = 0;
    };

    /// <summary>Correction bits and run lengths that the refinement tokens of the parts [partBegin, partEnd) read.</summary>
    void CountRefinementStreams(const ScanTokens& tokens, std::int32_t partBegin, std::int32_t partEnd, std::int64_t& refBits, std::int64_t& runs) noexcept;

    /// <summary>The cursor after refBits correction bits and runs run lengths of the scan.</summary>
    ChunkCursor LocateRefinementStreams(const ScanTokens& tokens, std::int64_t refBits, std::int64_t runs) noexcept;

    /// <summary>
    /// Writes the tokens of the parts [partBegin, partEnd) of a scan as a plain bit stream; a
    /// refinement scan starts reading its streams at cursor. False on a missing code or out of memory.
    /// </summary>
    bool WriteScanChunk(const EncoderSetup& setup, std::int32_t scanIndex, const ScanTokens& tokens, std::int32_t partBegin, std::int32_t partEnd,
                        const ChunkCursor& cursor, const HuffmanCoding& coding, ChunkBits& out) noexcept;

    /// <summary>
    /// The entropy-coded bytes of a scan that start in chunk (the bytes whose first bit it holds),
    /// with byte stuffing; the last chunk pads the last byte with 1 bits. The outputs of all chunks
    /// in order equal WriteScanData of the whole scan. False if out of memory.
    /// </summary>
    bool SpliceScanChunk(const ChunkBits* chunks, std::int32_t chunkCount, std::int32_t chunk, ByteBuffer& output) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
