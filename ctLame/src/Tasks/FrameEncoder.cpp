// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "FrameEncoder.h"

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    FrameEncoder::FrameEncoder(Mp3File& mp3, Encoder& encoder) noexcept : mp3_(mp3), encoder_(encoder)
    {}

    void FrameEncoder::Begin() noexcept
    {
        mp3_.Append(buffer_.data(), encoder_.TakeHeaderBytes(buffer_.data(), sizeof(buffer_)));
    }

    void FrameEncoder::EncodeFrame(FrameData& frame) noexcept
    {
        if (error_ != 0)
            return;
        if (!encoder_.Encode(frame)) {
            error_ = -2;
            return;
        }
        if (frame.outputSize < 0 || frame.outputSize > MaxFrameBytes) {
            error_ = -1;
            return;
        }
        mp3_.Append(frame.output.data(), frame.outputSize);
    }

    void FrameEncoder::Finish(std::int32_t encoderPadding, std::int32_t radioGain, std::int32_t error) noexcept
    {
        if (error_ == 0)
            error_ = error;
        if (error_ == 0) {
            encoder_.SetEncoderPadding(encoderPadding);
            encoder_.SetRadioGain(radioGain);
            const std::int32_t bytes = encoder_.Flush(buffer_.data(), sizeof(buffer_));
            if (bytes < 0)
                error_ = bytes;
            else
                mp3_.Append(buffer_.data(), bytes);
        }

        if (error_ != 0) {
            mp3_.EndAfterError();
            return;
        }
        // write_xing_frame(gf, outf, id3v2_size): the frame goes over its placeholder, behind the ID3v2 tag
        const std::size_t size = encoder_.GetLameTagFrame(mp3_.LameTagBuffer(), Mp3File::MaxLameTagBytes);
        mp3_.End(size <= Mp3File::MaxLameTagBytes ? static_cast<std::uint32_t>(size) : 0);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
