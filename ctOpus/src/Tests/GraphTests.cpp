// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "OggOpusEncoder.h"
#include "OpusDefines.h"
#include "OpusHeaders.h"
#include "WavInput.h"

// headers with imports last
#include "EncodeGraph.h"
#include "TestTasks.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Opus;

// The task graph itself (EncodeGraph on real schedulers) against the sequential graph: the same Ogg
// Opus bytes for every thread count. The thread counts switch the graph's helper nodes on (Theta
// from 3, one speculation level from 7, the cascade from 8, the levels' own Theta from 12); 48 kHz
// takes the copy instead of the resampler; six channels give several streams; a short input ends
// inside the first block.
namespace OpusTests {

    namespace {

        // A WAV file of 16-bit PCM: tones that glide, bursts and noise, the same on every run.
        std::wstring WriteTestWav(const wchar_t* name, std::int32_t rate, std::int32_t channels, std::int64_t frames)
        {
            std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames * channels));
            std::uint32_t             seed = 12345;
            for (std::int64_t i = 0; i < frames; ++i) {
                const double t     = static_cast<double>(i) / rate;
                const double burst = (i / (rate / 4)) % 3 == 0 ? 1.0 : 0.2;
                for (std::int32_t c = 0; c < channels; ++c) {
                    seed            = seed * 1664525u + 1013904223u;
                    const double f  = 220.0 * (c + 1) + 400.0 * t;
                    const double v  = 0.4 * std::sin(6.283185307179586 * f * t) * burst + 0.05 * (static_cast<std::int32_t>(seed >> 16) - 32768) / 32768.0;
                    pcm[i * channels + c] = static_cast<std::int16_t>(v * 32767);
                }
            }

            const std::uint32_t dataBytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
            std::vector<std::uint8_t> file(44 + dataBytes);
            auto put16 = [&](std::size_t at, std::uint32_t v) { file[at] = static_cast<std::uint8_t>(v); file[at + 1] = static_cast<std::uint8_t>(v >> 8); };
            auto put32 = [&](std::size_t at, std::uint32_t v) { put16(at, v & 0xffff); put16(at + 2, v >> 16); };
            std::memcpy(&file[0], "RIFF", 4);
            put32(4, 36 + dataBytes);
            std::memcpy(&file[8], "WAVEfmt ", 8);
            put32(16, 16);
            put16(20, 1); // PCM
            put16(22, static_cast<std::uint32_t>(channels));
            put32(24, static_cast<std::uint32_t>(rate));
            put32(28, static_cast<std::uint32_t>(rate * channels * 2));
            put16(32, static_cast<std::uint32_t>(channels * 2));
            put16(34, 16);
            std::memcpy(&file[36], "data", 4);
            put32(40, dataBytes);
            std::memcpy(&file[44], pcm.data(), dataBytes);

            wchar_t dir[MAX_PATH];
            GetTempPathW(MAX_PATH, dir);
            const std::wstring path = std::wstring(dir) + L"ctOpusGraphTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + name + L".wav";
            HANDLE             h    = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            Assert::IsTrue(h != INVALID_HANDLE_VALUE);
            unsigned long written = 0; // DWORD of WriteFile
            Assert::IsTrue(WriteFile(h, file.data(), static_cast<unsigned long>(file.size()), &written, nullptr) != 0);
            CloseHandle(h);
            return path;
        }

        bool CollectPage(void* user, const std::uint8_t* data, std::int32_t length)
        {
            auto& out = *static_cast<std::vector<std::uint8_t>*>(user);
            out.insert(out.end(), data, data + length);
            return true;
        }

        bool CloseOutput(void*)
        {
            return true;
        }

        constexpr std::uint32_t Sequential = 0;

        // Encodes the file with the parallel graph on threads compute threads, or with the
        // sequential graph; the Ogg Opus bytes.
        std::vector<std::uint8_t> Encode(const std::wstring& path, std::uint32_t threads, std::int32_t complexity)
        {
            InputFile file;
            Assert::IsTrue(file.Open(path.c_str()));
            WavInput wav;
            Assert::IsTrue(wav.Open(file, false, nullptr));
            const WavInput::Info info = wav.GetInfo();

            std::vector<std::uint8_t> out;
            OpusTags                  tags;
            Assert::IsTrue(tags.Initialize("ctOpus graph tests"));
            OggOpusEncoder::Callbacks callbacks;
            callbacks.user  = &out;
            callbacks.write = CollectPage;
            callbacks.close = CloseOutput;
            OggOpusEncoder encoder;
            Assert::AreEqual(OpeOk, encoder.Create(callbacks, tags, info.rate, info.channels, info.channels > 2 ? 1 : 0));
            Assert::AreEqual(OpeOk, encoder.Ctl(OpeSetSerialNoRequest, 1));
            Assert::AreEqual(OpeOk, encoder.Ctl(OPUS_SET_BITRATE_REQUEST, 64000 * info.channels));
            Assert::AreEqual(OpeOk, encoder.Ctl(OPUS_SET_COMPLEXITY_REQUEST, complexity));
            std::int32_t lookahead = 0;
            Assert::AreEqual(OpeOk, encoder.Get(OPUS_GET_LOOKAHEAD_REQUEST, lookahead));

            std::int32_t ret = OpeAllocFail;
            if (threads == Sequential) {
                constexpr std::int32_t FrameSize = 960; // 20 ms
                std::vector<float>     buffer(static_cast<std::size_t>(FrameSize * info.channels));
                const EncodeProgress   noProgress;
                Shared::TaskRuntime      runtime(0);
                Shared::RunningScheduler compute(Ct::SchedulerKind::Default, 1);
                Assert::IsTrue(runtime && compute);
                SequentialGraph graph(wav, encoder, buffer.data(), FrameSize, noProgress);
                ret = graph.Run(*compute);
            } else {
                EncodeGraph::Settings settings;
                settings.threads    = threads;
                settings.lookahead  = lookahead;
                settings.complexity = complexity;
                Shared::TaskRuntime      runtime(1); // first: the graph's events end before the thread converts back
                Shared::RunningScheduler compute(Ct::SchedulerKind::Default, threads);
                Assert::IsTrue(runtime && compute);
                EncodeGraph graph(wav, encoder, settings);
                Assert::IsTrue(graph.Initialize());
                ret = graph.Run(*runtime.Io(), *compute);
            }
            Assert::AreEqual(OpeOk, ret);
            Assert::IsTrue(out.size() > 100);
            return out;
        }

        void ExpectSameAsSequential(const wchar_t* name, std::int32_t rate, std::int32_t channels, std::int64_t frames, std::initializer_list<std::uint32_t> threadCounts,
                                    std::int32_t complexity = 10)
        {
            const std::wstring              path      = WriteTestWav(name, rate, channels, frames);
            const std::vector<std::uint8_t> reference = Encode(path, Sequential, complexity);
            for (const std::uint32_t threads : threadCounts) {
                const std::vector<std::uint8_t> parallel = Encode(path, threads, complexity);
                const std::wstring message = std::wstring(name) + L", " + std::to_wstring(threads) + L" threads";
                Assert::AreEqual(reference.size(), parallel.size(), message.c_str());
                Assert::IsTrue(reference == parallel, message.c_str());
            }
            DeleteFileW(path.c_str());
        }

    } // namespace

    TEST_CLASS(GraphTests)
    {
    public:
        TEST_METHOD(Stereo44100EveryHelperLevel) { ExpectSameAsSequential(L"stereo44", 44100, 2, 44100 * 6, {1, 2, 3, 7, 8, 12}); }

        TEST_METHOD(Stereo48000Copy) { ExpectSameAsSequential(L"stereo48", 48000, 2, 48000 * 5, {2, 8}); }

        TEST_METHOD(Mono16000) { ExpectSameAsSequential(L"mono16", 16000, 1, 16000 * 5, {2, 8}); }

        TEST_METHOD(SurroundSeveralStreams) { ExpectSameAsSequential(L"surround", 44100, 6, 44100 * 3, {3, 8}); }

        TEST_METHOD(ShortInput) { ExpectSameAsSequential(L"short", 44100, 2, 2205, {2, 8}); }

        // below complexity 8 CELT tries no second theta: the graph has no Theta node
        TEST_METHOD(LowComplexity) { ExpectSameAsSequential(L"comp5", 44100, 2, 44100 * 3, {3, 8}, 5); }
    };

} // namespace OpusTests
