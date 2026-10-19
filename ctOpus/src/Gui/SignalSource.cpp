// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "Resampler.h"
#include "SignalSource.h"
#include "WavInput.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui {

    namespace {

        constexpr double Pi = 3.14159265358979323846;

        void Fail(wchar_t* error, std::size_t capacity, const wchar_t* message) noexcept
        {
            if (error != nullptr)
                CopyText(error, capacity, message);
        }

    } // namespace

    bool SignalBuffer::MakeTestSignal() noexcept
    {
        constexpr std::int64_t Seconds = 8;
        constexpr std::int64_t Total   = Seconds * SampleRate;
        if (!data_.Allocate(static_cast<std::size_t>(Total * Channels)))
            return false;
        // four chords of two seconds (MIDI note numbers), each note a few harmonics with vibrato
        static constexpr std::int32_t Chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {52, 55, 59}};
        double                        phase[3]     = {};
        std::uint32_t                 noise        = 0x12345678u;
        for (std::int64_t i = 0; i < Total; ++i) {
            const double t     = static_cast<double>(i) / SampleRate;
            const auto   chord = static_cast<std::int32_t>(i / (2 * SampleRate)) & 3;
            const double beat  = std::fmod(t, 0.25);
            const double env   = 0.35 + 0.65 * std::exp(-beat * 9.0);
            double       left  = 0;
            double       right = 0;
            for (std::int32_t n = 0; n < 3; ++n) {
                const double f = 440.0 * std::pow(2.0, (Chords[chord][n] - 69) / 12.0) * (1.0 + 0.003 * std::sin(2 * Pi * 5.0 * t + n));
                phase[n] += 2 * Pi * f / SampleRate;
                if (phase[n] > 2 * Pi)
                    phase[n] -= 2 * Pi;
                const double v   = std::sin(phase[n]) + 0.5 * std::sin(2 * phase[n]) + 0.25 * std::sin(3 * phase[n]) + 0.12 * std::sin(5 * phase[n]);
                const double pan = 0.25 + 0.25 * n;
                left += v * (1.0 - pan);
                right += v * pan;
            }
            noise                                      = noise * 1664525u + 1013904223u;
            const double nz                            = (static_cast<double>(noise >> 8) / 16777216.0 - 0.5) * 0.01;
            data_[static_cast<std::size_t>(2 * i)]     = static_cast<float>(0.18 * env * left + nz);
            data_[static_cast<std::size_t>(2 * i + 1)] = static_cast<float>(0.18 * env * right + nz);
        }
        frames_         = Total;
        sourceRate_     = SampleRate;
        sourceChannels_ = Channels;
        return true;
    }

    bool SignalBuffer::LoadWav(const wchar_t* path, wchar_t* error, std::size_t errorCapacity) noexcept
    {
        InputFile file;
        if (!file.Open(path)) {
            Fail(error, errorCapacity, L"The file cannot be opened.");
            return false;
        }
        WavInput wav;
        if (!wav.Open(file, false, nullptr)) {
            Fail(error, errorCapacity, L"Not a supported WAV file (PCM 8/16/24 bit or 32-bit float).");
            return false;
        }
        std::int32_t channels = wav.GetInfo().channels;
        if (channels > 2)
            channels = wav.SetupDownmix(2, nullptr);
        const std::int32_t rate = wav.GetInfo().rate;
        if (channels < 1 || channels > 2 || rate <= 0) {
            Fail(error, errorCapacity, L"Unsupported channel layout or sample rate.");
            return false;
        }

        Resampler  resampler;
        const bool resample = rate != SampleRate;
        if (resample && !resampler.Initialize(channels, static_cast<std::uint32_t>(rate), SampleRate)) {
            Fail(error, errorCapacity, L"The sample rate cannot be converted to 48 kHz.");
            return false;
        }

        constexpr std::int32_t ReadFrames = 4096;
        constexpr std::int64_t MaxFrames  = MaxSeconds * SampleRate;
        HeapArray<float>       in;
        HeapArray<float>       out;
        HeapArray<float>       all;
        if (!in.Allocate(ReadFrames * 2) || !out.Allocate(ReadFrames * 2 * 8 + 64) || !all.Allocate(static_cast<std::size_t>(SampleRate * Channels))) {
            Fail(error, errorCapacity, L"Out of memory.");
            return false;
        }
        std::int64_t total  = 0;
        auto         append = [&](const float* samples, std::int64_t frames) noexcept {
            if (total + frames > MaxFrames)
                frames = MaxFrames - total;
            if (frames <= 0)
                return true;
            std::size_t need = all.Size();
            while (need < static_cast<std::size_t>((total + frames) * Channels))
                need *= 2;
            if (!all.Grow(need))
                return false;
            for (std::int64_t i = 0; i < frames; ++i) {
                const float l                                             = samples[i * channels];
                const float r                                             = channels == 2 ? samples[i * channels + 1] : l;
                all[static_cast<std::size_t>((total + i) * Channels)]     = l;
                all[static_cast<std::size_t>((total + i) * Channels + 1)] = r;
            }
            total += frames;
            return true;
        };

        for (;;) {
            const std::int32_t got = wav.Read(in.Data(), ReadFrames);
            if (got <= 0 || total >= MaxFrames)
                break;
            if (!resample) {
                if (!append(in.Data(), got)) {
                    Fail(error, errorCapacity, L"Out of memory.");
                    return false;
                }
                continue;
            }
            std::uint32_t consumed = 0;
            while (consumed < static_cast<std::uint32_t>(got)) {
                std::uint32_t inLen  = static_cast<std::uint32_t>(got) - consumed;
                std::uint32_t outLen = ReadFrames * 8;
                resampler.ProcessInterleaved(in.Data() + consumed * channels, inLen, out.Data(), outLen);
                consumed += inLen;
                if (!append(out.Data(), outLen)) {
                    Fail(error, errorCapacity, L"Out of memory.");
                    return false;
                }
                if (inLen == 0 && outLen == 0)
                    break;
            }
        }
        if (total < SampleRate / 10) {
            Fail(error, errorCapacity, L"The file holds less than 0.1 s of audio.");
            return false;
        }
        data_           = std::move(all);
        frames_         = total;
        sourceRate_     = rate;
        sourceChannels_ = wav.GetInfo().inputChannels;
        return true;
    }

    void SignalBuffer::Read(std::int64_t position, float* out, std::int32_t frames) const noexcept
    {
        if (frames_ == 0) {
            std::memset(out, 0, static_cast<std::size_t>(frames) * Channels * sizeof(float));
            return;
        }
        std::int64_t at = position % frames_;
        for (std::int32_t done = 0; done < frames;) {
            std::int64_t chunk = frames_ - at;
            if (chunk > frames - done)
                chunk = frames - done;
            std::memcpy(out + static_cast<std::size_t>(done) * Channels, data_.Data() + at * Channels,
                        static_cast<std::size_t>(chunk) * Channels * sizeof(float));
            done += static_cast<std::int32_t>(chunk);
            at = 0;
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui
