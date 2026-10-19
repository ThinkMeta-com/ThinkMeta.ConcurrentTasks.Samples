// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "EncoderSetup.h"
#include "JpegTypes.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    /// <summary>
    /// The quantized coefficients of the whole image (jpegli's coeff_buffers): per component
    /// widthInBlocks x heightInBlocks blocks of 64 values in zigzag order. A copy of the DC values
    /// in a dense plane per component lets the DC scan read 2 bytes per block instead of a cache
    /// line per block.
    /// </summary>
    class Coefficients
    {
    public:
        Coefficients() noexcept                      = default;
        Coefficients(const Coefficients&)            = delete;
        Coefficients& operator=(const Coefficients&) = delete;
        ~Coefficients() { Reset(); }

        /// <summary>Allocates the planes for setup; false if out of memory.</summary>
        bool Allocate(const EncoderSetup& setup) noexcept;

        void Reset() noexcept;

        /// <summary>Frees the planes of component c (the others stay); for freeing in parallel.</summary>
        void ReleaseComponent(std::int32_t c) noexcept;

        std::int32_t Components() const noexcept { return components_; }
        std::int32_t WidthInBlocks(std::int32_t c) const noexcept { return width_[static_cast<std::size_t>(c)]; }
        std::int32_t HeightInBlocks(std::int32_t c) const noexcept { return height_[static_cast<std::size_t>(c)]; }

        /// <summary>The 64 coefficients (zigzag order) of block (bx, by) of component c.</summary>
        std::int16_t* Block(std::int32_t c, std::int32_t bx, std::int32_t by) noexcept
        {
            return planes_[static_cast<std::size_t>(c)]
                + (static_cast<std::size_t>(by) * static_cast<std::size_t>(width_[static_cast<std::size_t>(c)]) + static_cast<std::size_t>(bx)) * DctSize2;
        }
        const std::int16_t* Block(std::int32_t c, std::int32_t bx, std::int32_t by) const noexcept
        {
            return planes_[static_cast<std::size_t>(c)]
                + (static_cast<std::size_t>(by) * static_cast<std::size_t>(width_[static_cast<std::size_t>(c)]) + static_cast<std::size_t>(bx)) * DctSize2;
        }

        /// <summary>The DC value of block (bx, by) of component c (the copy in the dense plane).</summary>
        std::int16_t& Dc(std::int32_t c, std::int32_t bx, std::int32_t by) noexcept
        {
            return dc_[static_cast<std::size_t>(c)]
                      [static_cast<std::size_t>(by) * static_cast<std::size_t>(width_[static_cast<std::size_t>(c)]) + static_cast<std::size_t>(bx)];
        }
        std::int16_t Dc(std::int32_t c, std::int32_t bx, std::int32_t by) const noexcept
        {
            return dc_[static_cast<std::size_t>(c)]
                      [static_cast<std::size_t>(by) * static_cast<std::size_t>(width_[static_cast<std::size_t>(c)]) + static_cast<std::size_t>(bx)];
        }

    private:
        std::array<std::int16_t*, MaxComponents> planes_{};
        std::array<std::int16_t*, MaxComponents> dc_{};
        std::array<std::int32_t, MaxComponents>  width_{}, height_{};
        std::int32_t                             components_ = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
