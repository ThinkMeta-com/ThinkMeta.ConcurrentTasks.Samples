// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

// New in ctJpeg, not in jpegli: the band split of the tokenization. A scan is tokenized in bands
// of rows in parallel (jpegli's tokenizers of Tokenize.cpp, run on a band), and a serial stitch
// joins the bands to exactly the tokens jpegli makes for the whole scan. The stitch of a
// refinement scan replays the EOB state machine of jpegli's TokenizeACRefinementScan, hence the
// notice above.

#include "Entropy.h"

#include <cstddef>
#include <cstdint>

#include "Tokenize.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    bool CanSplitScan(const EncoderSetup& setup, std::int32_t scanIndex) noexcept
    {
        const ScanInfo& si = setup.scans[static_cast<std::size_t>(scanIndex)];
        return si.ss > 0 && si.componentCount == 1;
    }

    std::int32_t DcScanMcuRows(const EncoderSetup& setup, std::int32_t scanIndex) noexcept
    {
        std::int32_t mcusPerRow, mcuRows;
        DcScanMcus(setup, setup.scans[static_cast<std::size_t>(scanIndex)], mcusPerRow, mcuRows);
        return mcuRows;
    }

    bool TokenizeDcBand(const EncoderSetup& setup, const Coefficients& coefficients, std::int32_t scanIndex, std::int32_t mcuRowBegin, std::int32_t mcuRowEnd,
                        TokenPart& part, Histograms& histograms) noexcept
    {
        const ScanInfo& si = setup.scans[static_cast<std::size_t>(scanIndex)];
        if (si.ah != 0 || !TokenizeDcRows(setup, coefficients, si, mcuRowBegin, mcuRowEnd, part))
            return false;
        for (std::size_t j = 0; j < part.tokenCount; ++j) {
            const Token t = part.tokens[j];
            ++histograms.count[t.context][t.symbol];
        }
        return true;
    }

    std::int32_t ScanBlockRows(const EncoderSetup& setup, std::int32_t scanIndex) noexcept
    {
        return setup.heightInBlocks[static_cast<std::size_t>(setup.scans[static_cast<std::size_t>(scanIndex)].components[0])];
    }

    bool TokenizeScanBand(const EncoderSetup& setup, const Coefficients& coefficients, std::int32_t scanIndex, std::int32_t rowBegin, std::int32_t rowEnd,
                          TokenPart& part, BandResult& result) noexcept
    {
        const ScanInfo& si = setup.scans[static_cast<std::size_t>(scanIndex)];
        if (si.ah == 0)
            return TokenizeAcFirstRows(setup, coefficients, si, setup.acContextOffset[static_cast<std::size_t>(scanIndex)], rowBegin, rowEnd, part, &result);
        return TokenizeAcRefinementRows(setup, coefficients, si, rowBegin, rowEnd, part, &result);
    }

    bool StitchScanBands(const EncoderSetup& setup, std::int32_t scanIndex, std::int32_t bands, ScanTokens& tokens, const BandResult* results,
                         Histograms& histograms) noexcept
    {
        const ScanInfo&    si      = setup.scans[static_cast<std::size_t>(scanIndex)];
        const std::int32_t context = setup.acContextOffset[static_cast<std::size_t>(scanIndex)];
        auto&              histo   = histograms.count[static_cast<std::size_t>(context)];
        TokenPart&         final   = tokens.parts[2 * bands];
        if (si.ah == 0) {
            // AC first scan: leading zero blocks extend the run (forced out at MaxEobRun), the
            // first active block flushes it, then the band's own run continues.
            std::int32_t eobRun = 0;
            for (std::int32_t b = 0; b < bands; ++b) {
                const BandResult& r      = results[b];
                TokenPart&        prefix = tokens.parts[2 * b];
                if (!prefix.tokens.Allocate(static_cast<std::size_t>(r.leadingBlocks) / MaxEobRun + 2))
                    return false;
                const std::int64_t total = static_cast<std::int64_t>(eobRun) + r.leadingBlocks;
                for (std::int64_t e = total / MaxEobRun; e > 0; --e)
                    prefix.tokens[prefix.tokenCount++] = EobRunToken(context, MaxEobRun);
                eobRun = static_cast<std::int32_t>(total % MaxEobRun);
                if (r.active) {
                    if (eobRun > 0)
                        prefix.tokens[prefix.tokenCount++] = EobRunToken(context, eobRun);
                    eobRun = r.exitEobRun;
                    for (std::size_t sym = 0; sym < HuffmanSymbols; ++sym)
                        histo[sym] += r.histogram[sym];
                }
                for (std::size_t i = 0; i < prefix.tokenCount; ++i)
                    ++histo[prefix.tokens[i].symbol];
            }
            if (!final.tokens.Allocate(1))
                return false;
            if (eobRun > 0) {
                final.tokens[final.tokenCount++] = EobRunToken(context, eobRun);
                ++histo[final.tokens[0].symbol];
            }
            return true;
        }

        // AC refinement scan: replay jpegli's EOB state machine (TokenizeACRefinementScan) for the
        // leading EOB blocks of every band; it may extend the open token of the preceding band,
        // split it after MaxRefBits correction bits, or start new tokens in the band's prefix part.
        std::int32_t   eobRun     = 0;
        std::int32_t   eobRefbits = 0;
        RefToken*      open       = nullptr; // the token of the open run
        std::uint16_t* lastRun    = nullptr; // its extra bits, once the run is 2 or longer
        auto           closeOpen  = [&]() noexcept {
            if (open != nullptr)
                ++histo[open->symbol & RefSymbolMask];
            open = nullptr;
        };
        for (std::int32_t b = 0; b < bands; ++b) {
            const BandResult& r      = results[b];
            TokenPart&        prefix = tokens.parts[2 * b];
            TokenPart&        own    = tokens.parts[2 * b + 1];
            if (!prefix.refTokens.Allocate(static_cast<std::size_t>(r.leadingBlocks) + 1)
                || !prefix.eobRuns.Allocate(static_cast<std::size_t>(r.leadingBlocks) / 2 + 2))
                return false;
            for (std::int32_t i = 0; i < r.leadingBlocks; ++i) {
                const std::int32_t bits = r.leadingBits[static_cast<std::size_t>(i)];
                ++eobRun;
                eobRefbits += bits;
                if (eobRefbits > MaxRefBits) {
                    closeOpen();
                    eobRefbits = bits;
                    eobRun     = 1;
                }
                if (eobRun == 1)
                    open = &prefix.refTokens[prefix.refTokenCount++];
                open->refbits = static_cast<std::uint8_t>(eobRefbits);
                if (eobRun == 1)
                    open->symbol = 0;
                else if (eobRun == 2) {
                    open->symbol = 16;
                    lastRun      = &prefix.eobRuns[prefix.eobRunCount++];
                    *lastRun     = 0;
                } else if ((eobRun & (eobRun - 1)) == 0) {
                    open->symbol = static_cast<std::uint8_t>(open->symbol + 16);
                    *lastRun     = 0;
                } else
                    ++*lastRun;
                if (eobRun == MaxEobRun) {
                    closeOpen();
                    eobRun = eobRefbits = 0;
                }
            }
            if (r.active) {
                // the first newly significant coefficient ends the open run for good
                closeOpen();
                eobRun     = r.exitEobRun;
                eobRefbits = r.exitEobRefbits;
                if (eobRun > 0)
                    open = &own.refTokens[own.refTokenCount - 1];
                if (eobRun > 1)
                    lastRun = &own.eobRuns[own.eobRunCount - 1];
                for (std::size_t sym = 0; sym < HuffmanSymbols; ++sym)
                    histo[sym] += r.histogram[sym];
            }
        }
        closeOpen();
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
