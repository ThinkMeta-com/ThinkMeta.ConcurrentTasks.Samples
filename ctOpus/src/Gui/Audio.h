// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

#include <Audioclient.h>
#include <mmdeviceapi.h>

#include <cstddef>
#include <cstdint>

#include "Common.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui {

    struct AudioDevice {
        wchar_t id[256];
        wchar_t name[160];
    };

    /// <summary>The active endpoints of one direction; returns how many were stored (COM must be initialised).</summary>
    std::int32_t EnumerateAudioDevices(bool capture, AudioDevice* devices, std::int32_t capacity) noexcept;

    /// <summary>The sample formats a stream can run with (converted to and from float in WasapiStream).</summary>
    enum class SampleKind { Float32, Int32, Int24In32, Int24Packed, Int16 };

    const wchar_t* SampleKindName(SampleKind kind) noexcept;

    /// <summary>The periods a device offers, queried before a stream is initialised.</summary>
    struct DevicePeriods {
        bool         valid               = false; // IAudioClient::GetDevicePeriod (the exclusive-mode periods)
        double       exclusiveMinMs      = 0;
        double       exclusiveDefaultMs  = 0;
        bool         engineValid         = false; // IAudioClient3::GetSharedModeEnginePeriod for the mix format
        double       engineMinMs         = 0;
        double       engineDefaultMs     = 0;
        double       engineFundamentalMs = 0;
        std::int32_t mixRate             = 0;
    };

    /// <summary>How a WASAPI stream was set up, for the status line and the latency report.</summary>
    struct StreamSetup {
        bool          exclusiveRequested = false;
        bool          exclusive          = false;
        bool          lowLatency         = false; // shared with the minimum period of IAudioClient3
        bool          converted          = false; // shared with AUTOCONVERTPCM (mix format not usable directly)
        SampleKind    kind               = SampleKind::Float32;
        std::int32_t  deviceChannels     = 0;
        std::int32_t  deviceRate         = 0;
        std::int32_t  periodFrames       = 0; // device frames
        std::int32_t  bufferFrames       = 0;
        double        streamLatencyMs    = 0;  // IAudioClient::GetStreamLatency
        wchar_t       note[128]          = {}; // why exclusive mode is not used, if it was requested
        DevicePeriods periods;                 // what the device offers

        double PeriodMs() const noexcept { return deviceRate > 0 ? periodFrames * 1000.0 / deviceRate : 0; }
        double BufferMs() const noexcept { return deviceRate > 0 ? bufferFrames * 1000.0 / deviceRate : 0; }
    };

    /// <summary>"exclusive, int24 in 32, 48000 Hz, 2 ch, period 2.00 ms" and the fallback note.</summary>
    void DescribeSetup(const StreamSetup& setup, wchar_t* text, std::size_t capacity) noexcept;

    /// <summary>The periods of an endpoint (id empty: the default one) without opening a stream; false if it cannot be activated.</summary>
    bool QueryDevicePeriods(bool capture, const wchar_t* id, DevicePeriods& periods) noexcept;

    /// <summary>
    /// "Playback: running exclusive 3.00 ms | exclusive min 3.00 / default 10.00 | shared engine min 10.00 / default 10.00 / fundamental 10.00 ms";
    /// running nullptr: only what the device offers.
    /// </summary>
    void DescribePeriods(const wchar_t* label, const DevicePeriods& periods, const StreamSetup* running, wchar_t* text, std::size_t capacity) noexcept;

    /// <summary>
    /// An event-driven WASAPI stream (AUDCLNT_STREAMFLAGS_EVENTCALLBACK) that delivers or takes
    /// interleaved stereo float at the device rate. Shared mode uses the mix format (optionally
    /// with the smallest engine period of IAudioClient3); exclusive mode negotiates 48 kHz first,
    /// then the device rate, each as float32, int32, int24 in 32, packed int24 and int16, with the
    /// minimum device period, and falls back to shared mode if the device is busy or refuses.
    /// Open, use and close it on one MTA thread.
    /// </summary>
    class WasapiStream
    {
    public:
        WasapiStream() noexcept                      = default;
        WasapiStream(const WasapiStream&)            = delete;
        WasapiStream& operator=(const WasapiStream&) = delete;
        ~WasapiStream() { Close(); }

        /// <summary>id nullptr or empty: the default endpoint of the direction (console role).</summary>
        bool Open(bool capture, const wchar_t* id, bool exclusive, bool lowLatency, wchar_t* error, std::size_t errorCapacity) noexcept;
        void Close() noexcept;
        bool Start() noexcept;
        void Stop() noexcept;

        HANDLE             Event() const noexcept { return event_.Get(); }
        const StreamSetup& Setup() const noexcept { return setup_; }

        // --- capture ---------------------------------------------------------------------------

        class CaptureSink
        {
        public:
            /// <summary>One packet as stereo frames at the device rate; time is the QPC time (100 ns) of its first frame.</summary>
            virtual void OnPacket(const float* stereo, std::int32_t frames, std::int64_t time, bool discontinuity) noexcept = 0;

        protected:
            ~CaptureSink() = default;
        };

        /// <summary>Hands all pending packets to sink; false if the device failed.</summary>
        bool DrainCapture(CaptureSink& sink) noexcept;

        // --- render ----------------------------------------------------------------------------

        class RenderSource
        {
        public:
            /// <summary>Fills frames stereo frames at the device rate; padding is what the device still has queued (device frames).</summary>
            virtual void Pull(float* stereo, std::int32_t frames, std::int32_t padding) noexcept = 0;

        protected:
            ~RenderSource() = default;
        };

        /// <summary>Tops up the device buffer from source (exclusive: one whole buffer per event); false if the device failed.</summary>
        bool FillRender(RenderSource& source) noexcept;

    private:
        bool OpenExclusive(IMMDevice* device, const WAVEFORMATEX* mix, HRESULT& hr) noexcept;
        bool OpenShared(IMMDevice* device, const WAVEFORMATEX* mix, bool lowLatency, HRESULT& hr) noexcept;

        ComPtr<IAudioClient>        client_;
        ComPtr<IAudioCaptureClient> capture_;
        ComPtr<IAudioRenderClient>  render_;
        Handle                      event_;
        StreamSetup                 setup_;
        bool                        started_ = false;
        HeapArray<float>            scratch_; // stereo frames of one buffer
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui
