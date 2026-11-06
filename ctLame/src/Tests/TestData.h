// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "File.h"
#include "Formatter.h"
#include "Memory.h"
#include "TestEnvironment.h"
#include "Text.h"

namespace LameTests {

    namespace Shared = ThinkMeta::ConcurrentTasks::Samples::Shared;
    using Shared::Char;
    using Shared::TestEnvironment;

    // Helpers of the golden tests: the FNV-1a checksum and the test WAV files.

    // raw bytes from integer or character literals: const auto data = Bytes(0x01, 0x02);
    template <std::size_t N>
    struct ByteArray {
        std::byte bytes[N];

        const std::byte* data() const noexcept { return bytes; }
        std::size_t      size() const noexcept { return N; }
    };

    template <typename... T>
    constexpr ByteArray<sizeof...(T)> Bytes(T... values) noexcept
    {
        return {{static_cast<std::byte>(values)...}};
    }

    struct Checksum {
        std::uint64_t value = 1469598103934665603ull;

        void Add(const void* data, std::size_t size)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < size; ++i) {
                value ^= bytes[i];
                value *= 1099511628211ull;
            }
        }
    };

    // the whole file in memory, with the format and data chunk of a 16 bit PCM WAV
    struct WavData {
        std::byte*       file     = nullptr;
        const std::byte* data     = nullptr;
        std::size_t      size     = 0;
        std::int32_t     channels = 0;
        std::int32_t     rate     = 0;

        WavData()                          = default;
        WavData(const WavData&)            = delete;
        WavData& operator=(const WavData&) = delete;

        ~WavData()
        {
            if (file != nullptr)
                Shared::Heap::Free(file);
        }

        bool Load(const Char* path)
        {
            std::uint64_t length = 0;
            if (!Shared::File::Size(path, length))
                return false;
            const auto read = static_cast<std::size_t>(length);
            file            = static_cast<std::byte*>(Shared::Heap::Allocate(read));
            if (file == nullptr || !Shared::File::Read(path, file, read))
                return false;

            std::size_t pos = 12;
            while (pos + 8 <= read) {
                const auto chunk = *reinterpret_cast<const std::uint32_t*>(file + pos + 4);
                if (std::memcmp(file + pos, "fmt ", 4) == 0) {
                    channels = *reinterpret_cast<const std::uint16_t*>(file + pos + 10);
                    rate     = *reinterpret_cast<const std::int32_t*>(file + pos + 12);
                } else if (std::memcmp(file + pos, "data", 4) == 0) {
                    data = file + pos + 8;
                    size = chunk < read - pos - 8 ? chunk : read - pos - 8;
                    return channels > 0;
                }
                pos += 8 + chunk + (chunk & 1);
            }
            return false;
        }
    };

    // The golden inputs come from the environment (Shared::TestEnvironment): CTLAME_TEST_CORPUS names the corpus file
    // the checksums were recorded with, CTLAME_TEST_SYNTH the folder of the synthetic files they were recorded
    // with. Without them the golden tests are skipped.
    inline const Char* CorpusFile()
    {
        static Char                        path[Shared::File::MaxPath];
        [[maybe_unused]] static const bool set = TestEnvironment::Get(CT_TEXT("CTLAME_TEST_CORPUS"), path);
        return path;
    }

    inline const Char* SynthDir()
    {
        static Char                        path[Shared::File::MaxPath];
        [[maybe_unused]] static const bool set = TestEnvironment::Folder(CT_TEXT("CTLAME_TEST_SYNTH"), path);
        return path;
    }

    // printf into a character array (Shared::Formatter), cut off at its end
    template <std::size_t N, typename... T>
    void FormatTo(Char (&text)[N], const Char* format, T... values) noexcept
    {
        Shared::Formatter::Format(text, static_cast<std::int32_t>(N), format, values...);
    }

    // The log line of a golden test whose input file is missing: the variable to set, or the file that is not there.
    inline const Char* MissingInputText(Char (&text)[400], const Char* test, const Char* path)
    {
        const bool corpus = path == CorpusFile();
        if ((corpus ? CorpusFile() : SynthDir())[0] == 0) {
            Char skip[200];
            TestEnvironment::SkipText(skip, corpus ? CT_TEXT("CTLAME_TEST_CORPUS") : CT_TEXT("CTLAME_TEST_SYNTH"),
                                      corpus ? CT_TEXT("the corpus file of the checksums") : CT_TEXT("the folder of the synthetic files of the checksums"));
            FormatTo(text, CT_TEXT("%s: %s"), test, skip);
        } else
            FormatTo(text, CT_TEXT("%s: skipped, %s not found"), test, path);
        return text;
    }

} // namespace LameTests
