// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#if defined(_WIN32)
#    include <Windows.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "Encoder.h"
#include "FrameData.h"

// headers with imports last
#include "Mp3File.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// The work of node Encode: quantization with the reservoir and bit stream of
    /// each searched frame, the bytes into the MP3 stream in frame order, and at the end
    /// lame_encode_flush and the LAME tag frame. Used by EncodeTask in the
    /// parallel graph and by SequentialEncodeTask in the sequential one.
    /// </summary>
    class FrameEncoder
    {
    public:
        FrameEncoder(Mp3File& mp3, Encoder& encoder) noexcept;

        /// <summary>
        /// The start of the MP3 stream: the placeholder of the LAME tag (lame_encode_buffer copies it out before the
        /// first frame).
        /// </summary>
        void Begin() noexcept;

        /// <summary>Encodes an analyzed frame and appends its bytes; does nothing after an error.</summary>
        void EncodeFrame(FrameData& frame) noexcept;

        /// <summary>
        /// The end of lame_encoder_loop: lame_encode_flush with the padding and the ReplayGain of the sample stream,
        /// then the end of the MP3 stream with the LAME tag frame for the file. error is an
        /// error of the input side (0: none, or OutOfMemory).
        /// </summary>
        void Finish(std::int32_t encoderPadding, std::int32_t radioGain, std::int32_t error) noexcept;

        /// <summary>
        /// ctLame's own error code beside LAME's: the input side ran out of memory (the sample stream, or the frame of
        /// the sequential graph), so the output is incomplete.
        /// </summary>
        static constexpr std::int32_t OutOfMemory = -3;

        /// <summary>
        /// 0, LAME's error code of lame_encode_buffer/lame_encode_flush (-1 buffer too small, -2 internal error), or
        /// OutOfMemory.
        /// </summary>
        std::int32_t Error() const noexcept { return error_; }

    private:
        Mp3File& mp3_;
        Encoder& encoder_;

        std::int32_t error_ = 0;

        std::array<std::byte, MaxFrameBytes> buffer_; // the tag placeholder and the flushed bytes
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
