// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "SampleStream.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "SessionConfig.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    namespace {

        constexpr std::size_t   CommitChunk          = 256 * 1024;           // bytes committed at once
        constexpr std::size_t   DecommitChunk        = 64 * 1024;            // bytes decommitted at once
        constexpr std::uint64_t UnknownInputSamples  = 1ull << 32;           // reservation when the length is unknown
        constexpr std::int32_t  StartZeros           = ENCDELAY - MDCTDELAY; // LAME pads the input with this many 0's
        constexpr std::uint64_t MaxSamplesAfterInput = 4 * FRAMESIZE;        // flush padding up to the last window

        // lame_copy_inbuffer: COPY_AND_TRANSFORM
        template <typename T>
        void Transform(const std::array<std::array<float, 2>, 2>& transform, const T* left, const T* right, std::int32_t count, float scale,
                       std::array<std::array<sample_t, FRAMESIZE>, 2>& out) noexcept
        {
            std::array<std::array<float, 2>, 2> m;
            m[0][0] = scale * transform[0][0];
            m[0][1] = scale * transform[0][1];
            m[1][0] = scale * transform[1][0];
            m[1][1] = scale * transform[1][1];

            for (std::int32_t i = 0; i < count; i++) {
                const sample_t xl = static_cast<sample_t>(left[i]);
                const sample_t xr = static_cast<sample_t>(right[i]);
                const sample_t u  = xl * m[0][0] + xr * m[0][1];
                const sample_t v  = xl * m[1][0] + xr * m[1][1];
                out[0][i]         = u;
                out[1][i]         = v;
            }
        }

        std::size_t RoundUp(std::size_t value, std::size_t chunk) noexcept
        {
            return (value + chunk - 1) / chunk * chunk;
        }

    } // namespace

    SampleStream::~SampleStream()
    {
        for (auto*& base : base_) {
            if (base != nullptr)
                VirtualFree(base, 0, MEM_RELEASE);
            base = nullptr;
        }
    }

    bool SampleStream::Initialize(const SessionConfig& config, std::uint64_t maxInputSamples) noexcept
    {
        for (std::int32_t i = 0; i < 2; ++i)
            for (std::int32_t j = 0; j < 2; ++j)
                transform_[i][j] = config.pcm_transform[i][j];
        channelsIn_ = config.channels_in;
        channels_   = config.channels_out;

        // lame_encode_flush pads with short zeros at scale 1.0; the transform still runs, so a
        // negative scale yields -0.0 like in LAME
        const std::array<std::int16_t, 1>              zeros = {};
        std::array<std::array<sample_t, FRAMESIZE>, 2> zero;
        Transform(transform_, zeros.data(), zeros.data(), 1, 1.0f, zero);
        zero_[0] = zero[0][0];
        zero_[1] = zero[1][0];

        if (!Reserve(StartZeros + (maxInputSamples != 0 ? maxInputSamples : UnknownInputSamples) + MaxSamplesAfterInput))
            return false;

        // the start zeros are +0.0 (LAME clears mfbuf), as fresh memory is
        if (!Grow(StartZeros))
            return false;
        length_ = StartZeros;
        UpdateFramesReady();
        return true;
    }

    bool SampleStream::Reserve(std::uint64_t samples) noexcept
    {
        reserved_ = RoundUp(static_cast<std::size_t>(samples) * sizeof(sample_t), CommitChunk);
        for (std::int32_t ch = 0; ch < channels_; ++ch) {
            base_[ch] = static_cast<sample_t*>(VirtualAlloc(nullptr, reserved_, MEM_RESERVE, PAGE_NOACCESS));
            if (base_[ch] == nullptr)
                return false;
        }
        return true;
    }

    bool SampleStream::Grow(std::uint64_t length) noexcept
    {
        const std::size_t needed = static_cast<std::size_t>(length) * sizeof(sample_t);
        if (needed <= committed_)
            return true;
        if (needed > reserved_)
            return false;

        std::size_t commit = RoundUp(needed, CommitChunk);
        if (commit > reserved_)
            commit = reserved_;
        for (std::int32_t ch = 0; ch < channels_; ++ch) {
            auto* const start = reinterpret_cast<std::byte*>(base_[ch]) + committed_;
            if (VirtualAlloc(start, commit - committed_, MEM_COMMIT, PAGE_READWRITE) == nullptr)
                return false;
        }
        committed_ = commit;
        return true;
    }

    void SampleStream::AddInt(const std::int32_t* left, const std::int32_t* right, std::int32_t count) noexcept
    {
        // input is assumed to be normalized to +/- MAX_INT for full scale
        float const norm = static_cast<float>(1.0 / (1L << (8 * sizeof(std::int32_t) - 16)));

        // the transform works sample by sample, so splitting into buffer-sized pieces does not
        // change anything
        while (count > 0) {
            const std::int32_t n = count < FRAMESIZE ? count : FRAMESIZE;
            Transform(transform_, left, channelsIn_ > 1 ? right : left, n, norm, buffer_);
            const ChannelSamples samples = {buffer_[0].data(), buffer_[1].data()};
            Append(samples, n);
            inputLength_ += static_cast<std::uint64_t>(n);
            left += n;
            right += n;
            count -= n;
        }
    }

    void SampleStream::Append(const ChannelSamples& samples, std::int32_t count) noexcept
    {
        if (failed_ || !Grow(length_ + static_cast<std::uint64_t>(count))) {
            failed_ = true;
            return;
        }
        for (std::int32_t ch = 0; ch < channels_; ++ch)
            std::memcpy(base_[ch] + length_, samples[ch], static_cast<std::size_t>(count) * sizeof(sample_t));
        length_ += static_cast<std::uint64_t>(count);
        UpdateFramesReady();
    }

    // fill_buffer and lame_encode_buffer_sample_t: frame k is encoded as soon as mfbuf holds
    // its window, k * FRAMESIZE + FrameInputSize samples of the stream
    void SampleStream::UpdateFramesReady() noexcept
    {
        if (length_ >= static_cast<std::uint64_t>(FrameInputSize))
            framesReady_ = static_cast<std::int64_t>((length_ - FrameInputSize) / FRAMESIZE) + 1;
    }

    void SampleStream::Finish() noexcept
    {
        if (finished_)
            return;
        finished_ = true;

        // lame_encode_flush: mf_samples_to_encode - POSTDELAY is ENCDELAY + input - FRAMESIZE *
        // (frames so far); the padding makes it whole frames, at least 576 samples
        const std::uint64_t samplesToEncode = ENCDELAY + inputLength_;
        std::int32_t        endPadding      = FRAMESIZE - static_cast<std::int32_t>(samplesToEncode % FRAMESIZE);
        if (endPadding < 576)
            endPadding += FRAMESIZE;
        encoderPadding_ = endPadding;

        // pad until the window of the last frame is complete
        const std::uint64_t frames = (samplesToEncode + static_cast<std::uint64_t>(endPadding)) / FRAMESIZE;
        const std::uint64_t end    = (frames - 1) * FRAMESIZE + FrameInputSize;
        while (length_ < end && !failed_) {
            const std::int32_t n = end - length_ < FRAMESIZE ? static_cast<std::int32_t>(end - length_) : FRAMESIZE;
            for (std::int32_t ch = 0; ch < 2; ++ch)
                for (std::int32_t i = 0; i < n; ++i)
                    buffer_[ch][i] = zero_[ch];
            const ChannelSamples samples = {buffer_[0].data(), buffer_[1].data()};
            Append(samples, n);
        }
    }

    void SampleStream::Window(std::int64_t frame, ChannelSamples& input) const noexcept
    {
        const auto offset = static_cast<std::size_t>(frame) * FRAMESIZE;
        input[0]          = base_[0] + offset;
        input[1]          = channels_ == 2 ? base_[1] + offset : nullptr;
    }

    void SampleStream::Release(std::int64_t frame) noexcept
    {
        // the window of the next frame starts at (frame + 1) * FRAMESIZE
        const std::size_t done  = static_cast<std::size_t>(frame + 1) * FRAMESIZE * sizeof(sample_t);
        const std::size_t bound = done / DecommitChunk * DecommitChunk;
        if (bound <= decommitted_)
            return;
        for (std::int32_t ch = 0; ch < channels_; ++ch)
            VirtualFree(reinterpret_cast<std::byte*>(base_[ch]) + decommitted_, bound - decommitted_, MEM_DECOMMIT);
        decommitted_ = bound;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
