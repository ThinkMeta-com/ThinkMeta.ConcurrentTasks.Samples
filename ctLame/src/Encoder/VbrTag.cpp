// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "VbrTag.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Bitstream.h"
#include "SessionConfig.h"
#include "Tables.h"
#include "Version.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        // the Xing header: tag, flags, frames, bytes, TOC, VBR scale; the LAME extension follows
        constexpr std::int32_t VBRHEADERSIZE  = NUMTOCENTRIES + 4 + 4 + 4 + 4 + 4;
        constexpr std::int32_t LAMEHEADERSIZE = VBRHEADERSIZE + 9 + 1 + 1 + 8 + 1 + 1 + 3 + 1 + 1 + 2 + 4 + 2 + 2;
        constexpr std::int32_t XING_BITRATE1  = 128; // the tag frame is a 128 kbps frame
        constexpr std::int32_t MAXFRAMESIZE   = 2880;

        constexpr std::uint32_t FRAMES_FLAG    = 0x0001;
        constexpr std::uint32_t BYTES_FLAG     = 0x0002;
        constexpr std::uint32_t TOC_FLAG       = 0x0004;
        constexpr std::uint32_t VBR_SCALE_FLAG = 0x0008;

        // LAME: CreateI4() (VbrTag.c)
        void CreateI4(std::uint8_t* buf, std::uint32_t value) noexcept
        {
            // big endian
            buf[0] = static_cast<std::uint8_t>((value >> 24) & 0xff);
            buf[1] = static_cast<std::uint8_t>((value >> 16) & 0xff);
            buf[2] = static_cast<std::uint8_t>((value >> 8) & 0xff);
            buf[3] = static_cast<std::uint8_t>(value & 0xff);
        }

        // LAME: CreateI2() (VbrTag.c)
        void CreateI2(std::uint8_t* buf, std::int32_t value) noexcept
        {
            buf[0] = static_cast<std::uint8_t>((value >> 8) & 0xff);
            buf[1] = static_cast<std::uint8_t>(value & 0xff);
        }

        // SHIFT_IN_BITS_VALUE: appends the lowest n bits of v to a byte
        void ShiftIn(std::uint8_t& x, std::uint32_t n, std::int32_t v) noexcept
        {
            x = static_cast<std::uint8_t>((x << n) | (v & ~(-1 << n)));
        }

        // LAME: CRC_update_lookup() (VbrTag.c)
        // CRC-16 with polynomial x^16 + x^15 + x^2 + 1 (reflected 0xA001), byte by byte
        std::uint16_t CrcUpdate(const std::array<std::uint16_t, 256>& table, std::uint16_t crc, const std::uint8_t* buffer, std::int32_t size) noexcept
        {
            while (size-- > 0)
                crc = static_cast<std::uint16_t>((crc >> 8) ^ table[(crc ^ *buffer++) & 0xff]);
            return crc;
        }

        // setLameTagFrameHeader: the frame header of the tag frame, from the session values
        void SetLameTagFrameHeader(const SessionConfig& config, const EncoderResult& result, std::uint8_t* buffer) noexcept
        {
            ShiftIn(buffer[0], 8u, 0xff);

            ShiftIn(buffer[1], 3u, 7);
            ShiftIn(buffer[1], 1u, 1); // MPEG version ID bit 0 - always MPEG-1
            ShiftIn(buffer[1], 1u, 1); // MPEG version ID bit 1 - always MPEG-1
            ShiftIn(buffer[1], 2u, 4 - 3);
            ShiftIn(buffer[1], 1u, (!config.error_protection) ? 1 : 0);

            ShiftIn(buffer[2], 4u, result.bitrate_index);
            ShiftIn(buffer[2], 2u, config.samplerate_index);
            ShiftIn(buffer[2], 1u, 0);
            ShiftIn(buffer[2], 1u, config.extension);

            ShiftIn(buffer[3], 2u, config.mode);
            ShiftIn(buffer[3], 2u, result.mode_ext);
            ShiftIn(buffer[3], 1u, config.copyright);
            ShiftIn(buffer[3], 1u, config.original);
            ShiftIn(buffer[3], 2u, config.emphasis);

            // a 128 kbps frame, no padding, no CRC; sample rate, mode and copyright from the
            // session
            buffer[0]                = 0xff;
            std::uint8_t       abyte = static_cast<std::uint8_t>(buffer[1] & 0xf1);
            const std::uint8_t bbyte = static_cast<std::uint8_t>(16 * BitrateIndex(XING_BITRATE1));
            buffer[1]                = static_cast<std::uint8_t>(abyte | 0x0a);
            abyte                    = static_cast<std::uint8_t>(buffer[2] & 0x0d); // keep also the private bit
            buffer[2]                = static_cast<std::uint8_t>(bbyte | abyte);
        }

        // Xing_seek_table: 100 entries, each the position of i % of the duration in 1/256 of
        // the stream
        void XingSeekTable(const VbrTagState& tag, std::uint8_t* t) noexcept
        {
            if (tag.pos <= 0)
                return;
            for (std::int32_t i = 1; i < NUMTOCENTRIES; ++i) {
                const float  j     = i / static_cast<float>(NUMTOCENTRIES);
                std::int32_t index = static_cast<std::int32_t>(std::floor(static_cast<double>(j * tag.pos)));
                if (index > tag.pos - 1)
                    index = tag.pos - 1;
                const float  act       = static_cast<float>(tag.bag[index]);
                const float  sum       = static_cast<float>(tag.sum);
                std::int32_t seekPoint = static_cast<std::int32_t>(256. * act / sum);
                if (seekPoint > 255)
                    seekPoint = 255;
                t[i] = static_cast<std::uint8_t>(seekPoint);
            }
        }

        // PutLameVBR: the LAME extension of the tag. crc is the CRC of the frame so far.
        std::int32_t PutLameVbr(const SessionConfig& config, const VbrTagState& tag, const EncoderResult& result, std::size_t musicLength, std::uint8_t* out,
                                std::uint16_t crc) noexcept
        {
            const EncoderOptions& options    = config.options;
            std::int32_t          written    = 0;
            const std::int32_t    encDelay   = result.encoder_delay;
            const std::int32_t    encPadding = result.encoder_padding;

            std::int32_t quality = 100 - 10 * options.vbrQ - options.quality;
            if (quality < 0)
                quality = 0;

            const double       lowpass  = (config.lowpassfreq / 100.0) + .5;
            const std::uint8_t nLowpass = static_cast<std::uint8_t>(lowpass > 255 ? 255 : static_cast<std::int32_t>(lowpass));

            // no ReplayGain analysis: peak and gain fields stay 0
            const std::uint32_t peakSignalAmplitude  = 0;
            const std::int32_t  radioReplayGain      = 0;
            const std::int32_t  audiophileReplayGain = 0;

            const std::uint8_t noiseShaping = static_cast<std::uint8_t>(config.noise_shaping);
            const std::uint8_t expNPsyTune  = 1; // only NsPsytune
            const std::uint8_t safeJoint    = config.use_safe_joint_stereo != 0;
            const std::uint8_t athType      = static_cast<std::uint8_t>(config.ATHtype);
            const std::uint8_t revMethod    = 0x10 * 0 + 4; // revision 0, VBR method 4 = mtrh

            // bits 6/7: nogap flags, always 0
            const std::uint8_t flags = static_cast<std::uint8_t>(athType + (expNPsyTune << 4) + (safeJoint << 5));

            // VBR: the minimum bitrate (-b)
            const std::int32_t abrBitrate = BitrateTable[config.vbr_min_bitrate_index];

            std::uint8_t stereoMode;
            switch (config.mode) {
                case Mono: stereoMode = 0; break;
                case Stereo: stereoMode = 1; break;
                case DualChannel: stereoMode = 2; break;
                case JointStereo: stereoMode = config.force_ms ? 4 : 3; break;
                default: stereoMode = 7; break;
            }

            std::uint8_t sourceFreq;
            if (config.samplerate_in <= 32000)
                sourceFreq = 0x00;
            else if (config.samplerate_in == 48000)
                sourceFreq = 0x02;
            else if (config.samplerate_in > 48000)
                sourceFreq = 0x03;
            else
                sourceFreq = 0x01; // 44.1 kHz

            // did the user override LAME's defaults with some nasty options?
            const std::int32_t nonOptimal
                = (config.short_blocks == ShortBlockForced || config.short_blocks == ShortBlockDispensed
                   || (config.lowpassfreq == -1 && config.highpassfreq == -1) || // -k
                   (config.disable_reservoir && config.avg_bitrate < 320) || config.noATH || config.ATHonly || athType == 0 || config.samplerate_in <= 32000)
                ? 1
                : 0;

            const std::uint8_t misc = static_cast<std::uint8_t>(noiseShaping + (stereoMode << 2) + (nonOptimal << 5) + (sourceFreq << 6));

            CreateI4(&out[written], static_cast<std::uint32_t>(quality));
            written += 4;

            // the encoder version: a fixed 9-byte field, zero padded (strncpy)
            const char* const version = GetLameTagEncoderShortVersion();
            std::size_t       length  = std::strlen(version);
            if (length > 9)
                length = 9;
            std::memcpy(&out[written], version, length);
            std::memset(&out[written + length], 0, 9 - length);
            written += 9;

            out[written++] = revMethod;
            out[written++] = nLowpass;

            CreateI4(&out[written], peakSignalAmplitude);
            written += 4;
            CreateI2(&out[written], radioReplayGain);
            written += 2;
            CreateI2(&out[written], audiophileReplayGain);
            written += 2;

            out[written++] = flags;
            out[written++] = abrBitrate >= 255 ? 0xFF : static_cast<std::uint8_t>(abrBitrate);

            out[written]     = static_cast<std::uint8_t>(encDelay >> 4);
            out[written + 1] = static_cast<std::uint8_t>((encDelay << 4) + (encPadding >> 8));
            out[written + 2] = static_cast<std::uint8_t>(encPadding);
            written += 3;

            out[written++] = misc;
            out[written++] = 0; // unused in rev0

            CreateI2(&out[written], config.preset);
            written += 2;
            CreateI4(&out[written], static_cast<std::uint32_t>(static_cast<std::int32_t>(musicLength)));
            written += 4;
            CreateI2(&out[written], tag.musicCrc);
            written += 2;

            // the CRC of the tag includes everything before it
            crc = CrcUpdate(tag.crcTable, crc, out, written);
            CreateI2(&out[written], crc);
            written += 2;
            return written;
        }

    } // namespace

    void InitVbrTag(SessionConfig& config, VbrTagState& tag, BitstreamState& bs) noexcept
    {
        for (std::int32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = static_cast<std::uint32_t>(i);
            for (std::int32_t bit = 0; bit < 8; ++bit)
                crc = (crc & 1) ? (crc >> 1) ^ 0xA001u : crc >> 1;
            tag.crcTable[i] = static_cast<std::uint16_t>(crc);
        }
        tag.musicCrc = 0;

        // the tag frame is a 128 kbps frame; the LAME header must fit into it
        const std::int32_t totalFrameSize = (2 * 72000 * XING_BITRATE1) / config.samplerate_out;
        const std::int32_t headerSize     = config.sideinfo_len + LAMEHEADERSIZE;
        tag.TotalFrameSize                = static_cast<std::uint32_t>(totalFrameSize);
        if (totalFrameSize < headerSize || totalFrameSize > MAXFRAMESIZE) {
            config.write_lame_tag = 0; // disable the tag, it won't fit
            return;
        }

        tag.nVbrNumFrames = 0;
        tag.nBytesWritten = 0;
        tag.sum           = 0;
        tag.seen          = 0;
        tag.want          = 1;
        tag.pos           = 0;
        tag.size          = 400;
        tag.bag.fill(0);

        // a zero frame with the tag's header as placeholder
        std::array<std::uint8_t, MAXFRAMESIZE> buffer;
        buffer.fill(0);
        const EncoderResult initial = {};
        SetLameTagFrameHeader(config, initial, buffer.data());
        for (std::uint32_t i = 0; i < tag.TotalFrameSize; ++i)
            BitstreamAddDummyBytes(bs, buffer[i], 1);
    }

    void AddVbrFrame(VbrTagState& tag, std::int32_t bitrateIndex) noexcept
    {
        tag.nVbrNumFrames++;
        tag.sum += BitrateTable[bitrateIndex];
        tag.seen++;
        if (tag.seen < tag.want)
            return;

        if (tag.pos < tag.size) {
            tag.bag[tag.pos] = tag.sum;
            tag.pos++;
            tag.seen = 0;
        }
        if (tag.pos == tag.size) {
            // full: keep every second entry and collect twice as many frames per entry
            for (std::int32_t i = 1; i < tag.size; i += 2)
                tag.bag[i / 2] = tag.bag[i];
            tag.want *= 2;
            tag.pos /= 2;
        }
    }

    void AddMusicBytes(VbrTagState& tag, const std::byte* bytes, std::int32_t size) noexcept
    {
        const auto* const buffer = reinterpret_cast<const std::uint8_t*>(bytes);
        tag.musicCrc             = CrcUpdate(tag.crcTable, tag.musicCrc, buffer, size);
        tag.nBytesWritten += static_cast<std::uint32_t>(size);
    }

    std::size_t GetLameTagFrame(const SessionConfig& config, const VbrTagState& tag, const EncoderResult& result, std::byte* frame, std::size_t size) noexcept
    {
        auto* const buffer = reinterpret_cast<std::uint8_t*>(frame);
        if (config.write_lame_tag == 0)
            return 0;
        if (tag.pos <= 0)
            return 0;
        if (size < tag.TotalFrameSize)
            return tag.TotalFrameSize;
        if (buffer == nullptr)
            return 0;

        std::memset(buffer, 0, tag.TotalFrameSize);
        SetLameTagFrameHeader(config, result, buffer);

        std::array<std::uint8_t, NUMTOCENTRIES> toc;
        toc.fill(0);
        XingSeekTable(tag, toc.data());

        // the Xing data starts after the side information, as ancillary data without error
        // protection: with CRC it starts at the same offset, inside the side information
        std::uint32_t index = static_cast<std::uint32_t>(config.sideinfo_len);
        if (config.error_protection)
            index -= 2;

        buffer[index++] = 'X'; // the VBR tag; the CBR tag "Info" is never written
        buffer[index++] = 'i';
        buffer[index++] = 'n';
        buffer[index++] = 'g';

        CreateI4(&buffer[index], FRAMES_FLAG + BYTES_FLAG + TOC_FLAG + VBR_SCALE_FLAG);
        index += 4;
        CreateI4(&buffer[index], tag.nVbrNumFrames);
        index += 4;

        // the size of the audio stream, including the tag frame
        const std::uint32_t streamSize = tag.nBytesWritten + tag.TotalFrameSize;
        CreateI4(&buffer[index], static_cast<std::uint32_t>(streamSize));
        index += 4;

        std::memcpy(&buffer[index], toc.data(), sizeof(toc));
        index += sizeof(toc);

        if (config.error_protection)
            CrcWriteHeader(config, buffer);

        const std::uint16_t crc = CrcUpdate(tag.crcTable, 0x00, buffer, static_cast<std::int32_t>(index));
        index += static_cast<std::uint32_t>(PutLameVbr(config, tag, result, streamSize, buffer + index, crc));
        (void)index;
        return tag.TotalFrameSize;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
