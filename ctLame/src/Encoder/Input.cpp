// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Input.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    // --- PcmReader: frontend/get_audio.c -----------------------------------------------------

    bool PcmReader::Initialize(const Format& format) noexcept
    {
        format_         = format;
        bytesPerSample_ = format.bitWidth / 8;
        used_           = 0;
        switch (format.bitWidth) {
            case 32:
            case 24:
            case 16:
            case 8: break;
            default: chunkBytes_ = 0; return false; // "Only 8, 16, 24 and 32 bit input files supported"
        }
        chunkBytes_ = static_cast<std::size_t>(FRAMESIZE) * format.channels * bytesPerSample_;
        return true;
    }

    void PcmReader::Push(const std::byte* data, std::size_t size, IPcmSink& sink)
    {
        if (chunkBytes_ == 0)
            return;
        while (size > 0) {
            const std::size_t take = chunkBytes_ - used_ < size ? chunkBytes_ - used_ : size;
            std::memcpy(chunk_.data() + used_, data, take);
            used_ += take;
            data += take;
            size -= take;
            if (used_ == chunkBytes_) {
                ProcessChunk(used_, sink);
                used_ = 0;
            }
        }
    }

    void PcmReader::Finish(IPcmSink& sink)
    {
        if (chunkBytes_ != 0 && used_ > 0)
            ProcessChunk(used_, sink);
        used_ = 0;
    }

    // One get_audio call: unpack_read_samples, then the split into channels.
    void PcmReader::ProcessChunk(std::size_t bytes, IPcmSink& sink)
    {
        // fread(sample_buffer, bytes_per_sample, samples_to_read): whole samples only
        const std::int32_t  samplesRead = static_cast<std::int32_t>(bytes / bytesPerSample_);
        const std::byte*    ip          = chunk_.data();
        std::int32_t* const op          = samples_.data();

        // the sample goes into the upper bits of an int (b = 32)
        for (std::int32_t n = 0; n < samplesRead; ++n) {
            const std::byte* const s = ip + static_cast<std::size_t>(n) * bytesPerSample_;
            std::uint32_t          v = 0;
            switch (bytesPerSample_) {
                case 1:
                    v = (std::to_integer<std::uint32_t>(s[0]) ^ 0x80u) << 24 | 0x7fu << 16; // convert from unsigned
                    break;
                case 2: v = std::to_integer<std::uint32_t>(s[0]) << 16 | std::to_integer<std::uint32_t>(s[1]) << 24; break;
                case 3:
                    v = std::to_integer<std::uint32_t>(s[0]) << 8 | std::to_integer<std::uint32_t>(s[1]) << 16 | std::to_integer<std::uint32_t>(s[2]) << 24;
                    break;
                default:
                    v = std::to_integer<std::uint32_t>(s[0]) | std::to_integer<std::uint32_t>(s[1]) << 8 | std::to_integer<std::uint32_t>(s[2]) << 16
                        | std::to_integer<std::uint32_t>(s[3]) << 24;
                    break;
            }
            op[n] = static_cast<std::int32_t>(v);
        }

        if (format_.ieeeFloat) {
            const float m_max = static_cast<float>(INT_MAX); // rounds up to 2^31, like LAME's implicit conversion
            const float m_min = -static_cast<float>(INT_MIN);
            for (std::int32_t n = 0; n < samplesRead; ++n) {
                const std::uint32_t bits = static_cast<std::uint32_t>(op[n]);
                float               u;
                std::memcpy(&u, &bits, sizeof(u));
                std::int32_t v;
                if (u >= 1)
                    v = INT_MAX;
                else if (u <= -1)
                    v = INT_MIN;
                else if (u >= 0)
                    v = static_cast<std::int32_t>(u * m_max + 0.5f);
                else
                    v = static_cast<std::int32_t>(u * m_min - 0.5f);
                op[n] = v;
            }
        }

        // split into channels; like LAME, walk backwards from the last sample read
        const std::int32_t* p      = samples_.data() + samplesRead;
        const std::int32_t  frames = samplesRead / format_.channels;
        if (format_.channels == 2) {
            for (std::int32_t i = frames; --i >= 0;) {
                buffer_[1][i] = *--p;
                buffer_[0][i] = *--p;
            }
        } else {
            std::memset(buffer_[1].data(), 0, static_cast<std::size_t>(frames) * sizeof(std::int32_t));
            for (std::int32_t i = frames; --i >= 0;)
                buffer_[0][i] = *--p;
        }

        if (format_.swapChannels) {
            for (std::int32_t i = 0; i < frames; ++i) {
                const std::int32_t tmp = buffer_[0][i];
                buffer_[0][i]          = buffer_[1][i];
                buffer_[1][i]          = tmp;
            }
        }

        if (frames > 0)
            sink.AddSamples(buffer_[0].data(), buffer_[1].data(), frames);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
