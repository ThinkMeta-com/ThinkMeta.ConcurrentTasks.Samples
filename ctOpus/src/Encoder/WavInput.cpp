// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "WavInput.h"

#include <climits>
#include <cstdint>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    namespace {

        constexpr std::size_t InputBufferSize = 64 * 1024;

        // wav_permute_matrix (opus-tools opus_header.c): WAV channel order to Vorbis order.
        constexpr std::int32_t WavPermute[8][8]
            = {{0}, {0, 1}, {0, 2, 1}, {0, 1, 2, 3}, {0, 2, 1, 3, 4}, {0, 2, 1, 4, 5, 3}, {0, 2, 1, 5, 6, 4, 3}, {0, 2, 1, 6, 7, 4, 5, 3}};

        // setup_downmix: the stereo mix of 2..8 input channels.
        constexpr float StereoMix[7][8][2] = {
            {{1, 0}, {0, 1}},
            {{1, 0}, {0.7071f, 0.7071f}, {0, 1}},
            {{1, 0}, {0, 1}, {0.866f, 0.5f}, {0.5f, 0.866f}},
            {{1, 0}, {0.7071f, 0.7071f}, {0, 1}, {0.866f, 0.5f}, {0.5f, 0.866f}},
            {{1, 0}, {0.7071f, 0.7071f}, {0, 1}, {0.866f, 0.5f}, {0.5f, 0.866f}, {0.7071f, 0.7071f}},
            {{1, 0}, {0.7071f, 0.7071f}, {0, 1}, {0.866f, 0.5f}, {0.5f, 0.866f}, {0.6123f, 0.6123f}, {0.7071f, 0.7071f}},
            {{1, 0}, {0.7071f, 0.7071f}, {0, 1}, {0.866f, 0.5f}, {0.5f, 0.866f}, {0.866f, 0.5f}, {0.5f, 0.866f}, {0.7071f, 0.7071f}},
        };

        std::uint32_t ReadU32(const std::uint8_t* p) noexcept
        {
            return (static_cast<std::uint32_t>(p[3]) << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
        }

        std::uint32_t ReadU16(const std::uint8_t* p) noexcept
        {
            return static_cast<std::uint32_t>((p[1] << 8) | p[0]);
        }

        // opus-tools: sanitize_fourcc() (src/audio-in.c)
        void SanitizeFourcc(std::uint8_t* p) noexcept
        {
            for (std::int32_t i = 0; i < 4; ++i)
                if (p[i] < ' ' || p[i] > '~')
                    p[i] = '?';
        }

        // opus-tools: find_wav_chunk() (src/audio-in.c)
        bool FindWavChunk(InputFile& file, const char* type, std::uint32_t& length, ReportFunction report) noexcept
        {
            for (;;) {
                std::uint8_t header[8];
                if (file.Read(header, 8) < 8)
                    return false;
                const std::uint32_t chunkLength = ReadU32(header + 4);
                if (std::memcmp(header, type, 4) != 0) {
                    SanitizeFourcc(header);
                    Report(report, L"Skipping chunk of type \"%.4hs\", length %u\n", reinterpret_cast<const char*>(header), chunkLength);
                    if (!file.SeekForward(static_cast<std::int64_t>(chunkLength) + (chunkLength & 1)))
                        return false;
                } else {
                    length = chunkLength;
                    return true;
                }
            }
        }

        // opus-tools: read_chunk() (src/audio-in.c)
        bool ReadChunk(InputFile& file, std::uint8_t* buffer, std::uint32_t bufferSize, std::uint32_t& length) noexcept
        {
            const std::uint32_t chunkLength = length;
            const std::uint32_t readLength  = chunkLength > bufferSize ? bufferSize : chunkLength;
            if (file.Read(buffer, readLength) != readLength)
                return false;
            if (!file.SeekForward(static_cast<std::int64_t>(chunkLength - readLength) + (chunkLength & 1)))
                return false;
            length = readLength;
            return true;
        }

    } // namespace

    // --- InputFile -------------------------------------------------------------------------------

    bool InputFile::Open(const wchar_t* path) noexcept
    {
        Close();
        if (path[0] == L'-' && path[1] == 0) {
            handle_   = GetStdHandle(STD_INPUT_HANDLE);
            ownsFile_ = false;
        } else {
            handle_   = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            ownsFile_ = true;
        }
        if (handle_ == INVALID_HANDLE_VALUE || handle_ == nullptr) {
            handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        canSeek_ = GetFileType(handle_) == FILE_TYPE_DISK;
        if (!buffer_.Allocate(InputBufferSize)) {
            Close();
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
        begin_ = end_ = 0;
        eof_          = false;
        return true;
    }

    void InputFile::Close() noexcept
    {
        if (ownsFile_ && handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
        handle_   = INVALID_HANDLE_VALUE;
        ownsFile_ = false;
    }

    std::size_t InputFile::Read(void* buffer, std::size_t size) noexcept
    {
        auto*       dst  = static_cast<std::byte*>(buffer);
        std::size_t done = 0;
        while (done < size) {
            if (begin_ == end_) {
                if (eof_)
                    break;
                if (source_ != nullptr) {
                    std::size_t got = 0;
                    if (!source_->Next(data_, got) || got == 0) {
                        eof_ = true;
                        break;
                    }
                    begin_ = 0;
                    end_   = got;
                } else {
                    unsigned long got = 0; // DWORD of ReadFile
                    if (!ReadFile(handle_, buffer_.Data(), static_cast<unsigned long>(buffer_.Size()), &got, nullptr) || got == 0) {
                        eof_ = true;
                        break;
                    }
                    data_  = buffer_.Data();
                    begin_ = 0;
                    end_   = got;
                }
            }
            const std::size_t take = end_ - begin_ < size - done ? end_ - begin_ : size - done;
            std::memcpy(dst + done, data_ + begin_, take);
            begin_ += take;
            done += take;
        }
        return done;
    }

    std::size_t InputFile::ReadItems(void* buffer, std::size_t itemSize, std::size_t count) noexcept
    {
        if (itemSize == 0 || count == 0)
            return 0;
        const std::size_t bytes = Read(buffer, itemSize * count);
        return bytes / itemSize;
    }

    // opus-tools: seek_forward() (src/audio-in.c)
    bool InputFile::SeekForward(std::int64_t length) noexcept
    {
        if (length <= 0)
            return true;
        const auto buffered = static_cast<std::int64_t>(end_ - begin_);
        if (length <= buffered) {
            begin_ += static_cast<std::size_t>(length);
            return true;
        }
        if (canSeek_) {
            LARGE_INTEGER distance;
            distance.QuadPart = length - buffered;
            if (SetFilePointerEx(handle_, distance, nullptr, FILE_CURRENT)) {
                begin_ = end_ = 0;
                eof_          = false;
                return true;
            }
        }
        // cannot seek: skip by reading
        std::byte    scratch[1024];
        std::int64_t remaining = length;
        while (remaining > 0) {
            const std::size_t step = remaining > 1024 ? 1024 : static_cast<std::size_t>(remaining);
            const std::size_t got  = Read(scratch, step);
            if (got == 0)
                return false;
            remaining -= static_cast<std::int64_t>(got);
        }
        return true;
    }

    std::int64_t InputFile::Remaining() const noexcept
    {
        if (!canSeek_)
            return -1;
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        LARGE_INTEGER position;
        LARGE_INTEGER size;
        if (!SetFilePointerEx(handle_, zero, &position, FILE_CURRENT) || !GetFileSizeEx(handle_, &size))
            return -1;
        return size.QuadPart - (position.QuadPart - static_cast<std::int64_t>(end_ - begin_));
    }

    std::int64_t InputFile::HandlePosition() const noexcept
    {
        if (!canSeek_)
            return -1;
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        LARGE_INTEGER position;
        if (!SetFilePointerEx(handle_, zero, &position, FILE_CURRENT))
            return -1;
        return eof_ ? -1 : position.QuadPart;
    }

    // --- WavInput --------------------------------------------------------------------------------

    // opus-tools: wav_open() (src/audio-in.c) with wav_id()
    bool WavInput::Open(InputFile& file, bool ignoreLength, ReportFunction report) noexcept
    {
        file_ = &file;

        // open_audio_file: wav_id on the first 12 bytes
        std::uint8_t id[12];
        if (file.Read(id, 12) < 12 || std::memcmp(id, "RIFF", 4) != 0 || std::memcmp(id + 8, "WAVE", 4) != 0)
            return false;

        // wav_open
        std::uint32_t length = 0;
        if (!FindWavChunk(file, "fmt ", length, report)) {
            Report(report, L"ERROR: No format chunk found in WAV file\n");
            return false;
        }
        if (length < 16) {
            Report(report, L"ERROR: Unrecognised format chunk in WAV header\n");
            return false;
        }
        if (length != 16 && length != 18 && length != 40)
            Report(report, L"Warning: INVALID format chunk in wav header.\n Trying to read anyway (may not work)...\n");

        std::uint8_t buf[40] = {};
        if (!ReadChunk(file, buf, sizeof(buf), length)) {
            Report(report, L"ERROR: Incomplete format chunk in WAV header\n");
            return false;
        }
        std::uint32_t       format     = ReadU16(buf);
        const std::uint32_t channels   = ReadU16(buf + 2);
        const std::uint32_t sampleRate = ReadU32(buf + 4);
        const std::uint32_t align      = ReadU16(buf + 12);
        const std::uint32_t sampleBits = ReadU16(buf + 14);
        std::int32_t        validBits  = 0;
        if (channels == 0) {
            Report(report, L"ERROR: Zero channels in WAV header\n");
            return false;
        }
        if (format == 0xfffe) { // WAVE_FORMAT_EXTENSIBLE
            if (length < 40) {
                Report(report, L"ERROR: Extended WAV format header invalid (too small)\n");
                return false;
            }
            validBits = static_cast<std::int32_t>(ReadU16(buf + 18));
            if (validBits < 1 || validBits > static_cast<std::int32_t>(sampleBits))
                validBits = static_cast<std::int32_t>(sampleBits);
            const std::uint32_t mask = ReadU32(buf + 20);
            switch (mask) {
                case 1539:
                    Report(report,
                           L"WARNING: WAV file uses side surround instead of rear for quadraphonic;\n"
                           L"remapping side speakers to rear in encoding.\n");
                    break;
                case 1551:
                    Report(report,
                           L"WARNING: WAV file uses side surround instead of rear for 5.1;\n"
                           L"remapping side speakers to rear in encoding.\n");
                    break;
                case 319:
                    Report(report,
                           L"WARNING: WAV file uses rear surround instead of side for 6.1;\n"
                           L"remapping rear speakers to side in encoding.\n");
                    break;
                case 255:
                    Report(report,
                           L"WARNING: WAV file is a 7.1 'Widescreen' channel mapping;\n"
                           L"remapping speakers to Vorbis 7.1 format.\n");
                    break;
                case 0:
                case 1:
                case 4:
                case 3:
                case 51:
                case 55:
                case 63:
                case 1807:
                case 1599: break;
                default:
                    Report(report,
                           L"WARNING: Unknown WAV surround channel mask: %u\n"
                           L"Blindly mapping speakers using default SMPTE/ITU ordering.\n",
                           mask);
                    break;
            }
            format = ReadU16(buf + 24);
        } else {
            validBits = static_cast<std::int32_t>(sampleBits);
        }

        std::int32_t sampleBytes = 0;
        if (format == 1) {
            sampleBytes = static_cast<std::int32_t>(sampleBits / 8);
            ieeeFloat_  = false;
        } else if (format == 3) {
            validBits   = 24;
            sampleBytes = 4;
            ieeeFloat_  = true;
        } else {
            Report(report,
                   L"ERROR: Unsupported WAV file type.\n"
                   L"Must be standard PCM or type 3 floating point PCM.\n");
            return false;
        }
        if (sampleRate > INT_MAX) {
            Report(report, L"ERROR: Preposterous sample rate in WAV header: %u Hz\n", sampleRate);
            return false;
        }
        if (!FindWavChunk(file, "data", length, report)) {
            Report(report, L"ERROR: No data chunk found in WAV file\n");
            return false;
        }
        if (align != channels * static_cast<std::uint32_t>(sampleBytes))
            Report(report,
                   L"Warning: WAV 'block alignment' value is incorrect, ignoring.\n"
                   L"The software that created this file is incorrect.\n");

        const bool supported = sampleBits == static_cast<std::uint32_t>(sampleBytes) * 8
            && (sampleBits == 24 || sampleBits == 16 || sampleBits == 8 || (sampleBits == 32 && format == 3));
        if (!supported) {
            Report(report,
                   L"ERROR: Unsupported WAV sample size.\n"
                   L"Must be 8, 16, or 24 bit PCM or 32 bit floating point PCM.\n");
            return false;
        }

        info_.rate                   = static_cast<std::int32_t>(sampleRate);
        info_.channels               = static_cast<std::int32_t>(channels);
        info_.inputChannels          = info_.channels;
        info_.sampleBits             = validBits;
        info_.totalSamplesPerChannel = 0;
        fileChannels_                = info_.channels;
        sampleSizeBits_              = static_cast<std::int32_t>(sampleBits);
        samplesRead_                 = 0;
        totalSamples_                = 0;

        const std::uint32_t frameBytes = channels * static_cast<std::uint32_t>(sampleBytes);
        if (ignoreLength) {
            // until EOF, no progress percentage
        } else if (length > frameBytes * 4u && length < ((1u << 31) - 65536)) {
            totalSamples_ = info_.totalSamplesPerChannel = length / frameBytes;
        } else {
            const std::int64_t remaining = file.Remaining();
            if (remaining > 0)
                info_.totalSamplesPerChannel = remaining / frameBytes;
        }

        if (!permute_.Allocate(channels))
            return false;
        for (std::uint32_t c = 0; c < channels; ++c)
            permute_[c] = channels <= 8 ? WavPermute[channels - 1][c] : static_cast<std::int32_t>(c);
        return true;
    }

    // opus-tools: setup_downmix() (src/audio-in.c)
    std::int32_t WavInput::SetupDownmix(std::int32_t outChannels, ReportFunction report) noexcept
    {
        const std::int32_t inChannels = info_.channels;
        if (inChannels <= outChannels || outChannels > 2 || inChannels <= 0 || outChannels <= 0) {
            Report(report, L"Downmix must actually downmix and only knows mono/stereo out.\n");
            return 0;
        }
        if (outChannels == 2 && inChannels > 8) {
            Report(report, L"Downmix only knows how to mix >8ch to mono.\n");
            return 0;
        }
        if (!downmixBuffer_.Allocate(static_cast<std::size_t>(inChannels) * 4096)
            || !downmixMatrix_.Allocate(static_cast<std::size_t>(inChannels) * outChannels))
            return 0;

        float* matrix = downmixMatrix_.Data();
        if (outChannels == 1 && inChannels > 8) {
            for (std::int32_t i = 0; i < inChannels; ++i)
                matrix[i] = 1.0f / inChannels;
        } else if (outChannels == 2) {
            for (std::int32_t j = 0; j < outChannels; ++j)
                for (std::int32_t i = 0; i < inChannels; ++i)
                    matrix[inChannels * j + i] = StereoMix[inChannels - 2][i][j];
        } else {
            for (std::int32_t i = 0; i < inChannels; ++i)
                matrix[i] = StereoMix[inChannels - 2][i][0] + StereoMix[inChannels - 2][i][1];
        }
        float sum = 0;
        for (std::int32_t i = 0; i < inChannels * outChannels; ++i)
            sum += matrix[i];
        sum = static_cast<float>(outChannels) / sum;
        for (std::int32_t i = 0; i < inChannels * outChannels; ++i)
            matrix[i] *= sum;

        downmix_       = true;
        info_.channels = outChannels;
        return outChannels;
    }

    // opus-tools: read_downmix() (src/audio-in.c)
    std::int32_t WavInput::Read(float* buffer, std::int32_t samples) noexcept
    {
        if (!downmix_)
            return ReadPcm(buffer, samples);
        // read_downmix, in pieces of at most the 4096 frames of its buffer
        const std::int32_t inCh   = fileChannels_;
        const std::int32_t outCh  = info_.channels;
        const float*       bufs   = downmixBuffer_.Data();
        const float*       matrix = downmixMatrix_.Data();
        std::int32_t       total  = 0;
        while (total < samples) {
            const std::int32_t want      = samples - total < 4096 ? samples - total : 4096;
            const std::int32_t inSamples = ReadPcm(downmixBuffer_.Data(), want);
            float*             out       = buffer + static_cast<std::size_t>(total) * outCh;
            for (std::int32_t i = 0; i < inSamples; ++i) {
                for (std::int32_t j = 0; j < outCh; ++j) {
                    float& samp = out[i * outCh + j];
                    samp        = 0;
                    for (std::int32_t k = 0; k < inCh; ++k)
                        samp += bufs[i * inCh + k] * matrix[inCh * j + k];
                }
            }
            total += inSamples;
            if (inSamples < want)
                break;
        }
        return total;
    }

    // opus-tools: wav_read() and wav_ieee_read() (src/audio-in.c)
    std::int32_t WavInput::ReadPcm(float* buffer, std::int32_t samples) noexcept
    {
        const std::int32_t sampleBytes = ieeeFloat_ ? 4 : sampleSizeBits_ / 8;
        const std::int32_t channels    = fileChannels_;
        std::int32_t       realSamples
            = totalSamples_ > 0 && samples > (totalSamples_ - samplesRead_) ? static_cast<std::int32_t>(totalSamples_ - samplesRead_) : samples;
        const std::size_t frameBytes = static_cast<std::size_t>(sampleBytes) * channels;
        if (raw_.Size() < frameBytes * static_cast<std::size_t>(realSamples) && !raw_.Allocate(frameBytes * static_cast<std::size_t>(realSamples)))
            return 0;

        realSamples = static_cast<std::int32_t>(file_->ReadItems(raw_.Data(), frameBytes, static_cast<std::size_t>(realSamples)));
        samplesRead_ += realSamples;

        const auto*         bytes   = reinterpret_cast<const std::uint8_t*>(raw_.Data());
        const std::int32_t* permute = permute_.Data();
        if (ieeeFloat_) {
            const auto* f = reinterpret_cast<const float*>(raw_.Data());
            for (std::int32_t i = 0; i < realSamples; ++i)
                for (std::int32_t j = 0; j < channels; ++j)
                    buffer[i * channels + j] = f[i * channels + permute[j]];
        } else if (sampleSizeBits_ == 8) {
            // WAV 8-bit samples are unsigned
            for (std::int32_t i = 0; i < realSamples; ++i)
                for (std::int32_t j = 0; j < channels; ++j)
                    buffer[i * channels + j] = (static_cast<std::int32_t>(bytes[i * channels + permute[j]]) - 128) / 128.0f;
        } else if (sampleSizeBits_ == 16) {
            for (std::int32_t i = 0; i < realSamples; ++i) {
                for (std::int32_t j = 0; j < channels; ++j) {
                    const std::uint8_t* p    = bytes + 2 * (i * channels + permute[j]);
                    const std::int32_t  v    = (static_cast<std::int8_t>(p[1]) << 8) | (p[0] & 0xff);
                    buffer[i * channels + j] = v / 32768.0f;
                }
            }
        } else if (sampleSizeBits_ == 24) {
            for (std::int32_t i = 0; i < realSamples; ++i) {
                for (std::int32_t j = 0; j < channels; ++j) {
                    const std::uint8_t* p    = bytes + 3 * (i * channels + permute[j]);
                    const std::int32_t  v    = (static_cast<std::int8_t>(p[2]) << 16) | (p[1] << 8) | (p[0] & 0xff);
                    buffer[i * channels + j] = v / 8388608.0f;
                }
            }
        } else {
            return 0;
        }
        return realSamples;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
