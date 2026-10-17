//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ctDeflateTests {

    /// <summary>Deterministic generator (xorshift64*) so every run compresses the same data.</summary>
    class Rng
    {
    public:
        explicit Rng(std::uint64_t seed) noexcept : state_(seed * 0x9E3779B97F4A7C15ull + 1) {}

        std::uint64_t Next() noexcept
        {
            state_ ^= state_ >> 12;
            state_ ^= state_ << 25;
            state_ ^= state_ >> 27;
            return state_ * 0x2545F4914F6CDD1Dull;
        }

        std::uint32_t Below(std::uint32_t n) noexcept { return static_cast<std::uint32_t>(Next() % n); }

    private:
        std::uint64_t state_;
    };

    enum class DataKind { Text, Random, Zeros, Periodic, Dna, Mixed, Binary, Log };

    /// <summary>Test data of the given kind and size.</summary>
    inline std::vector<std::uint8_t> MakeData(DataKind kind, std::size_t size, std::uint64_t seed = 1)
    {
        std::vector<std::uint8_t> data(size);
        Rng                       rng(seed);
        static const char* const  Words[] = {"the ",   "deflate ", "stream ", "block ", "match ", "literal ", "Huffman ", "code ",
                                             "window ", "offset ",  "length ", "\n",     "int ",   "return ",  "if (",     "x = y;\n",
                                             "{\n",     "}\n",      "    ",    "// ",    "0x1F, ", "parallel ", "fiber ",  "task "};
        switch (kind) {
            case DataKind::Text: {
                std::size_t i = 0;
                while (i < size) {
                    const char* w = Words[rng.Below(sizeof(Words) / sizeof(Words[0]))];
                    while (*w != 0 && i < size)
                        data[i++] = static_cast<std::uint8_t>(*w++);
                    if (rng.Below(13) == 0 && i < size)
                        data[i++] = static_cast<std::uint8_t>('A' + rng.Below(26));
                }
                break;
            }
            case DataKind::Random:
                for (auto& b : data)
                    b = static_cast<std::uint8_t>(rng.Next());
                break;
            case DataKind::Zeros: break;
            case DataKind::Periodic:
                for (std::size_t i = 0; i < size; ++i)
                    data[i] = static_cast<std::uint8_t>((i % 7919) * 31 + (i % 13));
                break;
            case DataKind::Dna: {
                static const char Bases[] = "ACGT";
                for (auto& b : data)
                    b = static_cast<std::uint8_t>(Bases[rng.Below(4)]);
                // Repeats so that matches exist.
                for (std::size_t i = 1000; i + 200 < size; i += 1000 + rng.Below(500)) {
                    const std::size_t from = rng.Below(static_cast<std::uint32_t>(i - 200));
                    for (std::size_t k = 0; k < 150; ++k)
                        data[i + k] = data[from + k];
                }
                break;
            }
            case DataKind::Mixed: {
                // Alternating sections of text, random bytes, DNA and zeros of varying length.
                std::size_t i       = 0;
                std::uint32_t section = 0;
                while (i < size) {
                    const std::size_t len   = 3000 + rng.Below(60000);
                    const auto        part  = MakeData(static_cast<DataKind>(section % 5), len, seed + section);
                    for (std::size_t k = 0; k < len && i < size; ++k)
                        data[i++] = part[k];
                    ++section;
                }
                break;
            }
            case DataKind::Log: {
                // Log lines with few distinct characters: libdeflate's minimum match length changes between 4 and 5.
                static const char* const Levels[] = {"INFO ", "WARN ", "DEBUG", "ERROR"};
                std::size_t              i        = 0;
                while (i < size) {
                    char line[160];
                    const int n = std::snprintf(line, sizeof(line), "2026-03-%02u %02u:%02u:%02u.%03u %s [w%u] req %u ok %u ms\n", 1 + rng.Below(28),
                                                rng.Below(24), rng.Below(60), rng.Below(60), rng.Below(1000), Levels[rng.Below(4)], rng.Below(16),
                                                rng.Below(100000), rng.Below(500));
                    for (int k = 0; k < n && i < size; ++k)
                        data[i++] = static_cast<std::uint8_t>(line[k]);
                }
                break;
            }
            case DataKind::Binary:
                // Structured records: small integers, repeated keys, occasional noise.
                for (std::size_t i = 0; i + 16 <= size; i += 16) {
                    const std::uint32_t id = static_cast<std::uint32_t>(i / 16);
                    std::memcpy(&data[i], &id, 4);
                    const std::uint32_t v = rng.Below(1000);
                    std::memcpy(&data[i + 4], &v, 4);
                    std::memcpy(&data[i + 8], "KEY_", 4);
                    data[i + 12] = static_cast<std::uint8_t>(rng.Below(4));
                    data[i + 13] = static_cast<std::uint8_t>(rng.Next());
                }
                break;
        }
        return data;
    }

} // namespace ctDeflateTests
