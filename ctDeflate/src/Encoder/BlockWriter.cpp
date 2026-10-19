//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#include "BlockWriter.h"

#include "Huffman.h"

#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    namespace {

        // libdeflate: deflate_make_huffman_codes() (deflate_compress.c)
        void MakeHuffmanCodes(const DeflateFreqs& freqs, DeflateCodes& codes) noexcept
        {
            MakeHuffmanCode(NumLitlenSyms, MaxLitlenCodewordLen, freqs.litlen, codes.LitlenLens(), codes.litlenCodewords);
            MakeHuffmanCode(NumOffsetSyms, MaxOffsetCodewordLen, freqs.offset, codes.OffsetLens(), codes.offsetCodewords);
        }

        /// <summary>
        /// libdeflate: deflate_compute_precode_items() (deflate_compress.c). Run-length codes the codeword lengths with
        /// precode symbols 16 (repeat previous 3..6), 17 (3..10 zeros) and 18 (11..138 zeros). Items: symbol | extra
        /// bits << 5. Returns the number of items.
        /// </summary>
        std::uint32_t ComputePrecodeItems(const std::uint8_t* lens, std::uint32_t numLens, std::uint32_t* precodeFreqs, std::uint32_t* items) noexcept
        {
            std::memset(precodeFreqs, 0, NumPrecodeSyms * sizeof(precodeFreqs[0]));

            std::uint32_t* item     = items;
            std::uint32_t  runStart = 0;
            do {
                const std::uint8_t len    = lens[runStart];
                std::uint32_t      runEnd = runStart;
                do {
                    ++runEnd;
                } while (runEnd != numLens && len == lens[runEnd]);

                if (len == 0) {
                    while (runEnd - runStart >= 11) {
                        const std::uint32_t extra = runEnd - runStart - 11 < 0x7F ? runEnd - runStart - 11 : 0x7F;
                        ++precodeFreqs[18];
                        *item++ = 18 | (extra << 5);
                        runStart += 11 + extra;
                    }
                    if (runEnd - runStart >= 3) {
                        const std::uint32_t extra = runEnd - runStart - 3 < 0x7 ? runEnd - runStart - 3 : 0x7;
                        ++precodeFreqs[17];
                        *item++ = 17 | (extra << 5);
                        runStart += 3 + extra;
                    }
                } else if (runEnd - runStart >= 4) {
                    ++precodeFreqs[len];
                    *item++ = len;
                    ++runStart;
                    do {
                        const std::uint32_t extra = runEnd - runStart - 3 < 0x3 ? runEnd - runStart - 3 : 0x3;
                        ++precodeFreqs[16];
                        *item++ = 16 | (extra << 5);
                        runStart += 3 + extra;
                    } while (runEnd - runStart >= 3);
                }

                while (runStart != runEnd) {
                    ++precodeFreqs[len];
                    *item++ = len;
                    ++runStart;
                }
            } while (runStart != numLens);

            return static_cast<std::uint32_t>(item - items);
        }

    } // namespace

    // libdeflate: deflate_init_static_codes() (deflate_compress.c)
    BlockWriter::BlockWriter() noexcept
    {
        // The static codes come from frequencies 2^(9 - length) through the same code builder.
        DeflateFreqs freqs{};
        std::uint32_t i = 0;
        for (; i < 144; ++i)
            freqs.litlen[i] = 1 << (9 - 8);
        for (; i < 256; ++i)
            freqs.litlen[i] = 1 << (9 - 9);
        for (; i < 280; ++i)
            freqs.litlen[i] = 1 << (9 - 7);
        for (; i < 288; ++i)
            freqs.litlen[i] = 1 << (9 - 8);
        for (i = 0; i < 32; ++i)
            freqs.offset[i] = 1 << (5 - 5);
        MakeHuffmanCodes(freqs, staticCodes_);
    }

    // libdeflate: deflate_finish_block() (deflate_compress.c)
    void BlockWriter::FinishBlock(BitWriter& os, const std::uint8_t* blockBegin, std::uint32_t blockLength, const DeflateSequence* sequences,
                                  DeflateFreqs& freqs, bool isFinalBlock) noexcept
    {
        ++freqs.litlen[EndOfBlock];
        MakeHuffmanCodes(freqs, codes_);
        FlushBlock(os, blockBegin, blockLength, sequences, freqs, isFinalBlock);
    }

    // libdeflate: deflate_precompute_huffman_header() (deflate_compress.c)
    void BlockWriter::PrecomputeHuffmanHeader() noexcept
    {
        auto& p = precode_;
        for (p.numLitlenSyms = NumLitlenSyms; p.numLitlenSyms > 257; --p.numLitlenSyms)
            if (codes_.LitlenLens()[p.numLitlenSyms - 1] != 0)
                break;
        for (p.numOffsetSyms = NumOffsetSyms; p.numOffsetSyms > 1; --p.numOffsetSyms)
            if (codes_.OffsetLens()[p.numOffsetSyms - 1] != 0)
                break;

        // Make the used litlen and offset lengths contiguous for the run-length coding.
        if (p.numLitlenSyms != NumLitlenSyms)
            std::memmove(codes_.lens + p.numLitlenSyms, codes_.lens + NumLitlenSyms, p.numOffsetSyms);

        p.numItems = ComputePrecodeItems(codes_.lens, p.numLitlenSyms + p.numOffsetSyms, p.freqs, p.items);
        MakeHuffmanCode(NumPrecodeSyms, MaxPreCodewordLen, p.freqs, p.lens, p.codewords);

        for (p.numExplicitLens = NumPrecodeSyms; p.numExplicitLens > 4; --p.numExplicitLens)
            if (p.lens[PrecodeLensPermutation[p.numExplicitLens - 1]] != 0)
                break;

        if (p.numLitlenSyms != NumLitlenSyms)
            std::memmove(codes_.lens + NumLitlenSyms, codes_.lens + p.numLitlenSyms, p.numOffsetSyms);
    }

    // libdeflate: deflate_compute_full_len_codewords() (deflate_compress.c)
    void BlockWriter::ComputeFullLenCodewords(const DeflateCodes& codes) noexcept
    {
        for (std::uint32_t len = MinMatchLen; len <= MaxMatchLen; ++len) {
            const std::uint32_t slot      = LengthSlot(len);
            const std::uint32_t litlenSym = FirstLenSym + slot;
            const std::uint32_t extraBits = len - LengthSlotBase[slot];
            lengthCodewords_[len]         = codes.litlenCodewords[litlenSym] | (extraBits << codes.LitlenLens()[litlenSym]);
            lengthLens_[len]              = static_cast<std::uint8_t>(codes.LitlenLens()[litlenSym] + ExtraLengthBits[slot]);
        }
    }

    // libdeflate: deflate_flush_block(), part 1: the block costs (deflate_compress.c)
    void BlockWriter::ComputeCosts(const DeflateFreqs& freqs, std::uint32_t blockLength, BlockCosts& costs) noexcept
    {
        std::uint32_t dynamicCost = 3; // block header
        std::uint32_t staticCost  = 3;

        PrecomputeHuffmanHeader();

        dynamicCost += 5 + 5 + 4 + 3 * precode_.numExplicitLens;
        for (std::uint32_t sym = 0; sym < NumPrecodeSyms; ++sym)
            dynamicCost += precode_.freqs[sym] * (ExtraPrecodeBits[sym] + precode_.lens[sym]);

        std::uint32_t sym = 0;
        for (; sym < 144; ++sym) {
            dynamicCost += freqs.litlen[sym] * codes_.LitlenLens()[sym];
            staticCost += freqs.litlen[sym] * 8;
        }
        for (; sym < 256; ++sym) {
            dynamicCost += freqs.litlen[sym] * codes_.LitlenLens()[sym];
            staticCost += freqs.litlen[sym] * 9;
        }

        dynamicCost += codes_.LitlenLens()[EndOfBlock];
        staticCost += 7;

        for (sym = FirstLenSym; sym < FirstLenSym + sizeof(ExtraLengthBits); ++sym) {
            const std::uint32_t extra = ExtraLengthBits[sym - FirstLenSym];
            dynamicCost += freqs.litlen[sym] * (extra + codes_.LitlenLens()[sym]);
            staticCost += freqs.litlen[sym] * (extra + staticCodes_.LitlenLens()[sym]);
        }

        for (sym = 0; sym < sizeof(ExtraOffsetBits); ++sym) {
            const std::uint32_t extra = ExtraOffsetBits[sym];
            dynamicCost += freqs.offset[sym] * (extra + codes_.OffsetLens()[sym]);
            staticCost += freqs.offset[sym] * (extra + 5);
        }

        costs.dynamicCost          = dynamicCost;
        costs.staticCost           = staticCost;
        costs.uncompressedBaseCost = 3 + 32 + 40 * ((blockLength + 0xFFFE) / 0xFFFF - 1) + 8 * blockLength;
    }

    // libdeflate: deflate_flush_block(), part 3a: uncompressed block(s) (deflate_compress.c)
    void BlockWriter::WriteStored(BitWriter& os, const std::uint8_t* blockBegin, std::size_t blockLength, bool isFinalBlock) noexcept
    {
        // Uncompressed block(s), at most 65535 bytes each.
        const std::uint8_t*       inNext = blockBegin;
        const std::uint8_t* const inEnd  = blockBegin + blockLength;
        do {
            std::uint32_t bfinal = 0;
            std::uint32_t len    = 0xFFFF;
            if (static_cast<std::size_t>(inEnd - inNext) <= 0xFFFF) {
                bfinal = isFinalBlock ? 1 : 0;
                len    = static_cast<std::uint32_t>(inEnd - inNext);
            }
            os.AddBits(bfinal, 1);
            os.AddBits(BlockTypeUncompressed, 2);
            os.Flush();
            os.AlignToByte();
            std::uint8_t* out = os.Next();
            out[0]            = static_cast<std::uint8_t>(len);
            out[1]            = static_cast<std::uint8_t>(len >> 8);
            out[2]            = static_cast<std::uint8_t>(~len);
            out[3]            = static_cast<std::uint8_t>(~len >> 8);
            std::memcpy(out + 4, inNext, len);
            os.SetNext(out + 4 + len);
            inNext += len;
        } while (inNext != inEnd);
    }

    // New in ctDeflate, not in libdeflate (parallel encoding)
    void BlockWriter::AppendBits(BitWriter& os, const std::uint8_t* bits, std::uint64_t bitCount) noexcept
    {
        // 7 bytes at a time: with at most 7 pending bits they fit the 64-bit buffer.
        while (bitCount >= 56) {
            std::uint64_t word;
            std::memcpy(&word, bits, 8); // the encoded block has slack bytes after its bits
            os.AddBits(word & 0x00FFFFFFFFFFFFFFull, 56);
            os.Flush();
            bits += 7;
            bitCount -= 56;
        }
        while (bitCount != 0) {
            const auto n = static_cast<std::uint32_t>(bitCount < 8 ? bitCount : 8);
            os.AddBits(*bits & ((1u << n) - 1), n);
            ++bits;
            bitCount -= n;
        }
        os.Flush();
    }

    // libdeflate: deflate_flush_block(), part 2: choice of the cheapest block type (deflate_compress.c)
    void BlockWriter::FlushBlock(BitWriter& os, const std::uint8_t* blockBegin, std::uint32_t blockLength, const DeflateSequence* sequences,
                                 const DeflateFreqs& freqs, bool isFinalBlock) noexcept
    {
        BlockCosts costs;
        ComputeCosts(freqs, blockLength, costs);

        // The alignment bits of an uncompressed block depend on where it starts in the bitstream.
        const std::uint32_t uncompressedCost = costs.UncompressedCost(os.BitCount());
        auto                bestCost         = costs.staticCost < uncompressedCost ? costs.staticCost : uncompressedCost;
        if (costs.dynamicCost < bestCost)
            bestCost = costs.dynamicCost;

        if (bestCost == uncompressedCost)
            WriteStored(os, blockBegin, blockLength, isFinalBlock);
        else
            WriteCompressed(os, blockBegin, sequences, isFinalBlock, bestCost == costs.staticCost);
    }

    // New in ctDeflate (parallel encoding); follows libdeflate: deflate_finish_block() (deflate_compress.c)
    void BlockWriter::Encode(const std::uint8_t* blockBegin, std::uint32_t blockLength, const DeflateSequence* sequences, DeflateFreqs& freqs,
                             bool isFinalBlock, EncodedBlock& out)
    {
        ++freqs.litlen[EndOfBlock];
        MakeHuffmanCodes(freqs, codes_);
        BlockCosts costs;
        ComputeCosts(freqs, blockLength, costs);

        const bool          useStatic = costs.staticCost <= costs.dynamicCost;
        const std::uint32_t best      = useStatic ? costs.staticCost : costs.dynamicCost;
        out.storedMask                = 0;
        for (std::uint32_t o = 0; o < 8; ++o)
            if (costs.UncompressedCost(o) <= best)
                out.storedMask |= static_cast<std::uint8_t>(1u << o);

        out.bitCount = 0;
        if (out.storedMask != 0xFF) {
            out.bits.resize(static_cast<std::size_t>(best / 8) + 16);
            BitWriter os(out.bits.data(), out.bits.data() + out.bits.size());
            WriteCompressed(os, blockBegin, sequences, isFinalBlock, useStatic);
            out.bitCount = static_cast<std::uint64_t>(os.Next() - out.bits.data()) * 8 + os.BitCount();
            os.AlignToByte();
        }
    }

    // libdeflate: deflate_flush_block(), part 3b: static/dynamic Huffman block with WRITE_MATCH() (deflate_compress.c)
    void BlockWriter::WriteCompressed(BitWriter& os, const std::uint8_t* blockBegin, const DeflateSequence* sequences, bool isFinalBlock,
                                      bool useStatic) noexcept
    {
        const DeflateCodes* codes;
        if (useStatic) {
            codes = &staticCodes_;
            os.AddBits(isFinalBlock ? 1 : 0, 1);
            os.AddBits(BlockTypeStaticHuffman, 2);
            os.Flush();
        } else {
            codes = &codes_;
            os.AddBits(isFinalBlock ? 1 : 0, 1);
            os.AddBits(BlockTypeDynamicHuffman, 2);
            os.AddBits(precode_.numLitlenSyms - 257, 5);
            os.AddBits(precode_.numOffsetSyms - 1, 5);
            os.AddBits(precode_.numExplicitLens - 4, 4);
            os.Flush();
            for (std::uint32_t i = 0; i < precode_.numExplicitLens; ++i) {
                os.AddBits(precode_.lens[PrecodeLensPermutation[i]], 3);
                os.Flush();
            }
            for (std::uint32_t i = 0; i < precode_.numItems; ++i) {
                const std::uint32_t item       = precode_.items[i];
                const std::uint32_t precodeSym = item & 0x1F;
                os.AddBits(precode_.codewords[precodeSym], precode_.lens[precodeSym]);
                os.AddBits(item >> 5, ExtraPrecodeBits[precodeSym]);
                os.Flush();
            }
        }

        ComputeFullLenCodewords(*codes);

        const std::uint8_t*  inNext    = blockBegin;
        const std::uint32_t* litlenCw  = codes->litlenCodewords;
        const std::uint8_t*  litlenLen = codes->LitlenLens();
        for (const DeflateSequence* seq = sequences;; ++seq) {
            std::uint32_t       litrunlen = seq->litrunlenAndLength & SeqLitrunlenMask;
            const std::uint32_t length    = seq->litrunlenAndLength >> SeqLengthShift;

            for (; litrunlen >= 4; litrunlen -= 4) {
                std::uint32_t lit = *inNext++;
                os.AddBits(litlenCw[lit], litlenLen[lit]);
                lit = *inNext++;
                os.AddBits(litlenCw[lit], litlenLen[lit]);
                lit = *inNext++;
                os.AddBits(litlenCw[lit], litlenLen[lit]);
                lit = *inNext++;
                os.AddBits(litlenCw[lit], litlenLen[lit]);
                os.Flush();
            }
            if (litrunlen != 0) {
                while (litrunlen-- != 0) {
                    const std::uint32_t lit = *inNext++;
                    os.AddBits(litlenCw[lit], litlenLen[lit]);
                }
                os.Flush();
            }

            if (length == 0)
                break;

            const std::uint32_t offsetSlot = seq->offsetSlot;
            os.AddBits(lengthCodewords_[length], lengthLens_[length]);
            os.AddBits(codes->offsetCodewords[offsetSlot], codes->OffsetLens()[offsetSlot]);
            os.AddBits(seq->offset - OffsetSlotBase[offsetSlot], ExtraOffsetBits[offsetSlot]);
            os.Flush();
            inNext += length;
        }

        os.AddBits(litlenCw[EndOfBlock], litlenLen[EndOfBlock]);
        os.Flush();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
