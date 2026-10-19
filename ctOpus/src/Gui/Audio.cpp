// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <Windows.h>

#include <Audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Audio.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui {

    namespace {

        // PKEY_Device_FriendlyName and PKEY_AudioEngine_DeviceFormat, defined here to need no INITGUID
        constexpr PROPERTYKEY DeviceFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
        constexpr PROPERTYKEY DeviceFormat       = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

        constexpr SampleKind ExclusiveKinds[] = {SampleKind::Float32, SampleKind::Int32, SampleKind::Int24In32, SampleKind::Int24Packed, SampleKind::Int16};

        void Fail(wchar_t* error, std::size_t capacity, const wchar_t* what, HRESULT hr) noexcept
        {
            if (error != nullptr && capacity > 0)
                swprintf_s(error, capacity, L"%ls (0x%08X)", what, static_cast<unsigned int>(hr));
        }

        std::int32_t BytesOf(SampleKind kind) noexcept
        {
            switch (kind) {
                case SampleKind::Int16: return 2;
                case SampleKind::Int24Packed: return 3;
                default: return 4;
            }
        }

        std::int32_t ValidBitsOf(SampleKind kind) noexcept
        {
            switch (kind) {
                case SampleKind::Int16: return 16;
                case SampleKind::Int24Packed:
                case SampleKind::Int24In32: return 24;
                default: return 32;
            }
        }

        /// <summary>The sample kind of a format, false if it is none of the supported ones.</summary>
        bool KindOf(const WAVEFORMATEX* format, SampleKind& kind) noexcept
        {
            bool         isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            bool         isPcm   = format->wFormatTag == WAVE_FORMAT_PCM;
            std::int32_t valid   = format->wBitsPerSample;
            if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
                const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
                isFloat         = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                isPcm           = ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
                if (ext->Samples.wValidBitsPerSample != 0)
                    valid = ext->Samples.wValidBitsPerSample;
            }
            const std::int32_t container = format->wBitsPerSample;
            if (isFloat && container == 32)
                kind = SampleKind::Float32;
            else if (isPcm && container == 16)
                kind = SampleKind::Int16;
            else if (isPcm && container == 24)
                kind = SampleKind::Int24Packed;
            else if (isPcm && container == 32 && valid == 24)
                kind = SampleKind::Int24In32;
            else if (isPcm && container == 32)
                kind = SampleKind::Int32;
            else
                return false;
            return true;
        }

        std::uint32_t DefaultMask(std::int32_t channels) noexcept
        {
            switch (channels) {
                case 1: return SPEAKER_FRONT_CENTER;
                case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
                case 4: return KSAUDIO_SPEAKER_QUAD;
                case 6: return KSAUDIO_SPEAKER_5POINT1;
                case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
                default: return 0;
            }
        }

        void MakeFormat(SampleKind kind, std::int32_t rate, std::int32_t channels, std::uint32_t mask, WAVEFORMATEXTENSIBLE& f) noexcept
        {
            f                             = {};
            f.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
            f.Format.nChannels            = static_cast<unsigned short>(channels);
            f.Format.nSamplesPerSec       = static_cast<unsigned long>(rate);
            f.Format.wBitsPerSample       = static_cast<unsigned short>(8 * BytesOf(kind));
            f.Format.nBlockAlign          = static_cast<unsigned short>(channels * BytesOf(kind));
            f.Format.nAvgBytesPerSec      = static_cast<unsigned long>(rate) * f.Format.nBlockAlign;
            f.Format.cbSize               = 22;
            f.Samples.wValidBitsPerSample = static_cast<unsigned short>(ValidBitsOf(kind));
            f.dwChannelMask               = mask;
            f.SubFormat                   = kind == SampleKind::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
        }

        float ReadSample(const unsigned char* p, SampleKind kind) noexcept
        {
            switch (kind) {
                case SampleKind::Float32: {
                    float v;
                    std::memcpy(&v, p, 4);
                    return v;
                }
                case SampleKind::Int16: {
                    std::int16_t v;
                    std::memcpy(&v, p, 2);
                    return static_cast<float>(v) * (1.0f / 32768.0f);
                }
                case SampleKind::Int24Packed: {
                    const std::int32_t v = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) << 8 | static_cast<std::uint32_t>(p[1]) << 16
                                                                     | static_cast<std::uint32_t>(p[2]) << 24);
                    return static_cast<float>(v) * (1.0f / 2147483648.0f);
                }
                default: { // int32 and int24 in the upper bits of 32
                    std::int32_t v;
                    std::memcpy(&v, p, 4);
                    return static_cast<float>(v) * (1.0f / 2147483648.0f);
                }
            }
        }

        void WriteSample(unsigned char* p, SampleKind kind, float v) noexcept
        {
            if (kind == SampleKind::Float32) {
                std::memcpy(p, &v, 4);
                return;
            }
            const double clamped = v > 1.0f ? 1.0 : v < -1.0f ? -1.0 : static_cast<double>(v);
            switch (kind) {
                case SampleKind::Int16: {
                    const auto s = static_cast<std::int16_t>(clamped * 32767.0);
                    std::memcpy(p, &s, 2);
                    break;
                }
                case SampleKind::Int24Packed: {
                    const auto s = static_cast<std::int32_t>(clamped * 8388607.0);
                    p[0]         = static_cast<unsigned char>(s & 0xff);
                    p[1]         = static_cast<unsigned char>((s >> 8) & 0xff);
                    p[2]         = static_cast<unsigned char>((s >> 16) & 0xff);
                    break;
                }
                case SampleKind::Int24In32: {
                    const auto s = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(clamped * 8388607.0)) << 8);
                    std::memcpy(p, &s, 4);
                    break;
                }
                default: {
                    const auto s = static_cast<std::int32_t>(clamped * 2147483647.0);
                    std::memcpy(p, &s, 4);
                    break;
                }
            }
        }

        bool OpenDevice(bool capture, const wchar_t* id, ComPtr<IMMDevice>& device, HRESULT& hr) noexcept
        {
            ComPtr<IMMDeviceEnumerator> enumerator;
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), enumerator.PutVoid());
            if (FAILED(hr))
                return false;
            if (id == nullptr || id[0] == 0)
                hr = enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, device.Put());
            else
                hr = enumerator->GetDevice(id, device.Put());
            return SUCCEEDED(hr);
        }

        /// <summary>The device format of the endpoint (PKEY_AudioEngine_DeviceFormat): rate, channels, mask; false if unknown.</summary>
        bool DeviceFormatOf(IMMDevice* device, std::int32_t& rate, std::int32_t& channels, std::uint32_t& mask) noexcept
        {
            ComPtr<IPropertyStore> properties;
            if (FAILED(device->OpenPropertyStore(STGM_READ, properties.Put())))
                return false;
            PROPVARIANT value;
            PropVariantInit(&value);
            bool ok = false;
            if (SUCCEEDED(properties->GetValue(DeviceFormat, &value)) && value.vt == VT_BLOB && value.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                const auto* format = reinterpret_cast<const WAVEFORMATEX*>(value.blob.pBlobData);
                rate               = static_cast<std::int32_t>(format->nSamplesPerSec);
                channels           = format->nChannels;
                mask               = DefaultMask(channels);
                if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && value.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE))
                    mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->dwChannelMask;
                ok = rate > 0 && channels > 0;
            }
            PropVariantClear(&value);
            return ok;
        }

        class CoTaskMemFormat
        {
        public:
            CoTaskMemFormat() noexcept                         = default;
            CoTaskMemFormat(const CoTaskMemFormat&)            = delete;
            CoTaskMemFormat& operator=(const CoTaskMemFormat&) = delete;
            ~CoTaskMemFormat() { CoTaskMemFree(format_); }
            WAVEFORMATEX** Put() noexcept { return &format_; }
            WAVEFORMATEX*  Get() const noexcept { return format_; }

        private:
            WAVEFORMATEX* format_ = nullptr;
        };

        const wchar_t* ExclusiveFailure(HRESULT hr) noexcept
        {
            switch (hr) {
                case AUDCLNT_E_DEVICE_IN_USE: return L"exclusive refused: device in use";
                case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return L"exclusive refused: not allowed for this device";
                case AUDCLNT_E_UNSUPPORTED_FORMAT: return L"exclusive refused: no supported format";
                default: return L"exclusive failed";
            }
        }

    } // namespace

    const wchar_t* SampleKindName(SampleKind kind) noexcept
    {
        switch (kind) {
            case SampleKind::Float32: return L"float32";
            case SampleKind::Int32: return L"int32";
            case SampleKind::Int24In32: return L"int24 in 32";
            case SampleKind::Int24Packed: return L"int24";
            default: return L"int16";
        }
    }

    void DescribeSetup(const StreamSetup& setup, wchar_t* text, std::size_t capacity) noexcept
    {
        const wchar_t* mode = setup.exclusive ? L"exclusive"
            : setup.lowLatency                ? L"shared (IAudioClient3)"
            : setup.converted                 ? L"shared (converted)"
                                              : L"shared";
        swprintf_s(text, capacity, L"%ls, %ls, %d Hz, %d ch, period %.2f ms%ls%ls", mode, SampleKindName(setup.kind), setup.deviceRate, setup.deviceChannels,
                   setup.PeriodMs(), setup.note[0] != 0 ? L" - " : L"", setup.note);
    }

    namespace {

        /// <summary>The periods from a client that is not initialised yet.</summary>
        void ReadPeriods(IAudioClient* client, const WAVEFORMATEX* mix, DevicePeriods& periods) noexcept
        {
            periods                      = {};
            periods.mixRate              = static_cast<std::int32_t>(mix->nSamplesPerSec);
            REFERENCE_TIME defaultPeriod = 0;
            REFERENCE_TIME minimumPeriod = 0;
            if (SUCCEEDED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) {
                periods.valid              = true;
                periods.exclusiveMinMs     = static_cast<double>(minimumPeriod) / 10'000.0;
                periods.exclusiveDefaultMs = static_cast<double>(defaultPeriod) / 10'000.0;
            }
            ComPtr<IAudioClient3> client3;
            if (periods.mixRate > 0 && SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), client3.PutVoid()))) {
                unsigned int defaultFrames = 0; // UINT32 out params of GetSharedModeEnginePeriod
                unsigned int fundamental   = 0;
                unsigned int minFrames     = 0;
                unsigned int maxFrames     = 0;
                if (SUCCEEDED(client3->GetSharedModeEnginePeriod(mix, &defaultFrames, &fundamental, &minFrames, &maxFrames))) {
                    periods.engineValid         = true;
                    periods.engineMinMs         = minFrames * 1000.0 / periods.mixRate;
                    periods.engineDefaultMs     = defaultFrames * 1000.0 / periods.mixRate;
                    periods.engineFundamentalMs = fundamental * 1000.0 / periods.mixRate;
                }
            }
        }

    } // namespace

    bool QueryDevicePeriods(bool capture, const wchar_t* id, DevicePeriods& periods) noexcept
    {
        periods              = {};
        HRESULT           hr = S_OK;
        ComPtr<IMMDevice> device;
        if (!OpenDevice(capture, id, device, hr))
            return false;
        ComPtr<IAudioClient> client;
        CoTaskMemFormat      mix;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.PutVoid())) || FAILED(client->GetMixFormat(mix.Put())))
            return false;
        ReadPeriods(client.Get(), mix.Get(), periods);
        return periods.valid;
    }

    void DescribePeriods(const wchar_t* label, const DevicePeriods& periods, const StreamSetup* running, wchar_t* text, std::size_t capacity) noexcept
    {
        wchar_t head[96];
        if (running != nullptr)
            swprintf_s(head, L"running %ls %.2f ms | ",
                       running->exclusive        ? L"exclusive"
                           : running->lowLatency ? L"shared (IAudioClient3)"
                                                 : L"shared",
                       running->PeriodMs());
        else
            head[0] = 0;
        wchar_t engine[128];
        if (periods.engineValid)
            swprintf_s(engine, L"shared engine min %.2f / default %.2f / fundamental %.2f ms", periods.engineMinMs, periods.engineDefaultMs,
                       periods.engineFundamentalMs);
        else
            swprintf_s(engine, L"shared engine: no IAudioClient3 periods");
        if (!periods.valid) {
            swprintf_s(text, capacity, L"%ls: periods unknown", label);
            return;
        }
        swprintf_s(text, capacity, L"%ls: %lsexclusive min %.2f / default %.2f | %ls", label, head, periods.exclusiveMinMs, periods.exclusiveDefaultMs, engine);
    }

    std::int32_t EnumerateAudioDevices(bool capture, AudioDevice* devices, std::int32_t capacity) noexcept
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), enumerator.PutVoid())))
            return 0;
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, collection.Put())))
            return 0;
        unsigned int count = 0; // UINT of GetCount
        collection->GetCount(&count);
        std::int32_t stored = 0;
        for (unsigned int i = 0; i < count && stored < capacity; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.Put())))
                continue;
            wchar_t* id = nullptr;
            if (FAILED(device->GetId(&id)))
                continue;
            AudioDevice& entry = devices[stored];
            CopyText(entry.id, sizeof(entry.id) / sizeof(entry.id[0]), id);
            CoTaskMemFree(id);
            CopyText(entry.name, sizeof(entry.name) / sizeof(entry.name[0]), L"(unnamed device)");
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.Put()))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(DeviceFriendlyName, &value)) && value.vt == VT_LPWSTR)
                    CopyText(entry.name, sizeof(entry.name) / sizeof(entry.name[0]), value.pwszVal);
                PropVariantClear(&value);
            }
            ++stored;
        }
        return stored;
    }

    bool WasapiStream::OpenExclusive(IMMDevice* device, const WAVEFORMATEX* mix, HRESULT& hr) noexcept
    {
        std::int32_t  deviceRate     = static_cast<std::int32_t>(mix->nSamplesPerSec);
        std::int32_t  deviceChannels = mix->nChannels;
        std::uint32_t mask           = DefaultMask(deviceChannels);
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
        DeviceFormatOf(device, deviceRate, deviceChannels, mask);

        // 48 kHz first, then the device rate, then the mix rate; each with the formats in order
        const std::int32_t   rates[] = {SampleRate, deviceRate, static_cast<std::int32_t>(mix->nSamplesPerSec)};
        WAVEFORMATEXTENSIBLE format  = {};
        bool                 found   = false;
        hr                           = AUDCLNT_E_UNSUPPORTED_FORMAT;
        for (std::int32_t r = 0; r < 3 && !found; ++r) {
            if (r > 0 && (rates[r] == rates[0] || (r == 2 && rates[2] == rates[1])))
                continue;
            for (const SampleKind kind : ExclusiveKinds) {
                MakeFormat(kind, rates[r], deviceChannels, mask, format);
                const HRESULT supported = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr);
                if (supported == S_OK) {
                    setup_.kind       = kind;
                    setup_.deviceRate = rates[r];
                    found             = true;
                    break;
                }
                if (supported == AUDCLNT_E_DEVICE_IN_USE || supported == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
                    hr = supported;
                    return false;
                }
            }
        }
        if (!found)
            return false;
        setup_.deviceChannels = deviceChannels;

        REFERENCE_TIME defaultPeriod = 0;
        REFERENCE_TIME minimumPeriod = 0;
        hr                           = client_->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
        if (FAILED(hr))
            return false;
        REFERENCE_TIME duration = minimumPeriod;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, duration, &format.Format, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            // the documented way: the aligned buffer size, a new client, Initialize again
            unsigned int frames = 0; // UINT32 of GetBufferSize
            hr                  = client_->GetBufferSize(&frames);
            if (FAILED(hr))
                return false;
            duration = static_cast<REFERENCE_TIME>(10'000'000.0 * frames / setup_.deviceRate + 0.5);
            client_.Reset();
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.PutVoid());
            if (SUCCEEDED(hr))
                hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, duration, &format.Format, nullptr);
        }
        if (FAILED(hr))
            return false;
        setup_.exclusive    = true;
        setup_.periodFrames = static_cast<std::int32_t>((duration * setup_.deviceRate + 5'000'000) / 10'000'000);
        return true;
    }

    bool WasapiStream::OpenShared(IMMDevice* device, const WAVEFORMATEX* mix, bool lowLatency, HRESULT& hr) noexcept
    {
        setup_.deviceRate     = static_cast<std::int32_t>(mix->nSamplesPerSec);
        setup_.deviceChannels = mix->nChannels;
        const bool native     = KindOf(mix, setup_.kind);

        if (native && lowLatency) {
            // IAudioClient3: the smallest period the engine supports for this format in shared mode
            ComPtr<IAudioClient3> client3;
            if (SUCCEEDED(client_->QueryInterface(__uuidof(IAudioClient3), client3.PutVoid()))) {
                unsigned int defaultPeriod = 0; // UINT32 out params of GetSharedModeEnginePeriod
                unsigned int fundamental   = 0;
                unsigned int minPeriod     = 0;
                unsigned int maxPeriod     = 0;
                if (SUCCEEDED(client3->GetSharedModeEnginePeriod(mix, &defaultPeriod, &fundamental, &minPeriod, &maxPeriod))
                    && SUCCEEDED(client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, mix, nullptr))) {
                    setup_.lowLatency   = true;
                    setup_.periodFrames = static_cast<std::int32_t>(minPeriod);
                    return true;
                }
            }
            // a failed InitializeSharedAudioStream leaves the client unusable
            client_.Reset();
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.PutVoid());
            if (FAILED(hr))
                return false;
        }
        WAVEFORMATEXTENSIBLE converted = {};
        unsigned long        flags     = AUDCLNT_STREAMFLAGS_EVENTCALLBACK; // DWORD of Initialize
        const WAVEFORMATEX*  format    = mix;
        if (!native) {
            // let the engine convert to float at the mix rate
            MakeFormat(SampleKind::Float32, setup_.deviceRate, setup_.deviceChannels, DefaultMask(setup_.deviceChannels), converted);
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            format           = &converted.Format;
            setup_.kind      = SampleKind::Float32;
            setup_.converted = true;
        }
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, format, nullptr);
        if (FAILED(hr))
            return false;
        REFERENCE_TIME defaultPeriod = 0;
        REFERENCE_TIME minimumPeriod = 0;
        client_->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
        setup_.periodFrames = static_cast<std::int32_t>(defaultPeriod * setup_.deviceRate / 10'000'000);
        return true;
    }

    bool WasapiStream::Open(bool capture, const wchar_t* id, bool exclusive, bool lowLatency, wchar_t* error, std::size_t errorCapacity) noexcept
    {
        Close();
        HRESULT           hr = S_OK;
        ComPtr<IMMDevice> device;
        if (!OpenDevice(capture, id, device, hr)) {
            Fail(error, errorCapacity, L"Audio device not found", hr);
            return false;
        }
        CoTaskMemFormat mix;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.PutVoid());
        if (SUCCEEDED(hr))
            hr = client_->GetMixFormat(mix.Put());
        if (FAILED(hr)) {
            Fail(error, errorCapacity, L"The audio client cannot be activated", hr);
            return false;
        }
        event_.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!event_) {
            Fail(error, errorCapacity, L"CreateEvent failed", HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }

        setup_                    = {};
        setup_.exclusiveRequested = exclusive;
        ReadPeriods(client_.Get(), mix.Get(), setup_.periods);
        if (exclusive && !OpenExclusive(device.Get(), mix.Get(), hr)) {
            // fall back to shared mode with a fresh client, and say why
            swprintf_s(setup_.note, L"%ls (0x%08X), fell back to shared", ExclusiveFailure(hr), static_cast<unsigned int>(hr));
            setup_.exclusive = false;
            client_.Reset();
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.PutVoid());
            if (FAILED(hr)) {
                Fail(error, errorCapacity, L"The audio client cannot be activated", hr);
                return false;
            }
        }
        if (!setup_.exclusive && !OpenShared(device.Get(), mix.Get(), lowLatency, hr)) {
            // another application holding the device exclusively also blocks shared mode
            Fail(error, errorCapacity,
                 hr == AUDCLNT_E_DEVICE_IN_USE ? L"The device is in use by another application in exclusive mode" : L"IAudioClient::Initialize failed", hr);
            return false;
        }

        unsigned int bufferFrames = 0; // UINT32 of GetBufferSize
        hr                        = client_->GetBufferSize(&bufferFrames);
        if (SUCCEEDED(hr))
            hr = client_->SetEventHandle(event_.Get());
        if (SUCCEEDED(hr))
            hr = capture ? client_->GetService(__uuidof(IAudioCaptureClient), capture_.PutVoid())
                         : client_->GetService(__uuidof(IAudioRenderClient), render_.PutVoid());
        if (FAILED(hr)) {
            Fail(error, errorCapacity, L"The audio stream cannot be set up", hr);
            return false;
        }
        setup_.bufferFrames    = static_cast<std::int32_t>(bufferFrames);
        REFERENCE_TIME latency = 0;
        if (SUCCEEDED(client_->GetStreamLatency(&latency)))
            setup_.streamLatencyMs = static_cast<double>(latency) / 10'000.0;
        if (!scratch_.Allocate(static_cast<std::size_t>(bufferFrames + 4096) * Channels)) {
            Fail(error, errorCapacity, L"Out of memory", E_OUTOFMEMORY);
            return false;
        }
        return true;
    }

    void WasapiStream::Close() noexcept
    {
        Stop();
        capture_.Reset();
        render_.Reset();
        client_.Reset();
        event_.Reset();
    }

    bool WasapiStream::Start() noexcept
    {
        if (!client_)
            return false;
        if (render_) {
            // start with a silent buffer, so the first event comes after a period of playback
            unsigned int padding = 0; // UINT32 of GetCurrentPadding
            if (!setup_.exclusive)
                client_->GetCurrentPadding(&padding);
            const auto     free = static_cast<unsigned int>(setup_.bufferFrames) - padding;
            unsigned char* data = nullptr; // BYTE* of GetBuffer
            if (free > 0 && SUCCEEDED(render_->GetBuffer(free, &data)))
                render_->ReleaseBuffer(free, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        started_ = SUCCEEDED(client_->Start());
        return started_;
    }

    void WasapiStream::Stop() noexcept
    {
        if (started_ && client_)
            client_->Stop();
        started_ = false;
    }

    bool WasapiStream::DrainCapture(CaptureSink& sink) noexcept
    {
        const std::int32_t channels = setup_.deviceChannels;
        const SampleKind   kind     = setup_.kind;
        const std::int32_t bytes    = BytesOf(kind);
        for (;;) {
            unsigned int packet = 0; // UINT32 of GetNextPacketSize
            if (FAILED(capture_->GetNextPacketSize(&packet)))
                return false;
            if (packet == 0)
                return true;
            unsigned char*     data      = nullptr; // BYTE* of GetBuffer
            unsigned int       frames    = 0;       // UINT32
            unsigned long      flags     = 0;       // DWORD
            unsigned long long position  = 0;       // UINT64 device position
            unsigned long long timestamp = 0;       // UINT64 QPC time in 100 ns
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, &position, &timestamp)))
                return false;
            const auto count = static_cast<std::int32_t>(frames);
            float*     out   = scratch_.Data();
            if (count > 0 && static_cast<std::size_t>(count) * Channels <= scratch_.Size()) {
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                    std::memset(out, 0, static_cast<std::size_t>(count) * Channels * sizeof(float));
                } else {
                    for (std::int32_t i = 0; i < count; ++i) {
                        const unsigned char* frame = data + static_cast<std::size_t>(i) * channels * bytes;
                        const float          l     = ReadSample(frame, kind);
                        out[2 * i]                 = l;
                        out[2 * i + 1]             = channels >= 2 ? ReadSample(frame + bytes, kind) : l;
                    }
                }
                const std::int64_t time = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 || timestamp == 0
                    ? Now100ns() - FramesTo100ns(count, setup_.deviceRate)
                    : static_cast<std::int64_t>(timestamp);
                sink.OnPacket(out, count, time, (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0);
            }
            capture_->ReleaseBuffer(frames);
        }
    }

    bool WasapiStream::FillRender(RenderSource& source) noexcept
    {
        std::int32_t free    = setup_.bufferFrames;
        std::int32_t padding = setup_.bufferFrames; // exclusive: the other half of the double buffer plays
        if (!setup_.exclusive) {
            unsigned int queued = 0; // UINT32 of GetCurrentPadding
            if (FAILED(client_->GetCurrentPadding(&queued)))
                return false;
            padding = static_cast<std::int32_t>(queued);
            free    = setup_.bufferFrames - padding;
        }
        if (free <= 0)
            return true;
        unsigned char* data = nullptr; // BYTE* of GetBuffer
        if (FAILED(render_->GetBuffer(static_cast<unsigned int>(free), &data)))
            return false;
        float* stereo = scratch_.Data();
        source.Pull(stereo, free, padding);
        const std::int32_t channels = setup_.deviceChannels;
        const SampleKind   kind     = setup_.kind;
        const std::int32_t bytes    = BytesOf(kind);
        for (std::int32_t i = 0; i < free; ++i) {
            unsigned char* frame = data + static_cast<std::size_t>(i) * channels * bytes;
            const float    l     = stereo[2 * i];
            const float    r     = stereo[2 * i + 1];
            if (channels == 1) {
                WriteSample(frame, kind, 0.5f * (l + r));
                continue;
            }
            WriteSample(frame, kind, l);
            WriteSample(frame + bytes, kind, r);
            for (std::int32_t c = 2; c < channels; ++c)
                WriteSample(frame + c * bytes, kind, 0.0f);
        }
        render_->ReleaseBuffer(static_cast<unsigned int>(free), 0);
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui
