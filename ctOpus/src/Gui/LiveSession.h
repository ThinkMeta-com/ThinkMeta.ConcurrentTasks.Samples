// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>

#include "Audio.h"
#include "FrameCodec.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui {

    class SignalBuffer;

    enum class SourceKind { TestSignal, WavFile, Device };

    struct LiveConfig {
        SourceKind          source         = SourceKind::TestSignal;
        wchar_t             captureId[256] = {};      // empty: the default capture device
        const SignalBuffer* signal         = nullptr; // TestSignal and WavFile (must outlive the session)
        bool                render         = false;
        wchar_t             renderId[256]  = {}; // empty: the default render device
        EncoderKind         engine         = EncoderKind::CtOpus;
        bool                verify         = false; // run libopus on the same input and compare the packets
        std::int32_t        frameSize      = 240;   // 120, 240, 480 or 960 (2.5 to 20 ms)
        Application         application    = Application::RestrictedLowDelay;
        std::int32_t        bitrate        = 128000;
        std::int32_t        complexity     = 10;
        bool                lowLatency     = true; // IAudioClient3 minimum periods where possible
        bool                mmcss          = true;
        bool                exclusive      = false; // WASAPI exclusive mode for capture and playback (falls back to shared)
        bool                silentOutput   = false; // run the whole playback path but write silence to the device (smoke tests)
    };

    /// <summary>What the UI shows of a live session; copied out under a lock.</summary>
    struct LiveSnapshot {
        static constexpr std::int32_t HistorySize  = 512; // frames
        static constexpr std::int32_t WaveformSize = 1024;

        bool    running        = false;
        wchar_t status[256]    = {};
        wchar_t engineName[32] = {};

        StreamSetup capture;
        bool        captureSimulated = false; // a file or the test signal paced by a timer
        StreamSetup render;
        bool        rendering           = false;
        double      renderStreamLatency = 0;
        double      frameMs             = 0;
        double      lookaheadMs         = 0;
        double      jitterBufferMs      = 0; // the target fill of the playback queue (48 kHz frames as ms)
        double      fillMs              = 0; // the smoothed fill the drift control sees
        double      driftPpm            = 0; // playback resampler ratio / nominal - 1
        double      captureResampleMs   = 0; // LiveResampler delay on the capture side (0: device at 48 kHz)
        double      renderResampleMs    = 0; // LiveResampler delay on the playback side

        std::int64_t frames          = 0;
        std::int64_t misses          = 0; // encoded later than the packet's delivery plus one frame
        std::int64_t inputOverruns   = 0; // input dropped (ring full or backlog cut)
        std::int64_t discontinuities = 0; // reported by the capture device
        std::int64_t underruns       = 0; // render starved
        std::int64_t driftSkips      = 0; // emergency cuts of the playback queue (far above the target)
        std::int64_t verified        = 0;
        std::int64_t mismatches      = 0;
        float        inputDb         = -100;
        float        outputDb        = -100;

        // per encoded frame, ring with the newest entry at encodeHead - 1
        std::int32_t encodeCount              = 0;
        std::int32_t encodeHead               = 0;
        float        ageMs[HistorySize]       = {}; // encode start minus the time of the frame's last sample
        float        readyMs[HistorySize]     = {}; // encode end minus the time of the frame's last sample
        float        encodeUs[HistorySize]    = {};
        float        referenceUs[HistorySize] = {}; // libopus on the same frame (verify)
        float        packetBytes[HistorySize] = {};

        // per decoded frame
        std::int32_t decodeCount                = 0;
        std::int32_t decodeHead                 = 0;
        float        decodeUs[HistorySize]      = {};
        float        renderQueueMs[HistorySize] = {}; // queued for playback when the frame was added (ring + device padding)

        std::int32_t waveformHead           = 0;
        float        waveform[WaveformSize] = {}; // the input, decimated
    };

    /// <summary>
    /// One live stream after plan section 3.3: capture thread (WASAPI event-driven, or a timer for
    /// a file) -> ring buffer -> encode task on the ConcurrentTasks scheduler -> monitor task
    /// (libopus decoder) -> playback queue -> LiveResampler with PI drift control -> render thread
    /// (WASAPI event-driven). Capture devices not at 48 kHz go through a LiveResampler as well.
    /// </summary>
    class LiveSession
    {
    public:
        LiveSession() noexcept;
        LiveSession(const LiveSession&)            = delete;
        LiveSession& operator=(const LiveSession&) = delete;
        ~LiveSession();

        bool Start(const LiveConfig& config, wchar_t* error, std::size_t errorCapacity) noexcept;
        void Stop() noexcept;
        bool Running() const noexcept;

        void Snapshot(LiveSnapshot& snapshot) const noexcept;

        /// <summary>Applied by the encode task before its next frame (to both engines when verifying).</summary>
        void SetBitrate(std::int32_t bitrate) noexcept;
        void SetComplexity(std::int32_t complexity) noexcept;

    private:
        struct Impl;
        Impl* impl_ = nullptr;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui
