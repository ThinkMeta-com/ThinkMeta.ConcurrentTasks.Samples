// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    /// <summary>
    /// opus_get_version_string of the libopus float build that ctOpus reproduces; opusenc writes it
    /// into the vendor string of OpusTags, so it stays for bit identity.
    /// </summary>
    inline constexpr const char* LibopusVersion = "libopus 1.6.1";

    /// <summary>opus_strerror: the text of an OPUS error code.</summary>
    const char* OpusStrError(std::int32_t error) noexcept;

    /// <summary>opus_packet_get_nb_samples: the samples of a packet at fs; an OPUS error code for invalid packets.</summary>
    std::int32_t PacketGetNbSamples(const std::uint8_t* packet, std::int32_t len, std::int32_t fs) noexcept;

    /// <summary>
    /// Port of the repacketizer of libopus 1.6.1 (src/repacketizer.c, opus_packet_parse_impl of
    /// src/opus.c) for the packets this encoder produces: their padding holds no extensions (only
    /// 0x00/0x01 padding bytes), so the extension handling of libopus reduces to nothing; Cat
    /// rejects padding with anything else.
    /// </summary>
    class Repacketizer
    {
    public:
        /// <summary>opus_repacketizer_init.</summary>
        void Init() noexcept { nbFrames_ = 0; }

        /// <summary>opus_repacketizer_cat: adds the frames of a packet (data must stay valid); an OPUS error code.</summary>
        std::int32_t Cat(const std::uint8_t* data, std::int32_t len) noexcept;

        std::int32_t NbFrames() const noexcept { return nbFrames_; }

        /// <summary>opus_repacketizer_out_range_impl without extensions: the frames [begin, end) as one packet; its size or an OPUS error code.</summary>
        std::int32_t OutRange(std::int32_t begin, std::int32_t end, std::uint8_t* data, std::int32_t maxLen, bool selfDelimited, bool pad) noexcept;

    private:
        std::uint8_t        toc_        = 0;
        std::int32_t        nbFrames_   = 0;
        const std::uint8_t* frames_[48] = {};
        std::int16_t        len_[48]    = {};
        std::int32_t        framesize_  = 0;
    };

    /// <summary>opus_packet_pad: pads a packet in place from len to newLen bytes; an OPUS error code.</summary>
    std::int32_t PacketPad(std::uint8_t* data, std::int32_t len, std::int32_t newLen) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
