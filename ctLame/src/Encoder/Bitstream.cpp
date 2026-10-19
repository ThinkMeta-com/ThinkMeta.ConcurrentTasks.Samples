// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Bitstream.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "BitCount.h"
#include "Console.h"
#include "SessionConfig.h"
#include "Version.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    using Shared::Console;
    using Shared::Stream;

    namespace {

        constexpr std::int32_t CRC16_POLYNOMIAL = 0x8005;

        // putheader_bits: the next frame's header and side information
        void PutHeader(const SessionConfig& config, BitstreamState& bs) noexcept
        {
            std::memcpy(&bs.buf[bs.buf_byte_idx], bs.header[bs.w_ptr].buf.data(), config.sideinfo_len);
            bs.buf_byte_idx += config.sideinfo_len;
            bs.totbit += config.sideinfo_len * 8;
            bs.w_ptr = (bs.w_ptr + 1) & (MaxHeaderBuffers - 1);
        }

        // putbits2: the lowest j bits of val into the main data; a header whose position is
        // reached is inserted first
        void PutBits(const SessionConfig& config, BitstreamState& bs, std::int32_t val, std::int32_t j) noexcept
        {
            while (j > 0) {
                if (bs.buf_bit_idx == 0) {
                    bs.buf_bit_idx = 8;
                    bs.buf_byte_idx++;
                    if (bs.header[bs.w_ptr].write_timing == bs.totbit)
                        PutHeader(config, bs);
                    bs.buf[bs.buf_byte_idx] = 0;
                }
                const std::int32_t k = Min(j, bs.buf_bit_idx);
                j -= k;
                bs.buf_bit_idx -= k;
                bs.buf[bs.buf_byte_idx] |= static_cast<std::uint8_t>((val >> j) << bs.buf_bit_idx);
                bs.totbit += k;
            }
        }

        // putbits_noheaders: like PutBits, but outside the frame structure
        void PutBitsNoHeaders(BitstreamState& bs, std::int32_t val, std::int32_t j) noexcept
        {
            while (j > 0) {
                if (bs.buf_bit_idx == 0) {
                    bs.buf_bit_idx = 8;
                    bs.buf_byte_idx++;
                    bs.buf[bs.buf_byte_idx] = 0;
                }
                const std::int32_t k = Min(j, bs.buf_bit_idx);
                j -= k;
                bs.buf_bit_idx -= k;
                bs.buf[bs.buf_byte_idx] |= static_cast<std::uint8_t>((val >> j) << bs.buf_bit_idx);
                bs.totbit += k;
            }
        }

        // drain_into_ancillary: stuffing bits as ancillary data: "LAME", the short version,
        // then alternating bits
        void DrainIntoAncillary(const SessionConfig& config, BitstreamState& bs, std::int32_t remainingBits) noexcept
        {
            static const std::array<char, 4> Lame = {'L', 'A', 'M', 'E'};
            for (const char c : Lame) {
                if (remainingBits >= 8) {
                    PutBits(config, bs, c, 8);
                    remainingBits -= 8;
                }
            }
            if (remainingBits >= 32) {
                const char* const  version = GetLameShortVersion();
                const std::int32_t length  = static_cast<std::int32_t>(std::strlen(version));
                for (std::int32_t i = 0; i < length && remainingBits >= 8; ++i) {
                    remainingBits -= 8;
                    PutBits(config, bs, version[i], 8);
                }
            }
            for (; remainingBits >= 1; remainingBits -= 1) {
                PutBits(config, bs, bs.ancillary_flag, 1);
                bs.ancillary_flag ^= !config.disable_reservoir;
            }
        }

        // writeheader: j bits into the header being built
        void WriteHeader(BitstreamState& bs, std::int32_t val, std::int32_t j) noexcept
        {
            BitstreamState::Header& header = bs.header[bs.h_ptr];
            std::int32_t            ptr    = header.ptr;
            while (j > 0) {
                const std::int32_t k = Min(j, 8 - (ptr & 7));
                j -= k;
                header.buf[ptr >> 3] = static_cast<std::uint8_t>(header.buf[ptr >> 3] | ((val >> j) << (8 - (ptr & 7) - k)));
                ptr += k;
            }
            header.ptr = ptr;
        }

        // LAME: CRC_update() (bitstream.c)
        std::int32_t CrcUpdate(std::int32_t value, std::int32_t crc) noexcept
        {
            value <<= 8;
            for (std::int32_t i = 0; i < 8; ++i) {
                value <<= 1;
                crc <<= 1;
                if (((crc ^ value) & 0x10000))
                    crc ^= CRC16_POLYNOMIAL;
            }
            return crc;
        }

        // table_select 14 does not exist; LAME stores 16 there (16 with no linbits)
        std::int32_t TableSelect(std::int32_t& table) noexcept
        {
            if (table == 14)
                table = 16;
            return table;
        }

        // encodeSideInfo2: the frame header and the side information into the header ring
        void EncodeSideInfo(const SessionConfig& config, BitstreamState& bs, SideInfo& side, std::int32_t bitrateIndex, std::int32_t modeExt,
                            std::int32_t bitsPerFrame) noexcept
        {
            bs.header[bs.h_ptr].ptr = 0;
            std::memset(bs.header[bs.h_ptr].buf.data(), 0, config.sideinfo_len);
            WriteHeader(bs, 0xfff, 12);
            WriteHeader(bs, 1, 1);     // MPEG version ID - always MPEG-1
            WriteHeader(bs, 4 - 3, 2); // layer III
            WriteHeader(bs, !config.error_protection, 1);
            WriteHeader(bs, bitrateIndex, 4);
            WriteHeader(bs, config.samplerate_index, 2);
            WriteHeader(bs, 0, 1); // padding: VBR frames are never padded
            WriteHeader(bs, config.extension, 1);
            WriteHeader(bs, config.mode, 2);
            WriteHeader(bs, modeExt, 2);
            WriteHeader(bs, config.copyright, 1);
            WriteHeader(bs, config.original, 1);
            WriteHeader(bs, config.emphasis, 2);
            if (config.error_protection)
                WriteHeader(bs, 0, 16); // dummy

            WriteHeader(bs, side.main_data_begin, 9);
            WriteHeader(bs, side.private_bits, config.channels_out == 2 ? 3 : 5);
            for (std::int32_t ch = 0; ch < config.channels_out; ++ch) {
                for (std::int32_t band = 0; band < 4; ++band)
                    WriteHeader(bs, side.scfsi[ch][band], 1);
            }

            for (std::int32_t gr = 0; gr < 2; ++gr) {
                for (std::int32_t ch = 0; ch < config.channels_out; ++ch) {
                    GrInfo& gi = side.tt[gr][ch];
                    WriteHeader(bs, gi.part2_3_length + gi.part2_length, 12);
                    WriteHeader(bs, gi.big_values / 2, 9);
                    WriteHeader(bs, gi.global_gain, 8);
                    WriteHeader(bs, gi.scalefac_compress, 4);

                    if (gi.block_type != NORM_TYPE) {
                        WriteHeader(bs, 1, 1); // window_switching_flag
                        WriteHeader(bs, gi.block_type, 2);
                        WriteHeader(bs, gi.mixed_block_flag, 1);
                        WriteHeader(bs, TableSelect(gi.table_select[0]), 5);
                        WriteHeader(bs, TableSelect(gi.table_select[1]), 5);
                        WriteHeader(bs, gi.subblock_gain[0], 3);
                        WriteHeader(bs, gi.subblock_gain[1], 3);
                        WriteHeader(bs, gi.subblock_gain[2], 3);
                    } else {
                        WriteHeader(bs, 0, 1); // window_switching_flag
                        WriteHeader(bs, TableSelect(gi.table_select[0]), 5);
                        WriteHeader(bs, TableSelect(gi.table_select[1]), 5);
                        WriteHeader(bs, TableSelect(gi.table_select[2]), 5);
                        WriteHeader(bs, gi.region0_count, 4);
                        WriteHeader(bs, gi.region1_count, 3);
                    }
                    WriteHeader(bs, gi.preflag, 1);
                    WriteHeader(bs, gi.scalefac_scale, 1);
                    WriteHeader(bs, gi.count1table_select, 1);
                }
            }

            if (config.error_protection)
                CrcWriteHeader(config, bs.header[bs.h_ptr].buf.data());

            const std::int32_t old           = bs.h_ptr;
            bs.h_ptr                         = (old + 1) & (MaxHeaderBuffers - 1);
            bs.header[bs.h_ptr].write_timing = bs.header[old].write_timing + bitsPerFrame;
            if (bs.h_ptr == bs.w_ptr)
                Console::Print(Stream::Error, L"Error: MAX_HEADER_BUF too small in bitstream.c \n");
        }

        // The main data writers below take a bit sink: the bit stream itself (StreamWriter), or a
        // frame's own buffer when the main data is coded ahead in parallel (BufferWriter).
        struct StreamWriter {
            const SessionConfig& config;
            BitstreamState&      bs;
            void                 Put(std::int32_t val, std::int32_t j) noexcept { PutBits(config, bs, val, j); }
        };

        // The bits collect in a 64-bit word and go to the buffer 32 at a time, most significant
        // first; Finish writes the rest, padded with zeros. One Put writes at most 28 bits (two
        // linbits of 13 and two signs), so fewer than 32 pending bits plus one Put fit.
        struct BufferWriter {
            std::uint8_t* buf;
            std::int32_t  bits    = 0; // the bit count, set by Finish
            std::uint64_t acc     = 0; // the pending bits are its lowest ones
            std::int32_t  pending = 0;
            std::int32_t  bytes   = 0;

            // the lowest j bits of val, most significant first, like PutBits
            void Put(std::int32_t val, std::int32_t j) noexcept
            {
                acc = (acc << j) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(val)) & ((std::uint64_t{1} << j) - 1u));
                pending += j;
                if (pending >= 32) {
                    pending -= 32;
                    const auto word = static_cast<std::uint32_t>(acc >> pending);
                    buf[bytes]      = static_cast<std::uint8_t>(word >> 24);
                    buf[bytes + 1]  = static_cast<std::uint8_t>(word >> 16);
                    buf[bytes + 2]  = static_cast<std::uint8_t>(word >> 8);
                    buf[bytes + 3]  = static_cast<std::uint8_t>(word);
                    bytes += 4;
                }
            }

            void Finish() noexcept
            {
                while (pending >= 8) {
                    pending -= 8;
                    buf[bytes++] = static_cast<std::uint8_t>(acc >> pending);
                }
                if (pending > 0)
                    buf[bytes] = static_cast<std::uint8_t>(acc << (8 - pending));
                bits = bytes * 8 + pending;
            }
        };

        // huffman_coder_count1: the quadruples of 0/1 values with their sign bits
        template <typename Writer>
        std::int32_t HuffmanCodeCount1(Writer& w, const GrInfo& gi) noexcept
        {
            const HuffCodeTab&  h    = ht[gi.count1table_select + 32];
            const std::int32_t* ix   = gi.l3_enc.data() + gi.big_values; // big_values may be 576
            const float*        xr   = gi.xr.data() + gi.big_values;
            std::int32_t        bits = 0;

            for (std::int32_t i = (gi.count1 - gi.big_values) / 4; i > 0; --i) {
                std::int32_t signs = 0;
                std::int32_t p     = 0;
                for (std::int32_t k = 0; k < 4; ++k) {
                    if (ix[k]) {
                        p += 8 >> k;
                        signs *= 2;
                        if (xr[k] < 0.0f)
                            signs++;
                    }
                }
                ix += 4;
                xr += 4;
                w.Put(signs + h.table[p], h.hlen[p]);
                bits += h.hlen[p];
            }
            return bits;
        }

        // Huffmancode: pairs of big values with table tableIndex. The sign bits go with the
        // linbits: the code is written with hlen minus the sign bits (LAME's tables are laid out
        // for this), linbits and signs follow in one word.
        template <typename Writer>
        std::int32_t HuffmanCode(Writer& w, std::uint32_t tableIndex, std::int32_t start, std::int32_t end, const GrInfo& gi) noexcept
        {
            // table_select 14 stands for 16 (TableSelect); the main data may be coded before
            // the side information maps it
            if (tableIndex == 14)
                tableIndex = 16;
            const HuffCodeTab&  h       = ht[tableIndex];
            const std::uint32_t linbits = h.xlen;
            std::int32_t        bits    = 0;

            if (!tableIndex)
                return bits;

            for (std::int32_t i = start; i < end; i += 2) {
                std::int16_t  cbits = 0;
                std::uint16_t xbits = 0;
                std::uint32_t xlen  = h.xlen;
                std::uint32_t ext   = 0;
                std::uint32_t x1    = static_cast<std::uint32_t>(gi.l3_enc[i]);
                std::uint32_t x2    = static_cast<std::uint32_t>(gi.l3_enc[i + 1]);

                if (x1 != 0u) {
                    if (gi.xr[i] < 0.0f)
                        ext++;
                    cbits--;
                }

                if (tableIndex > 15u) {
                    // escape words
                    if (x1 >= 15u) {
                        const std::uint16_t linbitsX1 = static_cast<std::uint16_t>(x1 - 15u);
                        ext |= static_cast<std::uint32_t>(linbitsX1) << 1u;
                        xbits = static_cast<std::uint16_t>(linbits);
                        x1    = 15u;
                    }
                    if (x2 >= 15u) {
                        const std::uint16_t linbitsX2 = static_cast<std::uint16_t>(x2 - 15u);
                        ext <<= linbits;
                        ext |= linbitsX2;
                        xbits = static_cast<std::uint16_t>(xbits + linbits);
                        x2    = 15u;
                    }
                    xlen = 16;
                }

                if (x2 != 0u) {
                    ext <<= 1;
                    if (gi.xr[i + 1] < 0.0f)
                        ext++;
                    cbits--;
                }

                x1    = x1 * xlen + x2;
                xbits = static_cast<std::uint16_t>(xbits - cbits);
                cbits = static_cast<std::int16_t>(cbits + h.hlen[x1]);

                w.Put(h.table[x1], cbits);
                w.Put(static_cast<std::int32_t>(ext), xbits);
                bits += cbits + xbits;
            }
            return bits;
        }

        // LAME: ShortHuffmancodebits() (bitstream.c)
        template <typename Writer>
        std::int32_t ShortHuffmanCodeBits(const SessionConfig& config, Writer& w, const GrInfo& gi) noexcept
        {
            std::int32_t region1Start = 3 * config.scalefac_band.s[3];
            if (region1Start > gi.big_values)
                region1Start = gi.big_values;
            // short blocks have no region2
            std::int32_t bits = HuffmanCode(w, gi.table_select[0], 0, region1Start, gi);
            bits += HuffmanCode(w, gi.table_select[1], region1Start, gi.big_values, gi);
            return bits;
        }

        // LAME: LongHuffmancodebits() (bitstream.c)
        template <typename Writer>
        std::int32_t LongHuffmanCodeBits(const SessionConfig& config, Writer& w, const GrInfo& gi) noexcept
        {
            const std::int32_t bigValues    = gi.big_values;
            std::uint32_t      i            = gi.region0_count + 1;
            std::int32_t       region1Start = config.scalefac_band.l[i];
            i += gi.region1_count + 1;
            std::int32_t region2Start = config.scalefac_band.l[i];

            if (region1Start > bigValues)
                region1Start = bigValues;
            if (region2Start > bigValues)
                region2Start = bigValues;

            std::int32_t bits = HuffmanCode(w, gi.table_select[0], 0, region1Start, gi);
            bits += HuffmanCode(w, gi.table_select[1], region1Start, region2Start, gi);
            bits += HuffmanCode(w, gi.table_select[2], region2Start, bigValues, gi);
            return bits;
        }

        // writeMainData: scalefactors and Huffman data of all granules and channels
        template <typename Writer>
        std::int32_t WriteMainData(const SessionConfig& config, Writer& w, const SideInfo& side) noexcept
        {
            std::int32_t total = 0;
            for (std::int32_t gr = 0; gr < 2; ++gr) {
                for (std::int32_t ch = 0; ch < config.channels_out; ++ch) {
                    const GrInfo&      gi    = side.tt[gr][ch];
                    const std::int32_t slen1 = slen1_tab[gi.scalefac_compress];
                    const std::int32_t slen2 = slen2_tab[gi.scalefac_compress];
                    std::int32_t       bits  = 0;
                    std::int32_t       sfb   = 0;
                    for (; sfb < gi.sfbdivide; ++sfb) {
                        if (gi.scalefac[sfb] == -1)
                            continue; // scfsi is used
                        w.Put(gi.scalefac[sfb], slen1);
                        bits += slen1;
                    }
                    for (; sfb < gi.sfbmax; ++sfb) {
                        if (gi.scalefac[sfb] == -1)
                            continue; // scfsi is used
                        w.Put(gi.scalefac[sfb], slen2);
                        bits += slen2;
                    }

                    if (gi.block_type == SHORT_TYPE)
                        bits += ShortHuffmanCodeBits(config, w, gi);
                    else
                        bits += LongHuffmanCodeBits(config, w, gi);
                    bits += HuffmanCodeCount1(w, gi);
                    total += bits;
                }
            }
            return total;
        }

        // compute_flushbits: the bits needed to complete all buffered frames; this equals the
        // reservoir between frames
        std::int32_t ComputeFlushBits(const SessionConfig& config, const BitstreamState& bs, std::int32_t bitrateIndex, std::int32_t& totalBytesOutput) noexcept
        {
            const std::int32_t firstPtr = bs.w_ptr; // first header to add to the bit stream
            std::int32_t       lastPtr  = bs.h_ptr - 1;
            if (lastPtr == -1)
                lastPtr = MaxHeaderBuffers - 1;

            // bits until all headers are written
            std::int32_t flushbits = bs.header[lastPtr].write_timing - bs.totbit;
            totalBytesOutput       = flushbits;
            if (flushbits >= 0) {
                // headers not yet written: without their own size
                std::int32_t remainingHeaders = 1 + lastPtr - firstPtr;
                if (lastPtr < firstPtr)
                    remainingHeaders = 1 + lastPtr - firstPtr + MaxHeaderBuffers;
                flushbits -= remainingHeaders * 8 * config.sideinfo_len;
            }

            // and the rest of the last frame, which some decoders want complete
            const std::int32_t bitsPerFrame = FrameBits(config, bitrateIndex);
            flushbits += bitsPerFrame;
            totalBytesOutput += bitsPerFrame;
            if (totalBytesOutput % 8)
                totalBytesOutput = 1 + (totalBytesOutput / 8);
            else
                totalBytesOutput = totalBytesOutput / 8;
            totalBytesOutput += bs.buf_byte_idx + 1;

            if (flushbits < 0)
                Console::Print(Stream::Error, L"strange error flushing buffer ... \n");
            return flushbits;
        }

    } // namespace

    void BitstreamInit(BitstreamState& bs) noexcept
    {
        std::memset(&bs, 0, sizeof(bs));
        bs.h_ptr                         = 0;
        bs.w_ptr                         = 0;
        bs.header[bs.h_ptr].write_timing = 0;
        bs.buf_byte_idx                  = -1;
        bs.buf_bit_idx                   = 0;
        bs.totbit                        = 0;
    }

    void BitstreamAddDummyBytes(BitstreamState& bs, std::uint8_t value, std::uint32_t count) noexcept
    {
        while (count-- > 0u) {
            PutBitsNoHeaders(bs, value, 8);
            for (std::int32_t i = 0; i < MaxHeaderBuffers; ++i)
                bs.header[i].write_timing += 8;
        }
    }

    void CrcWriteHeader(const SessionConfig& config, std::uint8_t* header) noexcept
    {
        const std::uint8_t* const bytes = header;
        std::int32_t              crc   = 0xffff; // init crc16 for error_protection
        crc                             = CrcUpdate(bytes[2], crc);
        crc                             = CrcUpdate(bytes[3], crc);
        for (std::int32_t i = 6; i < config.sideinfo_len; ++i)
            crc = CrcUpdate(bytes[i], crc);
        header[4] = static_cast<std::uint8_t>(crc >> 8);
        header[5] = static_cast<std::uint8_t>(crc & 255);
    }

    void FormatBitstream(const SessionConfig& config, BitstreamState& bs, ReservoirState& reservoir, FrameData& frame) noexcept
    {
        SideInfo&          side         = frame.side;
        const std::int32_t bitsPerFrame = FrameBits(config, frame.bitrateIndex);
        DrainIntoAncillary(config, bs, side.resvDrain_pre);

        EncodeSideInfo(config, bs, side, frame.bitrateIndex, frame.modeExt, bitsPerFrame);
        std::int32_t bits = 8 * config.sideinfo_len;
        if (frame.mainDataValid) {
            // coded ahead by PrepareMainData: append the bits, byte by byte through PutBits, so
            // that headers go in at the same positions
            const std::int32_t full = frame.mainDataBits >> 3;
            for (std::int32_t i = 0; i < full; ++i)
                PutBits(config, bs, frame.mainData[i], 8);
            if (const std::int32_t rest = frame.mainDataBits & 7)
                PutBits(config, bs, frame.mainData[full] >> (8 - rest), rest);
            bits += frame.mainDataBits;
        } else {
            StreamWriter writer{config, bs};
            bits += WriteMainData(config, writer, side);
        }
        DrainIntoAncillary(config, bs, side.resvDrain_post);
        bits += side.resvDrain_post;

        reservoir.mainDataBegin = side.main_data_begin + (bitsPerFrame - bits) / 8;

        // the bits that complete the buffered frames must equal the reservoir, and so must
        // main_data_begin of the next frame
        std::int32_t totalBytes;
        if (ComputeFlushBits(config, bs, frame.bitrateIndex, totalBytes) != reservoir.size)
            Console::Print(Stream::Error, L"Internal buffer inconsistency. flushbits <> ResvSize");

        if ((reservoir.mainDataBegin * 8) != reservoir.size) {
            Console::Print(Stream::Error,
                           L"bit reservoir error: \n"
                           L"l3_side->main_data_begin: %i \n"
                           L"Resvoir size:             %i \n"
                           L"resv drain (post)         %i \n"
                           L"resv drain (pre)          %i \n"
                           L"header and sideinfo:      %i \n"
                           L"data bits:                %i \n"
                           L"total bits:               %i (remainder: %i) \n"
                           L"bitsperframe:             %i \n",
                           8 * reservoir.mainDataBegin, reservoir.size, side.resvDrain_post, side.resvDrain_pre, 8 * config.sideinfo_len,
                           bits - side.resvDrain_post - 8 * config.sideinfo_len, bits, bits % 8, bitsPerFrame);
            Console::Print(Stream::Error, L"This is a fatal error.  It has several possible causes:");
            Console::Print(Stream::Error, L"90%%  LAME compiled with buggy version of gcc using advanced optimizations");
            Console::Print(Stream::Error, L" 9%%  Your system is overclocked");
            Console::Print(Stream::Error, L" 1%%  bug in LAME encoding library");
            reservoir.size = reservoir.mainDataBegin * 8;
        }

        // avoid an overflow of totbit (after 8 hours at 128 kbps)
        if (bs.totbit > 1000000000) {
            for (std::int32_t i = 0; i < MaxHeaderBuffers; ++i)
                bs.header[i].write_timing -= bs.totbit;
            bs.totbit = 0;
        }
    }

    void PrepareMainData(const SessionConfig& config, FrameData& frame) noexcept
    {
        // a granule beyond MAX_BITS_PER_CHANNEL (LARGE_BITS: values beyond the Huffman tables)
        // is always reworked by the commit, and its tables are not valid yet
        frame.mainDataValid = false;
        for (std::int32_t gr = 0; gr < 2; ++gr) {
            for (std::int32_t ch = 0; ch < config.channels_out; ++ch) {
                if (frame.search[gr][ch].usedBits > MAX_BITS_PER_CHANNEL || frame.search[gr][ch].failed)
                    return;
            }
        }

        BufferWriter writer{frame.mainData.data()};
        WriteMainData(config, writer, frame.side);
        writer.Finish();
        frame.mainDataBits  = writer.bits;
        frame.mainDataValid = true;
    }

    void FlushBitstream(const SessionConfig& config, BitstreamState& bs, ReservoirState& reservoir, std::int32_t lastBitrateIndex) noexcept
    {
        std::int32_t       totalBytes;
        const std::int32_t flushbits = ComputeFlushBits(config, bs, lastBitrateIndex, totalBytes);
        if (flushbits < 0)
            return;
        DrainIntoAncillary(config, bs, flushbits);

        // all frames are padded with ancillary data, which empties the reservoir
        reservoir.size          = 0;
        reservoir.mainDataBegin = 0;
    }

    std::int32_t BitstreamCopy(BitstreamState& bs, std::byte* buffer, std::int32_t size) noexcept
    {
        const std::int32_t minimum = bs.buf_byte_idx + 1;
        if (minimum <= 0)
            return 0;
        if (minimum > size)
            return -1; // buffer is too small
        std::memcpy(buffer, bs.buf.data(), minimum);
        bs.buf_byte_idx = -1;
        bs.buf_bit_idx  = 0;
        return minimum;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
