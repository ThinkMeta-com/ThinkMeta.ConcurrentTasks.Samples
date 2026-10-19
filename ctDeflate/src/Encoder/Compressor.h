//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include "BlockRules.h"
#include "BlockWriter.h"
#include "Matchfinder.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>
    /// New in ctDeflate, not in libdeflate. Lets a caller stream the input in and the output out while the compressor
    /// runs: called around every block, with positions relative to the input and the output of CompressDeflate.
    /// </summary>
    class ICompressProgress
    {
    public:
        /// <summary>The next block starts at inPos and reads no input at or after inputEnd; output goes on at outPos.</summary>
        virtual void BeforeBlock(std::size_t inPos, std::size_t inputEnd, std::size_t outPos) = 0;
        /// <summary>The block ended at inPos; the output below outEnd is final.</summary>
        virtual void AfterBlock(std::size_t inPos, std::size_t outEnd) = 0;

    protected:
        ~ICompressProgress() = default;
    };

    /// <summary>
    /// libdeflate: struct libdeflate_compressor (deflate_compress.c). Sequential DEFLATE compressor, bit-identical to
    /// libdeflate 1.26 for levels 0..9 (none, fastest, greedy, lazy, lazy2). Levels 10..12 (near-optimal parsing) are
    /// not supported.
    /// </summary>
    class Compressor
    {
    public:
        static constexpr std::int32_t MaxLevel = LevelParams::MaxLevel;

        /// <summary>libdeflate: libdeflate_alloc_compressor() (deflate_compress.c). Creates a compressor; level outside
        /// 0..9 gives a null pointer.</summary>
        static std::unique_ptr<Compressor> Create(std::int32_t level);

        std::int32_t Level() const noexcept { return level_; }

        /// <summary>CompressDeflate reports every block to progress (null: no reports).</summary>
        void SetProgress(ICompressProgress* progress) noexcept { progress_ = progress; }

        /// <summary>libdeflate: libdeflate_deflate_compress_bound() (deflate_compress.c). Upper bound of the raw
        /// DEFLATE size.</summary>
        static std::size_t DeflateBound(std::size_t inSize) noexcept;
        /// <summary>libdeflate: libdeflate_gzip_compress_bound() (gzip_compress.c). Upper bound of the gzip size
        /// including 8 bytes of slack for the bitstream's word stores.</summary>
        static std::size_t GzipBound(std::size_t inSize) noexcept { return DeflateBound(inSize) + 18 + 8; }
        // libdeflate: libdeflate_zlib_compress_bound() (zlib_compress.c), plus 8 bytes of slack
        static std::size_t ZlibBound(std::size_t inSize) noexcept { return DeflateBound(inSize) + 6 + 8; }

        /// <summary>libdeflate: libdeflate_deflate_compress() (deflate_compress.c). Raw DEFLATE stream. out must hold
        /// DeflateBound(inSize) + 8 bytes. Returns the size.</summary>
        std::size_t CompressDeflate(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out) noexcept;
        /// <summary>libdeflate: libdeflate_gzip_compress() (gzip_compress.c). gzip stream: MTIME 0, XFL by level, OS 255.
        /// out: GzipBound(inSize).</summary>
        std::size_t CompressGzip(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out) noexcept;
        /// <summary>libdeflate: libdeflate_zlib_compress() (zlib_compress.c). zlib stream. out: ZlibBound(inSize).</summary>
        std::size_t CompressZlib(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out) noexcept;

    private:
        explicit Compressor(std::int32_t level) noexcept;

        void CompressFastest(const std::uint8_t* in, std::size_t inSize, BitWriter& os) noexcept;
        void CompressGreedy(const std::uint8_t* in, std::size_t inSize, BitWriter& os) noexcept;
        void CompressLazy(const std::uint8_t* in, std::size_t inSize, BitWriter& os, bool lazy2) noexcept;

        void          BeginSequences(DeflateSequence* first) noexcept;
        void          ChooseLiteral(std::uint32_t literal, bool gatherSplitStats, DeflateSequence* seq) noexcept;
        void          ChooseMatch(std::uint32_t length, std::uint32_t offset, bool gatherSplitStats, DeflateSequence** seqP) noexcept;
        bool          ShouldEndBlock(const std::uint8_t* inBlockBegin, const std::uint8_t* inNext, const std::uint8_t* inEnd) noexcept;
        std::uint32_t RecalculateMinMatchLen() const noexcept;

        /// <summary>New in ctDeflate, not in libdeflate. Reports the start of a block: it reads at most maxBlockLength
        /// bytes and its last matches beyond.</summary>
        void BeforeBlock(const std::uint8_t* in, std::size_t inSize, const std::uint8_t* inNext, std::size_t maxBlockLength, const BitWriter& os) noexcept;

        std::int32_t       level_;
        ICompressProgress* progress_ = nullptr;
        std::uint8_t*      out_      = nullptr; // output of the running CompressDeflate
        std::size_t        maxPassthroughSize_;
        std::uint32_t      maxSearchDepth_  = 0;
        std::uint32_t      niceMatchLength_ = 0;

        DeflateFreqs freqs_{};
        SplitStats   splitStats_{};
        BlockWriter  blockWriter_;

        std::unique_ptr<HcMatchfinder>     hc_;
        std::unique_ptr<HtMatchfinder>     ht_;
        std::unique_ptr<DeflateSequence[]> sequences_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
