// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ctLameTests {

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
                HeapFree(GetProcessHeap(), 0, file);
        }

        bool Load(const wchar_t* path)
        {
            HANDLE const handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return false;
            LARGE_INTEGER length{};
            GetFileSizeEx(handle, &length);
            file               = static_cast<std::byte*>(HeapAlloc(GetProcessHeap(), 0, static_cast<std::size_t>(length.QuadPart)));
            unsigned long read = 0; // DWORD of ReadFile
            const bool    ok   = file != nullptr && ReadFile(handle, file, static_cast<std::uint32_t>(length.QuadPart), &read, nullptr);
            CloseHandle(handle);
            if (!ok || read != length.QuadPart)
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

    // The golden inputs come from the environment: CTLAME_TEST_CORPUS names the corpus file the
    // checksums were recorded with, CTLAME_TEST_SYNTH the folder of the synth files made from it
    // (New-SyntheticWavs.py). Without them the golden tests are skipped.
    inline const wchar_t* TestPath(const wchar_t* variable, wchar_t (&path)[MAX_PATH], bool folder) noexcept
    {
        const auto length = GetEnvironmentVariableW(variable, path, MAX_PATH - 1);
        if (length == 0 || length >= MAX_PATH - 1)
            path[0] = L'\0';
        else if (folder && path[length - 1] != L'\\') {
            path[length]     = L'\\';
            path[length + 1] = L'\0';
        }
        return path;
    }

    inline const wchar_t* CorpusFile()
    {
        static wchar_t path[MAX_PATH];
        return TestPath(L"CTLAME_TEST_CORPUS", path, false);
    }

    inline const wchar_t* SynthDir()
    {
        static wchar_t path[MAX_PATH];
        return TestPath(L"CTLAME_TEST_SYNTH", path, true);
    }

} // namespace ctLameTests
