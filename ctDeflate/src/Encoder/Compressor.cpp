//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#include "Compressor.h"

#include "BlockRules.h"
#include "Checksums.h"
#include "Gzip.h"

#include <bit>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    namespace {

        // libdeflate: adjust_max_and_nice_len() (deflate_compress.c)
        void AdjustMaxAndNiceLen(std::uint32_t* maxLen, std::uint32_t* niceLen, std::size_t remaining) noexcept
        {
            if (remaining < MaxMatchLen) {
                *maxLen = static_cast<std::uint32_t>(remaining);
                if (*niceLen > *maxLen)
                    *niceLen = *maxLen;
            }
        }

        /// <summary>libdeflate: deflate_compress_none() (deflate_compress.c). Level 0 and tiny inputs: uncompressed
        /// blocks only.</summary>
        std::size_t CompressNone(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out, ICompressProgress* progress) noexcept
        {
            std::uint8_t* outNext = out;
            if (inSize == 0) {
                *outNext++                  = 1 | (BlockTypeUncompressed << 1);
                const std::uint32_t lenNlen = 0xFFFF0000u;
                std::memcpy(outNext, &lenNlen, 4);
                return 5;
            }
            const std::uint8_t*       inNext = in;
            const std::uint8_t* const inEnd  = in + inSize;
            do {
                std::uint8_t  bfinal = 0;
                std::uint32_t len    = 0xFFFF;
                if (static_cast<std::size_t>(inEnd - inNext) <= 0xFFFF) {
                    bfinal = 1;
                    len    = static_cast<std::uint32_t>(inEnd - inNext);
                }
                if (progress != nullptr)
                    progress->BeforeBlock(static_cast<std::size_t>(inNext - in), static_cast<std::size_t>(inNext - in) + len,
                                          static_cast<std::size_t>(outNext - out));
                *outNext++ = static_cast<std::uint8_t>(bfinal | (BlockTypeUncompressed << 1));
                *outNext++ = static_cast<std::uint8_t>(len);
                *outNext++ = static_cast<std::uint8_t>(len >> 8);
                *outNext++ = static_cast<std::uint8_t>(~len);
                *outNext++ = static_cast<std::uint8_t>(~len >> 8);
                std::memcpy(outNext, inNext, len);
                outNext += len;
                inNext += len;
                if (progress != nullptr)
                    progress->AfterBlock(static_cast<std::size_t>(inNext - in), static_cast<std::size_t>(outNext - out));
            } while (inNext != inEnd);
            return static_cast<std::size_t>(outNext - out);
        }

    } // namespace

    // libdeflate: libdeflate_alloc_compressor() (deflate_compress.c)
    std::unique_ptr<Compressor> Compressor::Create(std::int32_t level)
    {
        if (level < 0 || level > MaxLevel)
            return nullptr;
        return std::unique_ptr<Compressor>(new Compressor(level));
    }

    // libdeflate: libdeflate_alloc_compressor_ex() (deflate_compress.c)
    Compressor::Compressor(std::int32_t level) noexcept : level_(level)
    {
        const LevelParams params = LevelParams::For(level);
        maxPassthroughSize_      = params.maxPassthroughSize;
        maxSearchDepth_          = params.maxSearchDepth;
        niceMatchLength_         = params.niceMatchLength;

        if (level >= 2) {
            hc_        = std::make_unique<HcMatchfinder>();
            sequences_ = std::make_unique<DeflateSequence[]>(SeqStoreLength + 1);
        } else if (level == 1) {
            ht_        = std::make_unique<HtMatchfinder>();
            sequences_ = std::make_unique<DeflateSequence[]>(FastSeqStoreLength + 1);
        }
    }

    // libdeflate: libdeflate_deflate_compress_bound() (deflate_compress.c)
    std::size_t Compressor::DeflateBound(std::size_t inSize) noexcept
    {
        // Compressed blocks are only used when cheaper than uncompressed ones, so the bound is that of uncompressed
        // blocks of at least MinBlockLength bytes: 5 bytes of overhead each.
        std::size_t maxBlocks = (inSize + MinBlockLength - 1) / MinBlockLength;
        if (maxBlocks < 1)
            maxBlocks = 1;
        return 5 * maxBlocks + inSize;
    }

    // libdeflate: libdeflate_deflate_compress() (deflate_compress.c)
    std::size_t Compressor::CompressDeflate(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out) noexcept
    {
        if (inSize <= maxPassthroughSize_)
            return CompressNone(in, inSize, out, progress_);

        out_ = out;
        BitWriter os(out, out + DeflateBound(inSize) + 8);
        switch (level_) {
            case 1: CompressFastest(in, inSize, os); break;
            case 2:
            case 3:
            case 4: CompressGreedy(in, inSize, os); break;
            case 5:
            case 6:
            case 7: CompressLazy(in, inSize, os, false); break;
            default: CompressLazy(in, inSize, os, true); break;
        }
        os.AlignToByte();
        return static_cast<std::size_t>(os.Next() - out);
    }

    // libdeflate: libdeflate_gzip_compress() (gzip_compress.c)
    std::size_t Compressor::CompressGzip(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out) noexcept
    {
        std::size_t pos = WriteGzipHeader(level_, out);
        pos += CompressDeflate(in, inSize, out + pos);
        pos += WriteGzipTrailer(Crc32(0, in, inSize), inSize, out + pos);
        return pos;
    }

    // libdeflate: libdeflate_zlib_compress() (zlib_compress.c)
    std::size_t Compressor::CompressZlib(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out) noexcept
    {
        std::uint32_t       hdr       = (8u << 8) | (7u << 12);
        const std::uint32_t levelHint = level_ < 2 ? 0 : level_ < 6 ? 1 : level_ < 8 ? 2 : 3;
        hdr |= levelHint << 6;
        hdr |= 31 - (hdr % 31);

        std::uint8_t* p = out;
        *p++            = static_cast<std::uint8_t>(hdr >> 8);
        *p++            = static_cast<std::uint8_t>(hdr);
        p += CompressDeflate(in, inSize, p);

        const std::uint32_t adler = Adler32(1, in, inSize);
        for (std::int32_t i = 3; i >= 0; --i)
            *p++ = static_cast<std::uint8_t>(adler >> (8 * i));
        return static_cast<std::size_t>(p - out);
    }

    // libdeflate: deflate_begin_sequences(), deflate_reset_symbol_frequencies() (deflate_compress.c)
    void Compressor::BeginSequences(DeflateSequence* first) noexcept
    {
        std::memset(&freqs_, 0, sizeof(freqs_));
        first->litrunlenAndLength = 0;
    }

    // libdeflate: deflate_choose_literal(), observe_literal() (deflate_compress.c)
    void Compressor::ChooseLiteral(std::uint32_t literal, bool gatherSplitStats, DeflateSequence* seq) noexcept
    {
        ++freqs_.litlen[literal];
        if (gatherSplitStats) {
            ++splitStats_.newObservations[((literal >> 5) & 0x6) | (literal & 1)];
            ++splitStats_.numNewObservations;
        }
        ++seq->litrunlenAndLength;
    }

    // libdeflate: deflate_choose_match(), observe_match() (deflate_compress.c)
    void Compressor::ChooseMatch(std::uint32_t length, std::uint32_t offset, bool gatherSplitStats, DeflateSequence** seqP) noexcept
    {
        DeflateSequence*    seq        = *seqP;
        const std::uint32_t lengthSlot = LengthSlot(length);
        const std::uint32_t offsetSlot = OffsetSlot(offset);

        ++freqs_.litlen[FirstLenSym + lengthSlot];
        ++freqs_.offset[offsetSlot];
        if (gatherSplitStats) {
            ++splitStats_.newObservations[NumLiteralObservations + (length >= 9 ? 1 : 0)];
            ++splitStats_.numNewObservations;
        }

        seq->litrunlenAndLength |= length << SeqLengthShift;
        seq->offset     = static_cast<std::uint16_t>(offset);
        seq->offsetSlot = static_cast<std::uint16_t>(offsetSlot);

        ++seq;
        seq->litrunlenAndLength = 0;
        *seqP                   = seq;
    }

    // libdeflate: should_end_block() (deflate_compress.c)
    bool Compressor::ShouldEndBlock(const std::uint8_t* inBlockBegin, const std::uint8_t* inNext, const std::uint8_t* inEnd) noexcept
    {
        if (!EndBlockCheckDue(splitStats_, static_cast<std::size_t>(inNext - inBlockBegin), static_cast<std::size_t>(inEnd - inNext)))
            return false;
        return DoEndBlockCheck(splitStats_, static_cast<std::uint32_t>(inNext - inBlockBegin));
    }

    // New in ctDeflate, not in libdeflate
    void Compressor::BeforeBlock(const std::uint8_t* in, std::size_t inSize, const std::uint8_t* inNext, std::size_t maxBlockLength,
                                 const BitWriter& os) noexcept
    {
        // Past the block's limit a block reads at most its last iterations: a lazy2 look two positions ahead, a match of
        // up to 258 bytes, word reads of the match extension, the next hashes.
        constexpr std::size_t Reach = 4096;
        const auto            pos   = static_cast<std::size_t>(inNext - in);
        const std::size_t     limit = pos + maxBlockLength + MinBlockLength + Reach;
        progress_->BeforeBlock(pos, limit < inSize ? limit : inSize, static_cast<std::size_t>(os.Next() - out_));
    }

    // libdeflate: recalculate_min_match_len() (deflate_compress.c)
    std::uint32_t Compressor::RecalculateMinMatchLen() const noexcept
    {
        return Deflate::RecalculateMinMatchLen(freqs_.litlen, maxSearchDepth_);
    }

    // libdeflate: deflate_compress_fastest() (deflate_compress.c)
    void Compressor::CompressFastest(const std::uint8_t* in, std::size_t inSize, BitWriter& os) noexcept
    {
        const std::uint8_t*       inNext    = in;
        const std::uint8_t* const inEnd     = in + inSize;
        const std::uint8_t*       inCurBase = inNext;
        std::uint32_t             maxLen    = MaxMatchLen;
        std::uint32_t             niceLen   = niceMatchLength_ < maxLen ? niceMatchLength_ : maxLen;
        std::uint32_t             nextHash  = 0;

        ht_->Init();
        do {
            const std::uint8_t* const inBlockBegin = inNext;
            if (progress_ != nullptr)
                BeforeBlock(in, inSize, inNext, FastSoftMaxBlockLength, os);
            const std::uint8_t* const inMaxBlockEnd = in + ChooseMaxBlockEnd(static_cast<std::size_t>(inNext - in), inSize, FastSoftMaxBlockLength);
            DeflateSequence*          seq           = sequences_.get();
            BeginSequences(seq);

            do {
                const auto remaining = static_cast<std::size_t>(inEnd - inNext);
                if (remaining < MaxMatchLen) {
                    maxLen = static_cast<std::uint32_t>(remaining);
                    if (maxLen < HtMatchfinder::RequiredBytes) {
                        do {
                            ChooseLiteral(*inNext++, false, seq);
                        } while (--maxLen != 0);
                        break;
                    }
                    if (niceLen > maxLen)
                        niceLen = maxLen;
                }
                std::uint32_t       offset;
                const std::uint32_t length = ht_->LongestMatch(&inCurBase, inNext, maxLen, niceLen, &nextHash, &offset);
                if (length != 0) {
                    ChooseMatch(length, offset, false, &seq);
                    ht_->SkipBytes(&inCurBase, inNext + 1, inEnd, length - 1, &nextHash);
                    inNext += length;
                } else {
                    ChooseLiteral(*inNext++, false, seq);
                }
            } while (inNext < inMaxBlockEnd && seq < &sequences_[FastSeqStoreLength]);

            blockWriter_.FinishBlock(os, inBlockBegin, static_cast<std::uint32_t>(inNext - inBlockBegin), sequences_.get(), freqs_, inNext == inEnd);
            if (progress_ != nullptr)
                progress_->AfterBlock(static_cast<std::size_t>(inNext - in), static_cast<std::size_t>(os.Next() - out_));
        } while (inNext != inEnd);
    }

    // libdeflate: deflate_compress_greedy() (deflate_compress.c)
    void Compressor::CompressGreedy(const std::uint8_t* in, std::size_t inSize, BitWriter& os) noexcept
    {
        const std::uint8_t*       inNext        = in;
        const std::uint8_t* const inEnd         = in + inSize;
        const std::uint8_t*       inCurBase     = inNext;
        std::uint32_t             maxLen        = MaxMatchLen;
        std::uint32_t             niceLen       = niceMatchLength_ < maxLen ? niceMatchLength_ : maxLen;
        std::uint32_t             nextHashes[2] = {0, 0};

        hc_->Init();
        do {
            const std::uint8_t* const inBlockBegin = inNext;
            if (progress_ != nullptr)
                BeforeBlock(in, inSize, inNext, SoftMaxBlockLength, os);
            const std::uint8_t* const inMaxBlockEnd = in + ChooseMaxBlockEnd(static_cast<std::size_t>(inNext - in), inSize, SoftMaxBlockLength);
            DeflateSequence*          seq           = sequences_.get();

            std::memset(&splitStats_, 0, sizeof(splitStats_));
            BeginSequences(seq);
            const std::uint32_t minLen = CalculateMinMatchLen(inNext, static_cast<std::size_t>(inMaxBlockEnd - inNext), maxSearchDepth_);
            do {
                AdjustMaxAndNiceLen(&maxLen, &niceLen, static_cast<std::size_t>(inEnd - inNext));
                std::uint32_t       offset;
                const std::uint32_t length = hc_->LongestMatch(&inCurBase, inNext, minLen - 1, maxLen, niceLen, maxSearchDepth_, nextHashes, &offset);
                if (length >= minLen && (length > MinMatchLen || offset <= 4096)) {
                    ChooseMatch(length, offset, true, &seq);
                    hc_->SkipBytes(&inCurBase, inNext + 1, inEnd, length - 1, nextHashes);
                    inNext += length;
                } else {
                    ChooseLiteral(*inNext++, true, seq);
                }
            } while (inNext < inMaxBlockEnd && seq < &sequences_[SeqStoreLength] && !ShouldEndBlock(inBlockBegin, inNext, inEnd));

            blockWriter_.FinishBlock(os, inBlockBegin, static_cast<std::uint32_t>(inNext - inBlockBegin), sequences_.get(), freqs_, inNext == inEnd);
            if (progress_ != nullptr)
                progress_->AfterBlock(static_cast<std::size_t>(inNext - in), static_cast<std::size_t>(os.Next() - out_));
        } while (inNext != inEnd);
    }

    // libdeflate: deflate_compress_lazy_generic() (deflate_compress.c), called by deflate_compress_lazy() and
    // deflate_compress_lazy2()
    void Compressor::CompressLazy(const std::uint8_t* in, std::size_t inSize, BitWriter& os, bool lazy2) noexcept
    {
        const std::uint8_t*       inNext        = in;
        const std::uint8_t* const inEnd         = in + inSize;
        const std::uint8_t*       inCurBase     = inNext;
        std::uint32_t             maxLen        = MaxMatchLen;
        std::uint32_t             niceLen       = niceMatchLength_ < maxLen ? niceMatchLength_ : maxLen;
        std::uint32_t             nextHashes[2] = {0, 0};

        hc_->Init();
        do {
            const std::uint8_t* const inBlockBegin = inNext;
            if (progress_ != nullptr)
                BeforeBlock(in, inSize, inNext, SoftMaxBlockLength, os);
            const std::uint8_t* const inMaxBlockEnd    = in + ChooseMaxBlockEnd(static_cast<std::size_t>(inNext - in), inSize, SoftMaxBlockLength);
            const std::uint8_t*       nextRecalcMinLen = inNext + (inEnd - inNext < 10000 ? inEnd - inNext : 10000);
            DeflateSequence*          seq              = sequences_.get();

            std::memset(&splitStats_, 0, sizeof(splitStats_));
            BeginSequences(seq);
            std::uint32_t minLen = CalculateMinMatchLen(inNext, static_cast<std::size_t>(inMaxBlockEnd - inNext), maxSearchDepth_);
            do {
                std::uint32_t curLen;
                std::uint32_t curOffset;
                std::uint32_t nextLen;
                std::uint32_t nextOffset;

                if (inNext >= nextRecalcMinLen) {
                    minLen                = RecalculateMinMatchLen();
                    const auto toEnd      = inEnd - nextRecalcMinLen;
                    const auto blockSoFar = inNext - inBlockBegin;
                    nextRecalcMinLen += toEnd < blockSoFar ? toEnd : blockSoFar;
                }

                AdjustMaxAndNiceLen(&maxLen, &niceLen, static_cast<std::size_t>(inEnd - inNext));
                curLen = hc_->LongestMatch(&inCurBase, inNext, minLen - 1, maxLen, niceLen, maxSearchDepth_, nextHashes, &curOffset);
                if (curLen < minLen || (curLen == MinMatchLen && curOffset > 8192)) {
                    ChooseLiteral(*inNext++, true, seq);
                    continue;
                }
                ++inNext;

            haveCurMatch:
                // A very long match is taken immediately.
                if (curLen >= niceLen) {
                    ChooseMatch(curLen, curOffset, true, &seq);
                    hc_->SkipBytes(&inCurBase, inNext, inEnd, curLen - 1, nextHashes);
                    inNext += curLen - 1;
                    continue;
                }

                // A better match at the next position? (half the search depth there)
                AdjustMaxAndNiceLen(&maxLen, &niceLen, static_cast<std::size_t>(inEnd - inNext));
                nextLen = hc_->LongestMatch(&inCurBase, inNext++, curLen - 1, maxLen, niceLen, maxSearchDepth_ >> 1, nextHashes, &nextOffset);
                if (nextLen >= curLen && 4 * static_cast<std::int32_t>(nextLen - curLen) + (Bsr32(curOffset) - Bsr32(nextOffset)) > 2) {
                    ChooseLiteral(*(inNext - 2), true, seq);
                    curLen    = nextLen;
                    curOffset = nextOffset;
                    goto haveCurMatch;
                }

                if (lazy2) {
                    // Two positions ahead (a quarter of the search depth).
                    AdjustMaxAndNiceLen(&maxLen, &niceLen, static_cast<std::size_t>(inEnd - inNext));
                    nextLen = hc_->LongestMatch(&inCurBase, inNext++, curLen - 1, maxLen, niceLen, maxSearchDepth_ >> 2, nextHashes, &nextOffset);
                    if (nextLen >= curLen && 4 * static_cast<std::int32_t>(nextLen - curLen) + (Bsr32(curOffset) - Bsr32(nextOffset)) > 6) {
                        ChooseLiteral(*(inNext - 3), true, seq);
                        ChooseLiteral(*(inNext - 2), true, seq);
                        curLen    = nextLen;
                        curOffset = nextOffset;
                        goto haveCurMatch;
                    }
                    ChooseMatch(curLen, curOffset, true, &seq);
                    if (curLen > 3) {
                        hc_->SkipBytes(&inCurBase, inNext, inEnd, curLen - 3, nextHashes);
                        inNext += curLen - 3;
                    }
                } else {
                    ChooseMatch(curLen, curOffset, true, &seq);
                    hc_->SkipBytes(&inCurBase, inNext, inEnd, curLen - 2, nextHashes);
                    inNext += curLen - 2;
                }
            } while (inNext < inMaxBlockEnd && seq < &sequences_[SeqStoreLength] && !ShouldEndBlock(inBlockBegin, inNext, inEnd));

            blockWriter_.FinishBlock(os, inBlockBegin, static_cast<std::uint32_t>(inNext - inBlockBegin), sequences_.get(), freqs_, inNext == inEnd);
            if (progress_ != nullptr)
                progress_->AfterBlock(static_cast<std::size_t>(inNext - in), static_cast<std::size_t>(os.Next() - out_));
        } while (inNext != inEnd);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
