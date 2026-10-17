// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

#include "HeapArray.h"
#include "Report.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    /// <summary>
    /// Where an InputFile takes its bytes from once a source is attached (asynchronous reading by
    /// another task): Next hands out the next block, valid until the next call; false at the end.
    /// </summary>
    class InputSource
    {
    public:
        virtual bool Next(const std::byte*& data, std::size_t& size) noexcept = 0;

    protected:
        ~InputSource() = default;
    };

    /// <summary>Buffered reading from a file or stdin, with the fread/fseek semantics opusenc relies on.</summary>
    class InputFile
    {
    public:
        InputFile() noexcept                   = default;
        InputFile(const InputFile&)            = delete;
        InputFile& operator=(const InputFile&) = delete;
        ~InputFile() { Close(); }

        /// <summary>Opens a file for reading; "-" is stdin. False with GetLastError set on failure.</summary>
        bool Open(const wchar_t* path) noexcept;
        void Close() noexcept;

        /// <summary>Reads up to size bytes; fewer only at the end of the input (fread).</summary>
        std::size_t Read(void* buffer, std::size_t size) noexcept;

        /// <summary>Reads count items of itemSize bytes; returns the number of complete items (fread).</summary>
        std::size_t ReadItems(void* buffer, std::size_t itemSize, std::size_t count) noexcept;

        /// <summary>seek_forward: skips bytes, by seeking or else by reading; false at the end of the input.</summary>
        bool SeekForward(std::int64_t length) noexcept;

        /// <summary>Bytes from the current position to the end, or -1 if the input cannot seek.</summary>
        std::int64_t Remaining() const noexcept;

        /// <summary>The offset of the next byte the file handle delivers (behind the buffered ones); -1 for stdin or a pipe.</summary>
        std::int64_t HandlePosition() const noexcept;

        /// <summary>
        /// From now on the bytes behind the buffered ones come from source, which reads the file from
        /// HandlePosition on; the input can no longer seek (SeekForward skips by reading).
        /// </summary>
        void Attach(InputSource* source) noexcept
        {
            source_  = source;
            canSeek_ = false;
        }

    private:
        HANDLE               handle_   = INVALID_HANDLE_VALUE;
        bool                 ownsFile_ = false;
        bool                 canSeek_  = false;
        InputSource*         source_   = nullptr;
        const std::byte*     data_     = nullptr; // the buffered bytes: buffer_ or the source's block
        HeapArray<std::byte> buffer_;
        std::size_t          begin_ = 0;
        std::size_t          end_   = 0;
        bool                 eof_   = false;
    };

    /// <summary>
    /// Port of the WAV reader of opus-tools 0.2 (audio-in.c: open_audio_file with wav_id/wav_open,
    /// wav_read, wav_ieee_read) and of its mono/stereo downmix (setup_downmix, read_downmix).
    /// Only WAV input: AIFF, FLAC and raw PCM are not supported.
    /// </summary>
    class WavInput
    {
    public:
        struct Info {
            std::int32_t channels               = 0; // after the downmix
            std::int32_t inputChannels          = 0;
            std::int32_t rate                   = 0;
            std::int32_t sampleBits             = 16; // opt->samplesize: valid bits, 24 for float
            std::int64_t totalSamplesPerChannel = 0;  // for the progress display only
        };

        /// <summary>False if the input is not a supported WAV file (messages reported as opusenc prints them).</summary>
        bool Open(InputFile& file, bool ignoreLength, ReportFunction report) noexcept;

        /// <summary>setup_downmix to 1 or 2 channels; returns the new channel count, 0 if it cannot downmix.</summary>
        std::int32_t SetupDownmix(std::int32_t outChannels, ReportFunction report) noexcept;

        /// <summary>Reads up to samples frames as interleaved floats; returns the frames read (0 at the end).</summary>
        std::int32_t Read(float* buffer, std::int32_t samples) noexcept;

        const Info& GetInfo() const noexcept { return info_; }

    private:
        std::int32_t ReadPcm(float* buffer, std::int32_t samples) noexcept;

        InputFile*              file_ = nullptr;
        Info                    info_;
        std::int32_t            fileChannels_   = 0;
        std::int32_t            sampleSizeBits_ = 0; // wavfile.samplesize: 8, 16, 24 or 32 (float)
        bool                    ieeeFloat_      = false;
        std::int64_t            totalSamples_   = 0; // limit from the data chunk length, 0 = until EOF
        std::int64_t            samplesRead_    = 0;
        HeapArray<std::int32_t> permute_;
        HeapArray<std::byte>    raw_;
        bool                    downmix_ = false;
        HeapArray<float>        downmixBuffer_;
        HeapArray<float>        downmixMatrix_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
