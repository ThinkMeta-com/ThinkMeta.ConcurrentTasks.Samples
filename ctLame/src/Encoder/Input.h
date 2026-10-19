// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>Receives blocks of PCM samples in LAME's int representation (lame_encode_buffer_int).</summary>
    struct IPcmSink {
        virtual void AddSamples(const std::int32_t* left, const std::int32_t* right, std::int32_t count) = 0;
    };

    /// <summary>
    /// Port of the sample reading of frontend/get_audio.c: turns the bytes of the WAV data
    /// chunk into ints (the sample in the upper bits), in blocks of 1152 frames like get_audio.
    /// The last, incomplete block behaves like LAME's last fread, including its quirks: a
    /// trailing partial sample is dropped, and an odd number of samples in a stereo file shifts
    /// the channels of that block.
    /// </summary>
    class PcmReader
    {
    public:
        struct Format {
            std::int32_t channels     = 2;
            std::int32_t bitWidth     = 16;
            bool         ieeeFloat    = false;
            bool         swapChannels = false; // --swap-channel
        };

        /// <summary>False if the sample width cannot be read (read_samples_pcm fails).</summary>
        bool Initialize(const Format& format) noexcept;

        /// <summary>Adds bytes of the data chunk; complete blocks go to the sink.</summary>
        void Push(const std::byte* data, std::size_t size, IPcmSink& sink);

        /// <summary>The end of the data: the incomplete last block goes to the sink.</summary>
        void Finish(IPcmSink& sink);

    private:
        void ProcessChunk(std::size_t bytes, IPcmSink& sink);

        Format                                             format_;
        std::int32_t                                       bytesPerSample_ = 2;
        std::size_t                                        chunkBytes_     = 0;
        std::size_t                                        used_           = 0;
        std::array<std::byte, FRAMESIZE * 2 * 4>           chunk_;
        std::array<std::int32_t, FRAMESIZE * 2>            samples_;
        std::array<std::array<std::int32_t, FRAMESIZE>, 2> buffer_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
