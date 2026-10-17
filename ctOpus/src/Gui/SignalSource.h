// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>

#include "Common.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui {

    /// <summary>
    /// A looped 48 kHz stereo signal in memory: a WAV file (read with the ctOpus WAV reader and
    /// resampled with the ctOpus port of the libopusenc resampler) or a synthetic test signal.
    /// The sources that are not a capture device play it in real time.
    /// </summary>
    class SignalBuffer
    {
    public:
        static constexpr std::int64_t MaxSeconds = 120; // longer files are cut

        /// <summary>A synthetic music-like signal (changing chords, vibrato, a little noise), so the encoder has real work.</summary>
        bool MakeTestSignal() noexcept;

        /// <summary>Loads a WAV file; false with a message in error.</summary>
        bool LoadWav(const wchar_t* path, wchar_t* error, std::size_t errorCapacity) noexcept;

        std::int64_t Frames() const noexcept { return frames_; }
        double       Seconds() const noexcept { return static_cast<double>(frames_) / SampleRate; }
        std::int32_t SourceRate() const noexcept { return sourceRate_; }
        std::int32_t SourceChannels() const noexcept { return sourceChannels_; }

        /// <summary>Copies frames interleaved stereo frames starting at position (wrapping around).</summary>
        void Read(std::int64_t position, float* out, std::int32_t frames) const noexcept;

    private:
        HeapArray<float> data_;
        std::int64_t     frames_         = 0;
        std::int32_t     sourceRate_     = SampleRate;
        std::int32_t     sourceChannels_ = Channels;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui
