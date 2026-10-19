// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

// New in ctJpeg, not in jpegli: a scan written in chunks in parallel. Every chunk writes its parts
// as a plain bit stream (no byte stuffing, no padding); the splice cuts the joined stream into
// the stuffed bytes that WriteScanData writes for the whole scan. A chunk of a refinement scan
// starts reading the correction-bit and run-length streams where the chunks before it stopped.

#include "Entropy.h"

#include <cstddef>
#include <cstdint>

#include <intrin.h>

#include "BitPacking.h"
#include "ScanWriter.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    namespace {

        // BitWriter without byte stuffing and padding: the plain bit stream of a chunk, packed
        // big-endian, for SpliceScanChunk.
        class RawBitWriter
        {
        public:
            explicit RawBitWriter(ByteBuffer& output) noexcept : output_(output) {}

            void WriteBits(std::int32_t nbits, std::uint64_t bits) noexcept
            {
                PackBits(putBuffer_, freeBits_, healthy_, nbits, bits, [this]() noexcept { Store(putBuffer_); });
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
                StoreBigEndian64(word, output_.Data() + output_.Size());
                output_.SetSize(output_.Size() + 8);
            }

            ByteBuffer&   output_;
            std::uint64_t putBuffer_ = 0;
            std::int32_t  freeBits_  = 64;
            bool          healthy_   = true;
        };

    } // namespace

    void CountRefinementStreams(const ScanTokens& tokens, std::int32_t partBegin, std::int32_t partEnd, std::int64_t& refBits, std::int64_t& runs) noexcept
    {
        std::int64_t bits = 0, count = 0;
        for (std::int32_t p = partBegin; p < partEnd; ++p) {
            const TokenPart& part = tokens.parts[p];
            for (std::size_t i = 0; i < part.refTokenCount; ++i) {
                const RefToken     t      = part.refTokens[i];
                const std::int32_t symbol = t.symbol & RefSymbolMask;
                const std::int32_t r      = symbol >> 4;
                bits += t.refbits;
                count += (symbol & 1) == 0 && r > 0 && r < 15 ? 1 : 0; // an EOB run with extra bits, as WriteTokenParts reads it
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
        WriteTokenParts(setup, scanIndex, tokens, partBegin, partEnd, cursor, coding, bw);
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
        if (!output.Reserve(static_cast<std::size_t>(j1 - j0) * 2)) // every byte may be stuffed
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

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
