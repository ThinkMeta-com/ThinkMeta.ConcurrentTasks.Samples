// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <Windows.h>

#include <cstdint>

#include "EncoderOptions.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// Opens a WAV file and reads its header, port of init_infile/open_wave_file and the header
    /// parser of frontend/get_audio.c. Sets the number of channels, the sample rate and the number
    /// of samples in the encoder options, exactly like LAME sets them in the global flags. The
    /// file stays open, positioned at the first sample.
    /// </summary>
    class WavFile
    {
    public:
        WavFile() noexcept = default;
        ~WavFile() { Close(); }

        WavFile(const WavFile&)            = delete;
        WavFile& operator=(const WavFile&) = delete;

        /// <summary>
        /// Opens the file and parses its header. Messages go to error unless silent is 10 or more
        /// (--quiet). Returns false if the file cannot be opened or is not a supported WAV file.
        /// </summary>
        bool Open(const wchar_t* path, EncoderOptions& options, bool ignoreLength, std::int32_t silent);

        void Close() noexcept;

        HANDLE Handle() const noexcept { return file_; }

        /// <summary>Bits per sample as the header says (pcmbitwidth).</summary>
        std::int32_t PcmBitWidth() const noexcept { return pcmBitWidth_; }

        /// <summary>WAVE_FORMAT_IEEE_FLOAT samples.</summary>
        bool IsIeeeFloat() const noexcept { return pcmIsIeeeFloat_; }

        /// <summary>Read exactly the number of samples of the header and no more (count_samples_carefully).</summary>
        bool CountSamplesCarefully() const noexcept { return countSamplesCarefully_; }

        /// <summary>File offset of the first sample.</summary>
        std::uint64_t DataOffset() const noexcept { return dataOffset_; }

    private:
        std::int32_t ParseFileHeader(EncoderOptions& options, bool ignoreLength, std::int32_t silent);
        std::int32_t ParseWaveHeader(EncoderOptions& options, std::int32_t silent);
        bool         SetInputNumChannels(EncoderOptions& options, std::int32_t numChannels, std::int32_t silent);
        bool         SetInputSamplerate(EncoderOptions& options, std::int32_t samplerate, std::int32_t silent);

        std::uint16_t Read16BitsLowHigh() noexcept;
        std::uint32_t Read32BitsLowHigh() noexcept;
        std::uint32_t Read32BitsHighLow() noexcept;
        void          ReadBytes(std::uint8_t* bytes, std::uint32_t count) noexcept;
        std::int32_t  Skip(std::uint32_t offset) noexcept;

        HANDLE        file_                  = INVALID_HANDLE_VALUE;
        std::int32_t  pcmBitWidth_           = 16;
        bool          pcmIsIeeeFloat_        = false;
        bool          countSamplesCarefully_ = false;
        std::uint64_t dataOffset_            = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
