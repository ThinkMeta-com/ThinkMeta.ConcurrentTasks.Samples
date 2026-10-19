// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

// ctOpusLiveBench: live encoding strategies for ctOpus. Frames arrive in real time
// (a dispatcher with a high-resolution timer, the streams' phases spread over the frame period);
// every frame is encoded as opus_encode does (the analysis sees the frame only, no look-ahead).
// Latency is measured from the frame's scheduled arrival to the end of its encoding.
//
//   single:  one stream, frames back to back: the encoding time per frame (optionally with the
//            theta RDO helper on another thread)
//   threads: one OS thread per stream (the usual libopus setup)
//   tasks:   one task per stream on the ConcurrentTasks scheduler (one thread per logical processor)

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <immintrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>

#include "CeltBands.h"
#include "EncodeHelpers.h"
#include "HeapArray.h"
#include "OpusDefines.h"
#include "OpusStreamEncoder.h"
#include "Resampler.h"
#include "SchedulerHandle.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

using namespace ThinkMeta::ConcurrentTasks;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus;

namespace {

    struct Options {
        const wchar_t* wav     = nullptr;
        const wchar_t* mode    = L"tasks";
        std::int32_t   streams = 64;
        double         frameMs = 20;
        std::int32_t   comp    = 10;
        std::int32_t   bitrate = 64000;
        double         seconds = 10;
        std::uint32_t  threads = 0; // scheduler threads (tasks mode); 0: one per logical processor
        bool           helper  = false;
        double         spinMs  = -1; // threads mode: spin this long before a frame's scheduled arrival (-1: block only)
        double         gapMs   = 0;  // single mode: pause between the frames
        bool           csv     = false;
    };

    // the input: 16-bit stereo PCM as floats, played as 48 kHz
    HeapArray<float> g_pcm;
    double           g_spinMs      = -1;
    std::int64_t     g_inputFrames = 0;
    double           g_ticksPerMs  = 0;

    std::int64_t Now() noexcept
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return c.QuadPart;
    }

    bool LoadWav(const wchar_t* path) noexcept
    {
        const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER size = {};
        GetFileSizeEx(file, &size);
        HeapArray<std::uint8_t> bytes;
        DWORD                   got = 0; // DWORD: ReadFile out parameter
        const bool              ok
            = bytes.Allocate(static_cast<std::size_t>(size.QuadPart)) && ReadFile(file, bytes.Data(), static_cast<DWORD>(size.QuadPart), &got, nullptr);
        CloseHandle(file);
        if (!ok || got < 44)
            return false;
        // the "data" chunk (16-bit stereo assumed)
        std::size_t pos = 12;
        while (pos + 8 <= got) {
            std::uint32_t chunk = 0;
            std::memcpy(&chunk, bytes.Data() + pos + 4, 4);
            if (std::memcmp(bytes.Data() + pos, "data", 4) == 0) {
                const std::size_t n = (chunk < got - pos - 8 ? chunk : got - pos - 8) / 4;
                if (!g_pcm.Allocate(n * 2))
                    return false;
                const auto* s = reinterpret_cast<const std::int16_t*>(bytes.Data() + pos + 8);
                for (std::size_t i = 0; i < n * 2; ++i)
                    g_pcm[i] = s[i] * (1.f / 32768);
                g_inputFrames = static_cast<std::int64_t>(n);
                return true;
            }
            pos += 8 + chunk + (chunk & 1);
        }
        return false;
    }

    // the theta RDO helper: ThetaHelper of the encode graph (EncodeHelpers.h), here on a task of its own
    class HelperTask : public Tasks::Task
    {
    public:
        explicit HelperTask(ThetaHelper& h) noexcept : h_(h) {}
        void Run() noexcept override { h_.Run(); }

    private:
        ThetaHelper& h_;
    };

    // One live stream: its encoder, its frames' schedule and its measurements.
    struct Stream {
        OpusStreamEncoder       enc;
        std::int64_t            inputPos   = 0;
        std::int64_t            firstTick  = 0; // the scheduled arrival of frame 0
        double                  periodTick = 0;
        std::int32_t            frames     = 0;
        std::int32_t            frameSize  = 0;
        std::uint8_t            packet[1276 * 3];
        HeapArray<std::int64_t> arrival;              // per frame: when the dispatcher handed it over
        HeapArray<float>        latencyMs;            // per frame: handed over -> encoded
        HeapArray<float>        lateMs;               // per frame: scheduled -> handed over (the timer's lateness)
        HeapArray<float>        encodeUs;             // per frame: the encoding alone
        alignas(64) std::atomic<std::int64_t> due{0}; // frames arrived
        std::int64_t                      done        = 0;
        HANDLE                            threadEvent = nullptr;
        Synchronization::AutoResetEvent_s taskEvent;

        std::int64_t Scheduled(std::int64_t k) const noexcept { return firstTick + static_cast<std::int64_t>(k * periodTick); }

        void EncodeFrame() noexcept
        {
            const std::int64_t k     = done;
            const std::int64_t frame = (inputPos + k * frameSize) % (g_inputFrames - frameSize);
            const float*       pcm   = g_pcm.Data() + frame * 2;
            const std::int64_t t0    = Now();
            enc.EncodeNative(pcm, frameSize, packet, static_cast<std::int32_t>(sizeof(packet)), 24, pcm, frameSize, 0, 1, 2);
            const std::int64_t t1                  = Now();
            encodeUs[static_cast<std::size_t>(k)]  = static_cast<float>((t1 - t0) / g_ticksPerMs * 1000);
            latencyMs[static_cast<std::size_t>(k)] = static_cast<float>((t1 - arrival[static_cast<std::size_t>(k)]) / g_ticksPerMs);
            lateMs[static_cast<std::size_t>(k)]    = static_cast<float>((arrival[static_cast<std::size_t>(k)] - Scheduled(k)) / g_ticksPerMs);
            ++done;
        }
    };

    bool InitStream(Stream& s, const Options& o, std::int32_t index) noexcept
    {
        if (s.enc.Init(48000, 2, OPUS_APPLICATION_AUDIO) != OPUS_OK)
            return false;
        s.enc.Ctl(OPUS_SET_BITRATE_REQUEST, o.bitrate);
        s.enc.Ctl(OPUS_SET_COMPLEXITY_REQUEST, o.comp);
        s.frameSize  = static_cast<std::int32_t>(o.frameMs * 48 + .5);
        s.frames     = static_cast<std::int32_t>(o.seconds * 1000 / o.frameMs);
        s.inputPos   = (static_cast<std::int64_t>(index) * 1234567) % (g_inputFrames / 2);
        const auto n = static_cast<std::size_t>(s.frames);
        return s.arrival.Allocate(n) && s.latencyMs.Allocate(n) && s.lateMs.Allocate(n) && s.encodeUs.Allocate(n);
    }

    // The dispatcher: every frame of every stream at its scheduled time (to the timer's resolution).
    void Dispatch(Stream* streams, std::int32_t n, bool tasks) noexcept
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        const HANDLE            timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        HeapArray<std::int64_t> issued;
        issued.Allocate(static_cast<std::size_t>(n));
        for (;;) {
            std::int64_t       next = INT64_MAX;
            const std::int64_t now  = Now();
            bool               left = false;
            for (std::int32_t s = 0; s < n; ++s) {
                Stream& st  = streams[s];
                bool    set = false;
                while (issued[s] < st.frames && st.Scheduled(issued[s]) <= now) {
                    st.arrival[static_cast<std::size_t>(issued[s])] = now;
                    ++issued[s];
                    set = true;
                }
                if (set) {
                    st.due.store(issued[s]);
                    if (tasks)
                        st.taskEvent.Set();
                    else
                        SetEvent(st.threadEvent);
                }
                if (issued[s] < st.frames) {
                    left                 = true;
                    const std::int64_t t = st.Scheduled(issued[s]);
                    next                 = t < next ? t : next;
                }
            }
            if (!left)
                break;
            const std::int64_t wait = next - Now();
            if (wait > 0) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>(wait / g_ticksPerMs * 10000); // 100 ns, relative
                if (due.QuadPart < 0 && timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(timer, INFINITE);
            }
        }
        if (timer != nullptr)
            CloseHandle(timer);
    }

    DWORD WINAPI StreamThread(void* context) // DWORD/WINAPI: the thread procedure's signature
    {
        Stream& s = *static_cast<Stream*>(context);
        while (s.done < s.frames) {
            if (g_spinMs >= 0) {
                // block until shortly before the frame is due, then spin (the core stays awake)
                const std::int64_t spinFrom = s.Scheduled(s.done) - static_cast<std::int64_t>(g_spinMs * g_ticksPerMs);
                while (s.done >= s.due.load(std::memory_order_acquire) && Now() < spinFrom)
                    WaitForSingleObject(s.threadEvent, 1);
                while (s.done >= s.due.load(std::memory_order_acquire))
                    _mm_pause();
            }
            while (s.done >= s.due.load(std::memory_order_acquire))
                WaitForSingleObject(s.threadEvent, INFINITE);
            s.EncodeFrame();
        }
        return 0;
    }

    class StreamTask : public Tasks::Task
    {
    public:
        explicit StreamTask(Stream& s) noexcept : s_(s) {}

        void Run() noexcept override
        {
            while (s_.done < s_.frames) {
                while (s_.done >= s_.due.load(std::memory_order_acquire))
                    s_.taskEvent.Wait();
                s_.EncodeFrame();
            }
        }

    private:
        Stream& s_;
    };

    struct DispatchContext {
        Stream*      streams;
        std::int32_t n;
        bool         tasks;
    };

    DWORD WINAPI DispatchThread(void* context)
    {
        const DispatchContext& d = *static_cast<DispatchContext*>(context);
        if (d.tasks)
            ConvertThreadToTask(); // to set the tasks' events
        Dispatch(d.streams, d.n, d.tasks);
        if (d.tasks)
            ConvertTaskToThread();
        return 0;
    }

    int CompareFloat(const void* a, const void* b)
    {
        const float x = *static_cast<const float*>(a);
        const float y = *static_cast<const float*>(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }

    void Report(const Options& o, Stream* streams, std::int32_t n, double wallS, double cpuS) noexcept
    {
        std::size_t total = 0;
        for (std::int32_t s = 0; s < n; ++s)
            total += static_cast<std::size_t>(streams[s].frames);
        HeapArray<float> lat;
        HeapArray<float> late;
        HeapArray<float> enc;
        if (!lat.Allocate(total) || !late.Allocate(total) || !enc.Allocate(total))
            return;
        std::size_t k      = 0;
        std::size_t misses = 0;
        for (std::int32_t s = 0; s < n; ++s) {
            for (std::int32_t i = 0; i < streams[s].frames; ++i, ++k) {
                lat[k]  = streams[s].latencyMs[static_cast<std::size_t>(i)];
                late[k] = streams[s].lateMs[static_cast<std::size_t>(i)];
                enc[k]  = streams[s].encodeUs[static_cast<std::size_t>(i)];
                misses += lat[k] + late[k] > o.frameMs ? 1 : 0;
            }
        }
        std::qsort(lat.Data(), total, sizeof(float), CompareFloat);
        std::qsort(enc.Data(), total, sizeof(float), CompareFloat);
        std::qsort(late.Data(), total, sizeof(float), CompareFloat);
        auto q = [&](const HeapArray<float>& a, double p) { return a[static_cast<std::size_t>(p * static_cast<double>(total - 1))]; };
        if (o.csv) {
            std::wprintf(L"%ls,%d,%.1f,%d,%u,%d,%.3f,%.3f,%.3f,%.3f,%zu,%.1f,%.1f,%.1f,%.2f,%.3f,%.3f\n", o.mode, n, o.frameMs, o.comp, o.threads,
                         o.helper ? 1 : 0, q(lat, .5), q(lat, .99), q(lat, .999), lat[total - 1], misses, q(enc, .5), q(enc, .99), enc[total - 1], cpuS / wallS,
                         q(late, .5), q(late, .99));
        } else {
            std::wprintf(L"%ls: %d stream(s), %.1f ms frames, comp %d, %zu frames%ls\n", o.mode, n, o.frameMs, o.comp, total,
                         o.helper ? L", theta helper" : L"");
            if (std::wcscmp(o.mode, L"single") != 0) {
                std::wprintf(L"  timer lateness ms: p50 %.3f  p99 %.3f\n", q(late, .5), q(late, .99));
                std::wprintf(L"  latency ms (handed over -> encoded): p50 %.3f  p99 %.3f  p99.9 %.3f  max %.3f; deadline misses (> %.1f ms) %zu (%.3f%%)\n",
                             q(lat, .5), q(lat, .99), q(lat, .999), lat[total - 1], o.frameMs, misses,
                             100.0 * static_cast<double>(misses) / static_cast<double>(total));
            }
            std::wprintf(L"  encode us per frame: p50 %.1f  p99 %.1f  max %.1f; CPU %.2f cores over %.2f s\n", q(enc, .5), q(enc, .99), enc[total - 1],
                         cpuS / wallS, wallS);
        }
    }

    // --mode resample: the live resampler against the opusenc resampler (fixed ratio) and against
    // an ideal sine (changing ratio); errors in dB relative to the sine's amplitude.
    double ErrorDb(double sumSq, std::int64_t n) noexcept
    {
        return 10 * std::log10(sumSq / static_cast<double>(n > 0 ? n : 1) + 1e-30);
    }

    bool ResamplerTest() noexcept
    {
        constexpr double        Pi         = 3.14159265358979323846;
        constexpr double        Freq       = 997;
        constexpr double        Amp        = 0.5;
        bool                    ok         = true;
        const std::uint32_t     rates[][2] = {{44100, 48000}, {48000, 44100}, {48000, 48000}, {96000, 48000}, {16000, 48000}};
        HeapArray<float>        in, a, b;
        constexpr std::uint32_t Chunk = 480; // input frames per call, as WASAPI packets
        for (const auto& r : rates) {
            const std::uint32_t inRate = r[0], outRate = r[1];
            const std::uint32_t frames = inRate * 2;
            if (!in.Allocate(frames) || !a.Allocate(outRate * 3) || !b.Allocate(outRate * 3))
                return false;
            for (std::uint32_t i = 0; i < frames; ++i)
                in[i] = static_cast<float>(Amp * std::sin(2 * Pi * Freq * i / inRate));
            // fixed ratio: both resamplers, fed in chunks
            Resampler     ref;
            LiveResampler live;
            if (!ref.Initialize(1, inRate, outRate) || !live.Initialize(1, inRate, outRate))
                return false;
            std::uint32_t na = 0, nb = 0;
            for (std::uint32_t pos = 0; pos < frames; pos += Chunk) {
                std::uint32_t il = frames - pos < Chunk ? frames - pos : Chunk, ol = outRate * 3 - na;
                ref.ProcessInterleaved(in.Data() + pos, il, a.Data() + na, ol);
                na += ol;
                il = frames - pos < Chunk ? frames - pos : Chunk;
                ol = outRate * 3 - nb;
                live.ProcessInterleaved(in.Data() + pos, il, b.Data() + nb, ol);
                nb += ol;
            }
            double             diff = 0, ideal = 0;
            const std::int64_t n    = na < nb ? na : nb;
            const std::int64_t skip = outRate / 100; // the first 10 ms: the filter fills
            for (std::int64_t k = skip; k < n - skip; ++k) {
                const double d = a[static_cast<std::size_t>(k)] - b[static_cast<std::size_t>(k)];
                const double e = b[static_cast<std::size_t>(k)] - Amp * std::sin(2 * Pi * Freq * static_cast<double>(k) / outRate);
                diff += d * d;
                ideal += e * e;
            }
            const double dDb = ErrorDb(diff / (Amp * Amp), n - 2 * skip), iDb = ErrorDb(ideal / (Amp * Amp), n - 2 * skip);
            const bool   pass = na == nb && dDb < -60 && iDb < -60;
            ok &= pass;
            std::wprintf(L"%6u -> %6u: frames %u / %u, live vs opusenc %.1f dB, live vs sine %.1f dB %ls\n", inRate, outRate, na, nb, dDb, iDb,
                         pass ? L"ok" : L"FAIL");

            // changing ratio: +-0.2 % every 50 ms, the ideal phase integrated over the actual ratios
            LiveResampler drift;
            if (!drift.Initialize(1, inRate, outRate))
                return false;
            double        t = 0, err = 0;
            std::int64_t  count = 0, produced = 0;
            std::uint32_t pos = 0;
            float         out[2048];
            for (std::int32_t step = 0; pos < frames - Chunk; ++step) {
                if (step % 10 == 0)
                    drift.SetRatio(drift.NominalRatio() * ((step / 10) % 2 == 0 ? 1.002 : 0.998));
                std::uint32_t il = Chunk, ol = 2048;
                drift.ProcessInterleaved(in.Data() + pos, il, out, ol);
                pos += il;
                for (std::uint32_t k = 0; k < ol; ++k, ++produced) {
                    if (produced > outRate / 100) {
                        const double e = out[k] - Amp * std::sin(2 * Pi * Freq * t / inRate);
                        err += e * e;
                        ++count;
                    }
                    t += drift.Ratio();
                }
            }
            const double eDb   = ErrorDb(err / (Amp * Amp), count);
            const bool   dPass = eDb < -60;
            ok &= dPass;
            std::wprintf(L"%6u -> %6u: ratio +-0.2%% every 50 ms, %lld frames, live vs sine %.1f dB %ls\n", inRate, outRate, static_cast<long long>(produced),
                         eDb, dPass ? L"ok" : L"FAIL");
        }
        return ok;
    }

    double ProcessCpuSeconds() noexcept
    {
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        auto s = [](const FILETIME& f) { return (static_cast<double>(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) / 1e7; };
        return s(k) + s(u);
    }

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        auto           v = [&]() { return i + 1 < argc ? argv[++i] : L""; };
        if (std::wcscmp(a, L"--mode") == 0)
            o.mode = v();
        else if (std::wcscmp(a, L"--streams") == 0)
            o.streams = _wtoi(v());
        else if (std::wcscmp(a, L"--frame") == 0)
            o.frameMs = _wtof(v());
        else if (std::wcscmp(a, L"--comp") == 0)
            o.comp = _wtoi(v());
        else if (std::wcscmp(a, L"--bitrate") == 0)
            o.bitrate = _wtoi(v()) * 1000;
        else if (std::wcscmp(a, L"--seconds") == 0)
            o.seconds = _wtof(v());
        else if (std::wcscmp(a, L"--threads") == 0)
            o.threads = static_cast<std::uint32_t>(_wtoi(v()));
        else if (std::wcscmp(a, L"--helper") == 0)
            o.helper = true;
        else if (std::wcscmp(a, L"--gap") == 0)
            o.gapMs = _wtof(v());
        else if (std::wcscmp(a, L"--spin") == 0)
            o.spinMs = _wtof(v());
        else if (std::wcscmp(a, L"--csv") == 0)
            o.csv = true;
        else
            o.wav = a;
    }
    if (o.mode != nullptr && std::wcscmp(o.mode, L"resample") == 0)
        return ResamplerTest() ? 0 : 1;
    if (o.wav == nullptr) {
        std::fwprintf(stderr,
                      L"usage: ctOpusLiveBench <wav> [--mode single|threads|tasks] [--streams n] [--frame ms] [--comp c] [--bitrate kbps] [--seconds s] "
                      L"[--threads n] [--helper] [--csv]\n");
        return 2;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_ticksPerMs = static_cast<double>(f.QuadPart) / 1000;
    if (!LoadWav(o.wav)) {
        std::fwprintf(stderr, L"cannot read %ls (16-bit stereo WAV)\n", o.wav);
        return 1;
    }
    g_spinMs                   = o.spinMs;
    const bool         single  = std::wcscmp(o.mode, L"single") == 0;
    const bool         tasks   = std::wcscmp(o.mode, L"tasks") == 0;
    const std::int32_t n       = single ? 1 : o.streams;
    Stream*            streams = new (std::nothrow) Stream[static_cast<std::size_t>(n)];
    if (streams == nullptr)
        return 1;
    for (std::int32_t s = 0; s < n; ++s) {
        if (!InitStream(streams[s], o, s))
            return 1;
    }

    const double       cpu0  = ProcessCpuSeconds();
    const std::int64_t wall0 = Now();
    if (single) {
        // back to back, on this thread (a task for the helper's events)
        ConvertThreadToTask();
        {
            SchedulerHandle            scheduler = SchedulerHandle::Create(SchedulerKind::Default, 1);
            ThetaHelper                helper;
            HelperTask                 helperTask(helper);
            Synchronization::TaskGroup group;
            if (o.helper && scheduler) {
                scheduler->Start();
                group.AddTask(&helperTask);
                scheduler->AddTask(&helperTask, TaskPriority::High);
                streams[0].enc.SetThetaTrialRunner(&helper);
            }
            Stream& s = streams[0];
            for (std::int32_t k = 0; k < s.frames; ++k) {
                s.arrival[static_cast<std::size_t>(k)] = Now();
                s.firstTick                            = s.arrival[static_cast<std::size_t>(k)];
                s.EncodeFrame();
                if (o.gapMs > 0) {
                    // a pause between the frames: blocked (Sleep) or busy (--spin)
                    const std::int64_t until = Now() + static_cast<std::int64_t>(o.gapMs * g_ticksPerMs);
                    if (o.spinMs >= 0) {
                        while (Now() < until)
                            _mm_pause();
                    } else {
                        Sleep(static_cast<DWORD>(o.gapMs));
                    }
                }
            }
            if (o.helper && scheduler) {
                streams[0].enc.SetThetaTrialRunner(nullptr);
                helper.Stop();
                group.Wait();
                scheduler->Stop();
            }
        }
        ConvertTaskToThread();
    } else {
        const std::int64_t start = Now() + static_cast<std::int64_t>(200 * g_ticksPerMs);
        for (std::int32_t s = 0; s < n; ++s) {
            streams[s].periodTick = o.frameMs * g_ticksPerMs;
            streams[s].firstTick  = start + static_cast<std::int64_t>(streams[s].periodTick * s / n);
        }
        DispatchContext d = {streams, n, tasks};
        if (tasks) {
            ConvertThreadToTask();
            {
                SchedulerHandle scheduler = SchedulerHandle::Create(SchedulerKind::Default, o.threads);
                if (!scheduler)
                    return 1;
                scheduler->Start();
                HeapArray<StreamTask*>     taskObjects;
                Synchronization::TaskGroup group;
                taskObjects.Allocate(static_cast<std::size_t>(n));
                for (std::int32_t s = 0; s < n; ++s) {
                    taskObjects[s] = new (std::nothrow) StreamTask(streams[s]);
                    group.AddTask(taskObjects[s]);
                }
                for (std::int32_t s = 0; s < n; ++s)
                    scheduler->AddTask(taskObjects[s], TaskPriority::High);
                const HANDLE dispatcher = CreateThread(nullptr, 0, DispatchThread, &d, 0, nullptr);
                group.Wait();
                WaitForSingleObject(dispatcher, INFINITE);
                CloseHandle(dispatcher);
                scheduler->Stop();
                for (std::int32_t s = 0; s < n; ++s)
                    delete taskObjects[s];
            }
            ConvertTaskToThread();
        } else {
            HeapArray<HANDLE> threads;
            threads.Allocate(static_cast<std::size_t>(n));
            for (std::int32_t s = 0; s < n; ++s) {
                streams[s].threadEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                threads[s]             = CreateThread(nullptr, 1024 * 1024, StreamThread, &streams[s], STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            }
            const HANDLE dispatcher = CreateThread(nullptr, 0, DispatchThread, &d, 0, nullptr);
            WaitForSingleObject(dispatcher, INFINITE);
            for (std::int32_t s = 0; s < n; ++s) {
                WaitForSingleObject(threads[s], INFINITE);
                CloseHandle(threads[s]);
                CloseHandle(streams[s].threadEvent);
            }
            CloseHandle(dispatcher);
        }
    }
    const double wallS = (Now() - wall0) / g_ticksPerMs / 1000;
    Report(o, streams, n, wallS, ProcessCpuSeconds() - cpu0);
    delete[] streams;
    return 0;
}
