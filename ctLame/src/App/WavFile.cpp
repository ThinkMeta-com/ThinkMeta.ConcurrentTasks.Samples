// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "WavFile.h"

#include <array>
#include <cstdint>

#include "Console.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    using Shared::Console;
    using Shared::Stream;

    namespace {
        constexpr std::uint32_t WavIdRiff = 0x52494646; // "RIFF"
        constexpr std::uint32_t WavIdWave = 0x57415645; // "WAVE"
        constexpr std::uint32_t WavIdFmt  = 0x666d7420; // "fmt "
        constexpr std::uint32_t WavIdData = 0x64617461; // "data"

        constexpr std::uint16_t WaveFormatPcm        = 0x0001;
        constexpr std::uint16_t WaveFormatIeeeFloat  = 0x0003;
        constexpr std::uint16_t WaveFormatExtensible = 0xFFFE;

        std::uint32_t MakeEvenNumberOfBytesInLength(std::uint32_t x) noexcept
        {
            return x + (x & 0x01);
        }
    } // namespace

    bool WavFile::Open(const wchar_t* path, EncoderOptions& options, bool ignoreLength, std::int32_t silent)
    {
        Close();
        pcmBitWidth_           = 16;
        pcmIsIeeeFloat_        = false;
        countSamplesCarefully_ = false;

        // set the defaults from info in case we cannot determine them from the file
        options.SetNumSamples(UnknownNumSamples);

        // like _wfopen(path, L"rb"): shared for reading and writing
        file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            if (silent < 10)
                Console::Print(Stream::Error, L"Could not find \"%ls\".\n", path);
            return false;
        }

        if (!ParseFileHeader(options, ignoreLength, silent)) {
            Close();
            return false;
        }

        LARGE_INTEGER position{};
        SetFilePointerEx(file_, LARGE_INTEGER{}, &position, FILE_CURRENT);
        dataOffset_ = static_cast<std::uint64_t>(position.QuadPart);

        if (options.numSamples == UnknownNumSamples) {
            const std::int32_t numChannels = options.numChannels;
            LARGE_INTEGER      size{};
            const double       flen = GetFileSizeEx(file_, &size) ? static_cast<double>(size.QuadPart) : -1.0; // try to figure out num_samples
            if (flen >= 0 && numChannels > 0) {
                // try file size, assume 2 bytes per sample
                const std::uint32_t fsize = static_cast<std::uint32_t>(flen / (2 * numChannels));
                options.SetNumSamples(fsize);
                countSamplesCarefully_ = false;
            }
        }
        return true;
    }

    void WavFile::Close() noexcept
    {
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
    }

    // parse_file_header: returns 1 for a supported WAV file, 0 otherwise.
    std::int32_t WavFile::ParseFileHeader(EncoderOptions& options, bool ignoreLength, std::int32_t silent)
    {
        const std::uint32_t type = Read32BitsHighLow();

        countSamplesCarefully_ = false;
        if (type == WavIdRiff) {
            // It's probably a WAV file
            const std::int32_t ret = ParseWaveHeader(options, silent);
            if (ret > 0) {
                if (options.numSamples == UnknownNumSamples || ignoreLength) {
                    countSamplesCarefully_ = false;
                    options.SetNumSamples(UnknownNumSamples);
                } else {
                    countSamplesCarefully_ = true;
                }
                return 1;
            }
            if (ret < 0) {
                if (silent < 10)
                    Console::Print(Stream::Error, L"Warning: corrupt or unsupported WAVE format\n");
            }
        } else {
            if (silent < 10)
                Console::Print(Stream::Error, L"Warning: unsupported audio format, only WAVE files are supported\n");
        }
        return 0;
    }

    // parse_wave_header: 1 = supported, 0 = unsupported format, -1 = corrupt.
    std::int32_t WavFile::ParseWaveHeader(EncoderOptions& options, std::int32_t silent)
    {
        std::uint32_t samplesPerSec = 0;
        std::uint32_t dataChunkSize = 0;
        std::uint16_t formatTag     = 0;
        std::uint16_t channels      = 0;
        std::uint16_t bitsPerSample = 0;
        bool          isWav         = false;

        const std::uint32_t chunkSize = Read32BitsHighLow(); // file_length
        const std::uint32_t waveId    = Read32BitsHighLow();
        if (waveId != WavIdWave || chunkSize < 1)
            return -1;

        for (std::int32_t loopSanity = 0; loopSanity < 20; ++loopSanity) {
            const std::uint32_t ckId = Read32BitsHighLow();
            if (ckId == WavIdFmt) {
                std::uint32_t cksize = Read32BitsLowHigh();
                cksize               = MakeEvenNumberOfBytesInLength(cksize);
                if (cksize < 16u)
                    return -1; // 'fmt' chunk too short

                formatTag     = Read16BitsLowHigh();
                channels      = Read16BitsLowHigh();
                samplesPerSec = Read32BitsLowHigh();
                (void)Read32BitsLowHigh(); // nAvgBytesPerSec
                (void)Read16BitsLowHigh(); // nBlockAlign
                bitsPerSample = Read16BitsLowHigh();
                cksize -= 16u;

                // WAVE_FORMAT_EXTENSIBLE support
                if ((cksize > 9u) && (formatTag == WaveFormatExtensible)) {
                    (void)Read16BitsLowHigh(); // cbSize
                    (void)Read16BitsLowHigh(); // wValidBitsPerSample
                    (void)Read32BitsLowHigh(); // dwChannelMask
                    const std::uint16_t subFormat = Read16BitsLowHigh();
                    cksize -= 10u;
                    formatTag = subFormat; // SubType coincident with format_tag for PCM int or float
                }
                if (cksize > 0) {
                    if (Skip(cksize) != 0)
                        return -1;
                }
            } else if (ckId == WavIdData) {
                dataChunkSize = Read32BitsLowHigh();
                isWav         = true;
                // We've found the audio data. Read no further!
                break;
            } else {
                std::uint32_t cksize = Read32BitsLowHigh();
                cksize               = MakeEvenNumberOfBytesInLength(cksize);
                if (Skip(cksize) != 0)
                    return -1;
            }
        }

        if (!isWav)
            return -1;

        if (formatTag != WaveFormatPcm && formatTag != WaveFormatIeeeFloat) {
            if (silent < 10)
                Console::Print(Stream::Error, L"Unsupported data format: 0x%04X\n", formatTag);
            return 0; // oh no! non-supported format
        }
        // make sure the header is sane
        if (!SetInputNumChannels(options, channels, silent))
            return 0;
        if (!SetInputSamplerate(options, static_cast<std::int32_t>(samplesPerSec), silent))
            return 0;
        // avoid division by zero
        if (bitsPerSample < 1) {
            if (silent < 10)
                Console::Print(Stream::Error, L"Unsupported bits per sample: %d\n", bitsPerSample);
            return -1;
        }
        pcmBitWidth_    = bitsPerSample;
        pcmIsIeeeFloat_ = formatTag == WaveFormatIeeeFloat;
        if (dataChunkSize == 0xFFFFFFFFu)
            options.SetNumSamples(UnknownNumSamples);
        else
            options.SetNumSamples(dataChunkSize / (channels * ((bitsPerSample + 7u) / 8u)));
        return 1;
    }

    bool WavFile::SetInputNumChannels(EncoderOptions& options, std::int32_t numChannels, std::int32_t silent)
    {
        if (-1 == options.SetNumChannels(numChannels)) {
            if (silent < 10)
                Console::Print(Stream::Error, L"Unsupported number of channels: %d\n", numChannels);
            return false;
        }
        return true;
    }

    bool WavFile::SetInputSamplerate(EncoderOptions& options, std::int32_t samplerate, std::int32_t silent)
    {
        // the input is never resampled, so only the MPEG-1 sample rates can be encoded
        if (samplerate != 32000 && samplerate != 44100 && samplerate != 48000) {
            if (silent < 10)
                Console::Print(Stream::Error, L"Unsupported sample rate: %d Hz (supported: 32, 44.1 and 48 kHz)\n", samplerate);
            return false;
        }
        if (-1 == options.SetInSamplerate(samplerate)) {
            if (silent < 10)
                Console::Print(Stream::Error, L"Unsupported sample rate: %d\n", samplerate);
            return false;
        }
        return true;
    }

    // Like fread: bytes beyond the end of the file stay 0.
    void WavFile::ReadBytes(std::uint8_t* bytes, std::uint32_t count) noexcept
    {
        unsigned long read = 0; // DWORD of ReadFile
        (void)ReadFile(file_, bytes, count, &read, nullptr);
    }

    std::uint16_t WavFile::Read16BitsLowHigh() noexcept
    {
        std::array<std::uint8_t, 2> bytes = {0, 0};
        ReadBytes(bytes.data(), 2);
        const std::uint16_t l = bytes[0];
        const std::uint16_t h = bytes[1];
        return static_cast<std::uint16_t>((h << 8) | l);
    }

    std::uint32_t WavFile::Read32BitsLowHigh() noexcept
    {
        std::array<std::uint8_t, 4> bytes = {0, 0, 0, 0};
        ReadBytes(bytes.data(), 4);
        const std::uint32_t ll = bytes[0];
        const std::uint32_t lh = bytes[1];
        const std::uint32_t hl = bytes[2];
        const std::uint32_t hh = bytes[3];
        return (hh << 24) | (hl << 16) | (lh << 8) | ll;
    }

    std::uint32_t WavFile::Read32BitsHighLow() noexcept
    {
        std::array<std::uint8_t, 4> bytes = {0, 0, 0, 0};
        ReadBytes(bytes.data(), 4);
        const std::uint32_t hh = bytes[0];
        const std::uint32_t hl = bytes[1];
        const std::uint32_t lh = bytes[2];
        const std::uint32_t ll = bytes[3];
        return (hh << 24) | (hl << 16) | (lh << 8) | ll;
    }

    // fskip_uint32: a forward seek on a regular file; it may go beyond the end of the file.
    std::int32_t WavFile::Skip(std::uint32_t offset) noexcept
    {
        LARGE_INTEGER distance{};
        distance.QuadPart = offset;
        return SetFilePointerEx(file_, distance, nullptr, FILE_CURRENT) ? 0 : -1;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
