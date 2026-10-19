// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "Audio.h"
#include "Common.h"
#include "FrameCodec.h"
#include "LiveSession.h"
#include "Resampler.h"
#include "Runtime.h"
#include "SchedulerHandle.h"
#include "SignalSource.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui {

    namespace {

        constexpr std::int32_t SimulatedPeriod = 120;  // 2.5 ms: the period of the timer that plays a file in real time
        constexpr std::int32_t MaxDecodeFrames = 5760; // 120 ms, the most a packet can hold
        constexpr std::int64_t MaxBacklog      = 9600; // 200 ms of input waiting for the encoder: cut back to one frame
        constexpr std::int32_t WaveDecimation  = 8;
        constexpr std::int64_t RenderChunk     = 2048; // 48 kHz frames handed to the playback resampler at once

        // A capture packet as the capture thread hands it to the encode task.
        struct PacketInfo {
            std::int64_t endIndex;  // ring index after the packet's last frame
            std::int64_t lastTime;  // QPC time (100 ns) of the packet's last frame
            std::int64_t delivered; // when the capture thread pushed it
        };

        struct EncodedPacket {
            std::int32_t bytes;
            std::uint8_t data[MaxPacketBytes];
        };

        float LevelDb(const float* pcm, std::int32_t samples) noexcept
        {
            double sum = 0;
            for (std::int32_t i = 0; i < samples; ++i)
                sum += static_cast<double>(pcm[i]) * pcm[i];
            const double rms = samples > 0 ? std::sqrt(sum / samples) : 0;
            return rms > 1e-5 ? static_cast<float>(20.0 * std::log10(rms)) : -100.0f;
        }

        class SrwExclusive
        {
        public:
            explicit SrwExclusive(SRWLOCK& lock) noexcept : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
            SrwExclusive(const SrwExclusive&)            = delete;
            SrwExclusive& operator=(const SrwExclusive&) = delete;
            ~SrwExclusive() { ReleaseSRWLockExclusive(&lock_); }

        private:
            SRWLOCK& lock_;
        };

    } // namespace

    struct LiveSession::Impl final : WasapiStream::CaptureSink, WasapiStream::RenderSource {
        LiveConfig config;

        // the encoders: the engine under test and, when verifying, libopus on the same input
        std::unique_ptr<FrameEncoder> encoder;
        std::unique_ptr<FrameEncoder> reference;
        std::unique_ptr<FrameDecoder> decoder;
        std::int32_t                  lookahead = 0;

        // capture thread -> encode task
        FrameRing                         input;
        SpscQueue<PacketInfo, 4096>       packets;
        Synchronization::AutoResetEvent_s inputReady;
        // encode task -> monitor task
        SpscQueue<EncodedPacket, 256>     encoded;
        Synchronization::AutoResetEvent_s encodedReady;
        // monitor task -> render thread
        FrameRing                 output;
        std::atomic<std::int32_t> renderPaddingUs{0}; // what the device still had queued at the last event
        std::atomic<std::int64_t> renderTarget{0};    // the target fill of output (48 kHz frames)
        std::atomic<double>       driftPpm{0};
        std::atomic<double>       fillFrames{0};

        // capture side: device rate -> 48 kHz (only if the device does not run at 48 kHz)
        LiveResampler    captureResampler;
        bool             captureResample = false;
        HeapArray<float> captureOut;

        // render thread only: 48 kHz -> device rate, the drift control of the playback queue
        LiveResampler             renderResampler;
        HeapArray<float>          renderIn;
        bool                      prefilling   = true;
        double                    fillSmoothed = 0;
        double                    integral     = 0;
        std::atomic<std::int64_t> underruns{0};
        std::atomic<std::int64_t> driftSkips{0};
        std::atomic<std::int64_t> overruns{0};
        std::atomic<std::int64_t> discontinuities{0};

        std::atomic<bool>         stop{false};
        std::atomic<std::int32_t> pendingBitrate{0};
        std::atomic<std::int32_t> pendingComplexity{-1};

        Shared::SchedulerHandle scheduler;
        TaskExitCounter         exits;
        Handle                  sourceThread;
        Handle                  renderThread;
        Handle                  sourceOpened;
        Handle                  renderOpened;
        bool                    sourceOk         = false;
        bool                    renderOk         = false;
        wchar_t                 sourceError[160] = {};
        wchar_t                 renderError[160] = {};
        StreamSetup             captureSetup;
        StreamSetup             renderSetup;

        // the tasks
        class EncodeTask final : public Tasks::Task
        {
        public:
            explicit EncodeTask(Impl& impl) noexcept : impl_(impl) {}
            void Run() noexcept override { impl_.EncodeLoop(); }

        private:
            Impl& impl_;
        };

        class MonitorTask final : public Tasks::Task
        {
        public:
            explicit MonitorTask(Impl& impl) noexcept : impl_(impl) {}
            void Run() noexcept override { impl_.MonitorLoop(); }

        private:
            Impl& impl_;
        };

        EncodeTask  encodeTask{*this};
        MonitorTask monitorTask{*this};
        bool        tasksAdded = false;

        // buffers of the tasks
        float        framePcm[2 * 960]               = {};
        std::uint8_t referencePacket[MaxPacketBytes] = {};
        float        decodedPcm[2 * MaxDecodeFrames] = {};
        float        waveMax                         = 0;
        std::int32_t waveCount                       = 0;

        // shared with the UI
        mutable SRWLOCK lock = SRWLOCK_INIT;
        LiveSnapshot    snap;

        // --- capture side ------------------------------------------------------------------------

        void OnPacket(const float* stereo, std::int32_t frames, std::int64_t time, bool discontinuity) noexcept override
        {
            if (discontinuity)
                discontinuities.fetch_add(1, std::memory_order_relaxed);
            if (!captureResample) {
                PushInput(stereo, frames, time + FramesTo100ns(frames - 1));
                return;
            }
            // to 48 kHz; the newest output frame lags the newest input frame by the resampler's delay
            const std::int64_t lastTime = time + FramesTo100ns(frames - 1, captureSetup.deviceRate) - FramesTo100ns(captureResampler.OutputLatency());
            std::uint32_t      consumed = 0;
            std::int64_t       produced = 0;
            const auto         room     = static_cast<std::uint32_t>(captureOut.Size() / Channels);
            while (consumed < static_cast<std::uint32_t>(frames) && produced < room) {
                std::uint32_t inLen  = static_cast<std::uint32_t>(frames) - consumed;
                std::uint32_t outLen = room - static_cast<std::uint32_t>(produced);
                captureResampler.ProcessInterleaved(stereo + static_cast<std::size_t>(consumed) * Channels, inLen, captureOut.Data() + produced * Channels,
                                                    outLen);
                consumed += inLen;
                produced += outLen;
                if (inLen == 0 && outLen == 0)
                    break;
            }
            if (produced > 0)
                PushInput(captureOut.Data(), produced, lastTime);
        }

        void PushInput(const float* stereo, std::int64_t frames, std::int64_t lastTime) noexcept
        {
            // the packet's info goes first, so the encode task never sees frames without it
            const std::int64_t w    = input.WriteIndex();
            const std::int64_t free = input.Capacity() - input.Fill();
            const std::int64_t n    = frames < free ? frames : free;
            if (n < frames)
                overruns.fetch_add(1, std::memory_order_relaxed);
            if (n > 0) {
                if (!packets.Push({w + n, lastTime, Now100ns()})) {
                    overruns.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                input.Write(stereo, n);
            }
            inputReady.Set();
        }

        static unsigned long __stdcall SourceThreadProc(void* param) noexcept
        {
            static_cast<Impl*>(param)->SourceLoop();
            return 0;
        }

        void SourceLoop() noexcept
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            HANDLE mmcss = config.mmcss ? EnterProAudio() : nullptr;
            if (config.source == SourceKind::Device)
                CaptureDevice();
            else
                PlaySignal();
            LeaveProAudio(mmcss);
            CoUninitialize();
        }

        void CaptureDevice() noexcept
        {
            WasapiStream stream;
            sourceOk = stream.Open(true, config.captureId, config.exclusive, config.lowLatency, sourceError, sizeof(sourceError) / sizeof(sourceError[0]));
            if (sourceOk) {
                captureSetup    = stream.Setup();
                captureResample = captureSetup.deviceRate != SampleRate;
                if (captureResample) {
                    const std::int64_t room = (captureSetup.bufferFrames + 4096) * static_cast<std::int64_t>(SampleRate) / captureSetup.deviceRate + 256;
                    sourceOk                = captureResampler.Initialize(Channels, static_cast<std::uint32_t>(captureSetup.deviceRate), SampleRate)
                        && captureOut.Allocate(static_cast<std::size_t>(room * Channels));
                    if (!sourceOk)
                        CopyText(sourceError, sizeof(sourceError) / sizeof(sourceError[0]), L"The capture resampler cannot be set up.");
                }
                sourceOk = sourceOk && stream.Start();
            }
            SetEvent(sourceOpened.Get());
            if (!sourceOk)
                return;
            while (!stop.load(std::memory_order_acquire)) {
                if (WaitForSingleObject(stream.Event(), 200) != WAIT_OBJECT_0)
                    continue;
                if (!stream.DrainCapture(*this)) {
                    SrwExclusive guard(lock);
                    CopyText(snap.status, sizeof(snap.status) / sizeof(snap.status[0]), L"The capture device failed (unplugged or format changed).");
                    break;
                }
            }
            stream.Stop();
        }

        // A file or the test signal in real time: a high-resolution timer delivers a packet of
        // SimulatedPeriod frames on an absolute schedule, like a capture device with that period.
        void PlaySignal() noexcept
        {
            Handle timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
            sourceOk                  = static_cast<bool>(timer) && config.signal != nullptr;
            captureSetup              = {};
            captureSetup.periodFrames = SimulatedPeriod;
            captureSetup.deviceRate   = SampleRate;
            if (!sourceOk)
                CopyText(sourceError, sizeof(sourceError) / sizeof(sourceError[0]), L"No high-resolution timer or no signal.");
            SetEvent(sourceOpened.Get());
            if (!sourceOk)
                return;
            float              packet[2 * SimulatedPeriod];
            const std::int64_t period   = FramesTo100ns(SimulatedPeriod);
            const std::int64_t start    = Now100ns();
            std::int64_t       position = 0;
            for (std::int64_t n = 1; !stop.load(std::memory_order_acquire); ++n) {
                const std::int64_t due  = start + n * period;
                const std::int64_t wait = due - Now100ns();
                if (wait > 0) {
                    LARGE_INTEGER relative;
                    relative.QuadPart = -wait;
                    SetWaitableTimer(timer.Get(), &relative, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer.Get(), 100);
                }
                config.signal->Read(position, packet, SimulatedPeriod);
                position += SimulatedPeriod;
                OnPacket(packet, SimulatedPeriod, due - period, false);
            }
        }

        // --- encode task ---------------------------------------------------------------------------

        void EncodeLoop() noexcept
        {
            const std::int32_t frameSize   = config.frameSize;
            const std::int64_t frameLength = FramesTo100ns(frameSize);
            PacketInfo         current     = {};
            bool               haveCurrent = false;
            EncodedPacket      out;
            for (;;) {
                inputReady.Wait();
                if (stop.load(std::memory_order_acquire))
                    break;
                const std::int32_t bitrate = pendingBitrate.exchange(0, std::memory_order_acq_rel);
                if (bitrate > 0) {
                    encoder->SetBitrate(bitrate);
                    if (reference)
                        reference->SetBitrate(bitrate);
                }
                const std::int32_t complexity = pendingComplexity.exchange(-1, std::memory_order_acq_rel);
                if (complexity >= 0) {
                    encoder->SetComplexity(complexity);
                    if (reference)
                        reference->SetComplexity(complexity);
                }
                // the encoder is too slow (or was stalled): drop input instead of piling up latency
                const std::int64_t backlog = input.Available();
                if (backlog > MaxBacklog) {
                    const std::int64_t skip = backlog - backlog % frameSize - frameSize;
                    input.Read(nullptr, skip);
                    overruns.fetch_add(1, std::memory_order_relaxed);
                }
                while (input.Available() >= frameSize && !stop.load(std::memory_order_relaxed)) {
                    input.Read(framePcm, frameSize);
                    const std::int64_t end = input.ReadIndex();
                    while (!haveCurrent || current.endIndex < end) {
                        const PacketInfo* next = packets.Front();
                        if (next == nullptr)
                            break;
                        current = *next;
                        packets.Pop();
                        haveCurrent = true;
                    }
                    const std::int64_t lastTime = current.lastTime - FramesTo100ns(current.endIndex - end);

                    const std::int64_t t0             = Now100ns();
                    out.bytes                         = encoder->Encode(framePcm, frameSize, out.data, MaxPacketBytes);
                    const std::int64_t t1             = Now100ns();
                    std::int32_t       referenceBytes = 0;
                    if (reference)
                        referenceBytes = reference->Encode(framePcm, frameSize, referencePacket, MaxPacketBytes);
                    const std::int64_t t2   = Now100ns();
                    const bool         miss = t1 > current.delivered + frameLength;
                    const bool         same = reference && referenceBytes == out.bytes && out.bytes > 0
                        && std::memcmp(referencePacket, out.data, static_cast<std::size_t>(out.bytes)) == 0;
                    if (out.bytes > 0 && encoded.Push(out))
                        encodedReady.Set();

                    const float        level = LevelDb(framePcm, 2 * frameSize);
                    SrwExclusive       guard(lock);
                    const std::int32_t at = snap.encodeHead;
                    snap.ageMs[at]        = static_cast<float>(ToMs(t0 - lastTime));
                    snap.readyMs[at]      = static_cast<float>(ToMs(t1 - lastTime));
                    snap.encodeUs[at]     = static_cast<float>((t1 - t0) / 10.0);
                    snap.referenceUs[at]  = reference ? static_cast<float>((t2 - t1) / 10.0) : 0.0f;
                    snap.packetBytes[at]  = static_cast<float>(out.bytes > 0 ? out.bytes : 0);
                    snap.encodeHead       = (at + 1) % LiveSnapshot::HistorySize;
                    if (snap.encodeCount < LiveSnapshot::HistorySize)
                        ++snap.encodeCount;
                    ++snap.frames;
                    if (miss)
                        ++snap.misses;
                    if (reference) {
                        ++snap.verified;
                        if (!same)
                            ++snap.mismatches;
                    }
                    snap.inputDb = level;
                    for (std::int32_t i = 0; i < frameSize; ++i) {
                        const float v = 0.5f * (framePcm[2 * i] + framePcm[2 * i + 1]);
                        if (std::fabs(v) > std::fabs(waveMax))
                            waveMax = v;
                        if (++waveCount == WaveDecimation) {
                            snap.waveform[snap.waveformHead] = waveMax;
                            snap.waveformHead                = (snap.waveformHead + 1) % LiveSnapshot::WaveformSize;
                            waveMax                          = 0;
                            waveCount                        = 0;
                        }
                    }
                }
            }
        }

        // --- monitor task: decode for playback ---------------------------------------------------------

        void MonitorLoop() noexcept
        {
            for (;;) {
                encodedReady.Wait();
                for (const EncodedPacket* packet = encoded.Front(); packet != nullptr; packet = encoded.Front()) {
                    const std::int64_t t0 = Now100ns();
                    const std::int32_t n  = decoder->Decode(packet->data, packet->bytes, decodedPcm, MaxDecodeFrames);
                    const std::int64_t t1 = Now100ns();
                    encoded.Pop();
                    if (n <= 0)
                        continue;
                    double queuedMs = 0;
                    if (config.render) {
                        queuedMs = output.Fill() * 1000.0 / SampleRate + renderPaddingUs.load(std::memory_order_relaxed) / 1000.0;
                        output.Write(decodedPcm, n);
                    }
                    const float        level = LevelDb(decodedPcm, 2 * n);
                    SrwExclusive       guard(lock);
                    const std::int32_t at  = snap.decodeHead;
                    snap.decodeUs[at]      = static_cast<float>((t1 - t0) / 10.0);
                    snap.renderQueueMs[at] = static_cast<float>(queuedMs);
                    snap.decodeHead        = (at + 1) % LiveSnapshot::HistorySize;
                    if (snap.decodeCount < LiveSnapshot::HistorySize)
                        ++snap.decodeCount;
                    snap.outputDb = level;
                }
                if (stop.load(std::memory_order_acquire))
                    break;
            }
        }

        // --- render thread -------------------------------------------------------------------------------

        static unsigned long __stdcall RenderThreadProc(void* param) noexcept
        {
            static_cast<Impl*>(param)->RenderLoop();
            return 0;
        }

        void RenderLoop() noexcept
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            HANDLE mmcss = config.mmcss ? EnterProAudio() : nullptr;
            {
                WasapiStream stream;
                renderOk = stream.Open(false, config.renderId, config.exclusive, config.lowLatency, renderError, sizeof(renderError) / sizeof(renderError[0]));
                if (renderOk) {
                    renderSetup = stream.Setup();
                    // always through the LiveResampler, even at 48 kHz: it is the drift actuator
                    renderOk = renderResampler.Initialize(Channels, SampleRate, static_cast<std::uint32_t>(renderSetup.deviceRate))
                        && renderIn.Allocate(static_cast<std::size_t>(RenderChunk) * Channels);
                    if (!renderOk)
                        CopyText(renderError, sizeof(renderError) / sizeof(renderError[0]), L"The playback resampler cannot be set up.");
                    renderTarget.store(TargetFill(SimulatedPeriod, SampleRate), std::memory_order_relaxed);
                    renderOk = renderOk && stream.Start();
                }
                SetEvent(renderOpened.Get());
                while (renderOk && !stop.load(std::memory_order_acquire)) {
                    if (WaitForSingleObject(stream.Event(), 200) != WAIT_OBJECT_0)
                        continue;
                    if (!stream.FillRender(*this))
                        break;
                }
                stream.Stop();
            }
            LeaveProAudio(mmcss);
            CoUninitialize();
        }

        /// <summary>
        /// The target fill of the playback queue in 48 kHz frames: one capture burst (a capture
        /// period, at least a frame), one render period and 1 ms of reserve for jitter.
        /// </summary>
        std::int64_t TargetFill(std::int32_t capturePeriod, std::int32_t captureRate) const noexcept
        {
            const std::int64_t burst = captureRate > 0 ? static_cast<std::int64_t>(capturePeriod) * SampleRate / captureRate : 0;
            const std::int64_t render
                = renderSetup.deviceRate > 0 ? static_cast<std::int64_t>(renderSetup.periodFrames) * SampleRate / renderSetup.deviceRate : 0;
            return (burst > config.frameSize ? burst : config.frameSize) + render + SampleRate / 1000;
        }

        void ResetDriftControl(double fill) noexcept
        {
            fillSmoothed = fill;
            integral     = 0;
            renderResampler.SetRatio(renderResampler.NominalRatio());
            driftPpm.store(0, std::memory_order_relaxed);
        }

        // The playback queue with drift control: playback starts (and restarts after an underrun)
        // once the target fill is queued; every underrun raises the target by a frame (up to
        // 100 ms). A PI controller on the smoothed fill sets the ratio of the playback resampler,
        // so the queue stays at the target although capture (or the timer) and playback run on
        // different clocks. Cutting the queue is only an emergency brake for large jumps.
        void Pull(float* stereo, std::int32_t frames, std::int32_t padding) noexcept override
        {
            const std::int32_t rate = renderSetup.deviceRate;
            renderPaddingUs.store(static_cast<std::int32_t>(static_cast<std::int64_t>(padding) * 1'000'000 / rate), std::memory_order_relaxed);
            std::int64_t       available = output.Available();
            const std::int64_t target    = renderTarget.load(std::memory_order_relaxed);
            std::int32_t       produced  = 0;
            if (prefilling && available >= target) {
                prefilling = false;
                ResetDriftControl(static_cast<double>(available));
            }
            if (!prefilling) {
                if (available > 2 * target + SampleRate / 20) {
                    // emergency brake: far too much queued (a stall upstream), cut to the target
                    output.Read(nullptr, available - target);
                    available = target;
                    driftSkips.fetch_add(1, std::memory_order_relaxed);
                    ResetDriftControl(static_cast<double>(target));
                }

                // the PI controller, in 48 kHz frames: d(fill)/dt = 48000 * (drift - u), so with
                // u = (Kp * error + Ki * integral) / 48000 the loop is second order with
                // omega = sqrt(Ki) = 0.5 rad/s and damping 1 (settles in some seconds); the fill
                // is smoothed over about half a second against the bursts of capture and decode.
                constexpr double Kp    = 1.0;
                constexpr double Ki    = 0.25;
                constexpr double Tau   = 0.5;
                constexpr double MaxU  = 0.005; // +-0.5 %
                const double     dt    = static_cast<double>(frames) / rate;
                const double     alpha = 1.0 - std::exp(-dt / Tau);
                fillSmoothed += alpha * (static_cast<double>(available) - fillSmoothed);
                const double error       = fillSmoothed - static_cast<double>(target);
                const double maxIntegral = MaxU * SampleRate / Ki;
                integral += error * dt;
                integral = integral > maxIntegral ? maxIntegral : integral < -maxIntegral ? -maxIntegral : integral;
                double u = (Kp * error + Ki * integral) / SampleRate;
                u        = u > MaxU ? MaxU : u < -MaxU ? -MaxU : u;
                // too full: consume more input per output frame
                renderResampler.SetRatio(renderResampler.NominalRatio() * (1.0 + u));
                driftPpm.store(u * 1e6, std::memory_order_relaxed);
                fillFrames.store(fillSmoothed, std::memory_order_relaxed);

                while (produced < frames) {
                    const std::int64_t queued = output.Available();
                    if (queued == 0)
                        break;
                    auto inLen = static_cast<std::uint32_t>(queued < RenderChunk ? queued : RenderChunk);
                    output.Peek(renderIn.Data(), inLen);
                    auto outLen = static_cast<std::uint32_t>(frames - produced);
                    renderResampler.ProcessInterleaved(renderIn.Data(), inLen, stereo + static_cast<std::size_t>(produced) * Channels, outLen);
                    output.Read(nullptr, inLen);
                    produced += static_cast<std::int32_t>(outLen);
                    if (inLen == 0 && outLen == 0)
                        break;
                }
                if (produced < frames) {
                    underruns.fetch_add(1, std::memory_order_relaxed);
                    prefilling = true;
                    if (target < SampleRate / 10)
                        renderTarget.store(target + config.frameSize, std::memory_order_relaxed);
                }
            }
            std::memset(stereo + static_cast<std::size_t>(produced) * Channels, 0, static_cast<std::size_t>(frames - produced) * Channels * sizeof(float));
            if (config.silentOutput)
                std::memset(stereo, 0, static_cast<std::size_t>(frames) * Channels * sizeof(float));
        }

        // --- control -----------------------------------------------------------------------------------

        bool Start(wchar_t* error, std::size_t errorCapacity) noexcept
        {
            EncoderSettings settings;
            settings.channels    = Channels;
            settings.application = config.application;
            settings.bitrate     = config.bitrate;
            settings.complexity  = config.complexity;
            encoder.reset(CreateEncoder(config.engine, settings));
            if (config.verify && config.engine == EncoderKind::CtOpus)
                reference.reset(CreateLibopusEncoder(settings));
            decoder.reset(CreateLibopusDecoder(Channels));
            if (!encoder || !decoder || (config.verify && config.engine == EncoderKind::CtOpus && !reference)) {
                CopyText(error, errorCapacity, L"The encoder or decoder could not be created.");
                return false;
            }
            lookahead = encoder->Lookahead();
            if (!input.Allocate(SampleRate) || !output.Allocate(SampleRate)) {
                CopyText(error, errorCapacity, L"Out of memory.");
                return false;
            }

            {
                SrwExclusive guard(lock);
                snap             = {};
                snap.frameMs     = config.frameSize * 1000.0 / SampleRate;
                snap.lookaheadMs = lookahead * 1000.0 / SampleRate;
                CopyText(snap.engineName, sizeof(snap.engineName) / sizeof(snap.engineName[0]), config.engine == EncoderKind::CtOpus ? L"ctOpus" : L"libopus");
            }

            // two scheduler threads: the encode task and the monitor task
            scheduler = CreateAudioScheduler(2, config.mmcss);
            if (!scheduler) {
                CopyText(error, errorCapacity, L"The task scheduler could not be created.");
                return false;
            }
            scheduler->Start();

            if (config.render) {
                renderOpened.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                renderThread.Reset(CreateThread(nullptr, 0, RenderThreadProc, this, 0, nullptr));
                if (!renderThread || WaitForSingleObject(renderOpened.Get(), 5000) != WAIT_OBJECT_0 || !renderOk) {
                    swprintf_s(error, errorCapacity, L"Playback: %ls", renderError[0] != 0 ? renderError : L"the render thread did not start");
                    return false;
                }
            }

            exits.Add(&encodeTask);
            exits.Add(&monitorTask);
            scheduler->AddTask(&encodeTask, TaskPriority::High);
            scheduler->AddTask(&monitorTask, TaskPriority::Normal);
            tasksAdded = true;

            sourceOpened.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            sourceThread.Reset(CreateThread(nullptr, 0, SourceThreadProc, this, 0, nullptr));
            if (!sourceThread || WaitForSingleObject(sourceOpened.Get(), 5000) != WAIT_OBJECT_0 || !sourceOk) {
                swprintf_s(error, errorCapacity, L"Capture: %ls", sourceError[0] != 0 ? sourceError : L"the capture thread did not start");
                return false;
            }

            // the target fill now that the capture period is known
            if (config.render)
                renderTarget.store(TargetFill(captureSetup.periodFrames, captureSetup.deviceRate), std::memory_order_relaxed);

            SrwExclusive guard(lock);
            snap.running             = true;
            snap.capture             = captureSetup;
            snap.captureSimulated    = config.source != SourceKind::Device;
            snap.rendering           = config.render;
            snap.render              = renderSetup;
            snap.renderStreamLatency = renderSetup.streamLatencyMs;
            snap.captureResampleMs   = captureResample ? captureResampler.OutputLatency() * 1000.0 / SampleRate : 0.0;
            snap.renderResampleMs    = config.render && renderSetup.deviceRate > 0 ? renderResampler.OutputLatency() * 1000.0 / renderSetup.deviceRate : 0.0;
            CopyText(snap.status, sizeof(snap.status) / sizeof(snap.status[0]), L"Running.");
            return true;
        }

        void Stop() noexcept
        {
            stop.store(true, std::memory_order_release);
            if (sourceThread)
                WaitForSingleObject(sourceThread.Get(), INFINITE);
            sourceThread.Reset();
            if (tasksAdded) {
                inputReady.Set();
                encodedReady.Set();
                exits.Wait(INFINITE);
                tasksAdded = false;
            }
            if (renderThread)
                WaitForSingleObject(renderThread.Get(), INFINITE);
            renderThread.Reset();
            if (scheduler) {
                scheduler->Stop();
                scheduler.Reset();
            }
            SrwExclusive guard(lock);
            snap.running = false;
        }
    };

    LiveSession::LiveSession() noexcept = default;

    LiveSession::~LiveSession()
    {
        Stop();
    }

    bool LiveSession::Start(const LiveConfig& config, wchar_t* error, std::size_t errorCapacity) noexcept
    {
        Stop();
        impl_ = new (std::nothrow) Impl;
        if (impl_ == nullptr) {
            CopyText(error, errorCapacity, L"Out of memory.");
            return false;
        }
        impl_->config = config;
        if (!impl_->Start(error, errorCapacity)) {
            Stop();
            return false;
        }
        return true;
    }

    void LiveSession::Stop() noexcept
    {
        if (impl_ == nullptr)
            return;
        impl_->Stop();
        delete impl_;
        impl_ = nullptr;
    }

    bool LiveSession::Running() const noexcept
    {
        return impl_ != nullptr;
    }

    void LiveSession::Snapshot(LiveSnapshot& snapshot) const noexcept
    {
        if (impl_ == nullptr) {
            snapshot.running = false;
            return;
        }
        AcquireSRWLockShared(&impl_->lock);
        snapshot = impl_->snap;
        ReleaseSRWLockShared(&impl_->lock);
        snapshot.inputOverruns   = impl_->overruns.load(std::memory_order_relaxed);
        snapshot.discontinuities = impl_->discontinuities.load(std::memory_order_relaxed);
        snapshot.underruns       = impl_->underruns.load(std::memory_order_relaxed);
        snapshot.driftSkips      = impl_->driftSkips.load(std::memory_order_relaxed);
        snapshot.jitterBufferMs  = impl_->renderTarget.load(std::memory_order_relaxed) * 1000.0 / SampleRate;
        snapshot.fillMs          = impl_->fillFrames.load(std::memory_order_relaxed) * 1000.0 / SampleRate;
        snapshot.driftPpm        = impl_->driftPpm.load(std::memory_order_relaxed);
    }

    void LiveSession::SetBitrate(std::int32_t bitrate) noexcept
    {
        if (impl_ != nullptr)
            impl_->pendingBitrate.store(bitrate, std::memory_order_release);
    }

    void LiveSession::SetComplexity(std::int32_t complexity) noexcept
    {
        if (impl_ != nullptr)
            impl_->pendingComplexity.store(complexity, std::memory_order_release);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui
