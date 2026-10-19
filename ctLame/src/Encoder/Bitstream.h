// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "FrameData.h"
#include "QuantizeTypes.h"
#include "Reservoir.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    struct SessionConfig;

    inline constexpr std::int32_t BitstreamBufferSize = 16384; // BUFFER_SIZE (LAME_MAXMP3BUFFER)
    inline constexpr std::int32_t MaxHeaderBuffers    = 256;   // MAX_HEADER_BUF
    inline constexpr std::int32_t MaxHeaderLength     = 40;    // MAX_HEADER_LEN: header and side info, at most 4 + 32 + 2 bytes

    /// <summary>
    /// The bit stream writer of bitstream.c (Bit_stream_struc and the header ring of
    /// EncStateVar_t). Main data goes into buf; frame headers with side information wait in a
    /// ring until the main data reaches their position (write_timing), because main data may
    /// start in earlier frames (bit reservoir).
    /// </summary>
    struct BitstreamState {
        struct Header {
            std::int32_t                              write_timing;
            std::int32_t                              ptr;
            std::array<std::uint8_t, MaxHeaderLength> buf;
        };

        std::array<std::uint8_t, BitstreamBufferSize> buf;
        std::int32_t                                  buf_byte_idx = -1;
        std::int32_t                                  buf_bit_idx  = 0;
        std::int32_t                                  totbit       = 0;

        std::array<Header, MaxHeaderBuffers> header;
        std::int32_t                         h_ptr          = 0;
        std::int32_t                         w_ptr          = 0;
        std::int32_t                         ancillary_flag = 0;
    };

    /// <summary>init_bit_stream_w.</summary>
    void BitstreamInit(BitstreamState& bs) noexcept;

    /// <summary>add_dummy_byte: bytes outside the frame structure (the placeholder of the LAME tag).</summary>
    void BitstreamAddDummyBytes(BitstreamState& bs, std::uint8_t value, std::uint32_t count) noexcept;

    /// <summary>
    /// format_bitstream: writes the quantized frame (side information into the header ring,
    /// main data and stuffing into the buffer) and checks the reservoir bookkeeping.
    /// </summary>
    void FormatBitstream(const SessionConfig& config, BitstreamState& bs, ReservoirState& reservoir, FrameData& frame) noexcept;

    /// <summary>
    /// The main data of a frame (scalefactors and Huffman codes) into frame.mainData, ahead of
    /// FormatBitstream: it depends on the quantized granules only, not on the
    /// reservoir, so it may run for several frames at once. FormatBitstream appends these bits
    /// while frame.mainDataValid holds; the quantization clears it when it changes the granules.
    /// </summary>
    void PrepareMainData(const SessionConfig& config, FrameData& frame) noexcept;

    /// <summary>flush_bitstream: pads the last frames with ancillary data so all headers are written.</summary>
    void FlushBitstream(const SessionConfig& config, BitstreamState& bs, ReservoirState& reservoir, std::int32_t lastBitrateIndex) noexcept;

    /// <summary>
    /// do_copy_buffer: moves the complete bytes out of the buffer. Returns their number, -1 if
    /// size is too small.
    /// </summary>
    std::int32_t BitstreamCopy(BitstreamState& bs, std::byte* buffer, std::int32_t size) noexcept;

    /// <summary>CRC_writeheader: CRC-16 of header and side information for error protection.</summary>
    void CrcWriteHeader(const SessionConfig& config, std::uint8_t* header) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
