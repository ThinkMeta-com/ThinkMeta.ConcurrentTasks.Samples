// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstdint>

#include "Coefficients.h"
#include "EncoderSetup.h"
#include "Entropy.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    // The row tokenizers of Tokenize.cpp, shared with the band split (BandStitch.cpp); internal to
    // the entropy coder. Integer arithmetic keeps jpegli's coeff_t (int16) types.

    /// <summary>floor(log2(v)); -1 for 0.</summary>
    inline std::int32_t FloorLog2(std::uint32_t v) noexcept
    {
        std::int32_t n = -1;
        while (v != 0) {
            v >>= 1;
            ++n;
        }
        return n;
    }

    /// <summary>The EOB-run token of an AC first scan. jpegli: emit_eob_run of TokenizeACProgressiveScan() (entropy_coding.cc).</summary>
    inline Token EobRunToken(std::int32_t context, std::int32_t eobRun) noexcept
    {
        const std::int32_t nbits = FloorLog2(static_cast<std::uint32_t>(eobRun));
        return {static_cast<std::uint8_t>(context), static_cast<std::uint8_t>(nbits << 4), static_cast<std::uint16_t>(eobRun & ((1 << nbits) - 1))};
    }

    /// <summary>MCUs per row and MCU rows of a DC scan.</summary>
    void DcScanMcus(const EncoderSetup& s, const ScanInfo& si, std::int32_t& mcusPerRow, std::int32_t& mcuRows) noexcept;

    /// <summary>
    /// TokenizeProgressiveDC over the MCU rows [m0, m1). The DC predictor of each component starts
    /// with the last block of that component before MCU row m0 (0 at the scan start), so the bands
    /// of a DC scan need no stitch. False if out of memory.
    /// jpegli: TokenizeProgressiveDC() and the DC loop of TokenizeScan() (entropy_coding.cc).
    /// </summary>
    bool TokenizeDcRows(const EncoderSetup& s, const Coefficients& coef, const ScanInfo& si, std::int32_t m0, std::int32_t m1, TokenPart& out) noexcept;

    /// <summary>
    /// TokenizeACProgressiveScan over the block rows [r0, r1). As a whole scan (result null) it is
    /// jpegli's loop. As a band, the leading zero blocks are only counted and the pending run at
    /// the first active block and at the end is left to the stitch. False if out of memory.
    /// jpegli: TokenizeACProgressiveScan() (entropy_coding.cc).
    /// </summary>
    bool TokenizeAcFirstRows(const EncoderSetup& s, const Coefficients& coef, const ScanInfo& si, std::int32_t context, std::int32_t r0, std::int32_t r1,
                             TokenPart& out, BandResult* result) noexcept;

    /// <summary>
    /// TokenizeACRefinementScan over the block rows [r0, r1). As a band, the blocks before the
    /// first newly significant coefficient only record their correction bits (they are EOB blocks
    /// that extend the open run of the preceding band); from that block on the band runs jpegli's
    /// loop with the state of a fresh scan. False if out of memory.
    /// jpegli: TokenizeACRefinementScan() (entropy_coding.cc).
    /// </summary>
    bool TokenizeAcRefinementRows(const EncoderSetup& s, const Coefficients& coef, const ScanInfo& si, std::int32_t r0, std::int32_t r1, TokenPart& out,
                                  BandResult* result) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
