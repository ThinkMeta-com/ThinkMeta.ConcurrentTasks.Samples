// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "Encoder.h"
#include "FrameData.h"
#include "NodeProfile.h"

// headers with imports last
#include "BlockPipe.h"
#include "Mp3WriterTask.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// The work of node Encode: quantization with the reservoir and bit stream of
    /// each searched frame, the bytes into the MP3 pipe in frame order, and at the end
    /// lame_encode_flush and the LAME tag frame for the writer. Used by EncodeTask in the
    /// parallel graph and by SequentialEncodeTask in the sequential one.
    /// </summary>
    class FrameEncoder
    {
    public:
        FrameEncoder(BlockPipe& mp3, Encoder& encoder, Mp3WriterTask& writer, TaskProfile* profile) noexcept;

        /// <summary>The placeholder of the LAME tag (lame_encode_buffer copies it out before the first frame).</summary>
        void Begin() noexcept;

        /// <summary>Encodes an analyzed frame and writes its bytes; does nothing after an error.</summary>
        void EncodeFrame(FrameData& frame) noexcept;

        /// <summary>
        /// The end of lame_encoder_loop: lame_encode_flush with the padding of the sample stream,
        /// the LAME tag, the end of the MP3 stream. error is an error of the input side (0: none).
        /// </summary>
        void Finish(std::int32_t encoderPadding, std::int32_t error) noexcept;

        /// <summary>0, or LAME's error code of lame_encode_buffer/lame_encode_flush (-1 buffer too small, -2 internal error).</summary>
        std::int32_t Error() const noexcept { return error_; }

    private:
        void Write(const std::byte* data, std::int32_t size) noexcept;

        BlockPipe&     mp3_;
        Encoder&       encoder_;
        Mp3WriterTask& writer_;
        TaskProfile*   profile_;

        std::byte*    block_     = nullptr; // the MP3 block being filled
        std::uint32_t blockUsed_ = 0;
        std::int32_t  error_     = 0;

        /// <summary>The largest LAME tag frame: MAXFRAMESIZE of VbrTag.c, the largest free-format frame (640 kbit/s at 32 kHz).</summary>
        static constexpr std::size_t MaxLameTagBytes = 2880;

        std::array<std::byte, MaxFrameBytes>   buffer_; // the tag placeholder and the flushed bytes
        std::array<std::byte, MaxLameTagBytes> tag_;    // the LAME tag frame, kept for the writer
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
