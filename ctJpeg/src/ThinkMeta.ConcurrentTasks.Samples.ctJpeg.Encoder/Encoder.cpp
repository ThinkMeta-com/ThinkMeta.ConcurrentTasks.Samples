// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "Encoder.h"

#include <cstdint>
#include <memory>
#include <new>

#include "Coefficients.h"
#include "EncoderSetup.h"
#include "Entropy.h"
#include "Markers.h"
#include "PixelPhase.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    EncodeStatus PrepareEncode(const Image& image, const EncoderOptions& options, EncoderSetup& setup, Coefficients& coefficients) noexcept
    {
        if (!image || !SetupEncoder(image.Width(), image.Height(), image.Components(), options, setup))
            return EncodeStatus::InvalidInput;
        if (!coefficients.Allocate(setup))
            return EncodeStatus::OutOfMemory;
        return EncodeStatus::Ok;
    }

    EncodeStatus ComputeAllCoefficients(const Image& image, const EncoderOptions& options, SimdLevel simd, EncoderSetup& setup, Coefficients& coefficients) noexcept
    {
        const EncodeStatus status = PrepareEncode(image, options, setup, coefficients);
        if (status != EncodeStatus::Ok)
            return status;
        PixelScratch scratch;
        for (std::int32_t r = 0; r < setup.iMcuRows; r += DefaultBandRows) {
            const std::int32_t end = r + DefaultBandRows < setup.iMcuRows ? r + DefaultBandRows : setup.iMcuRows;
            if (!ComputeCoefficients(setup, image, simd, r, end, coefficients, scratch))
                return EncodeStatus::OutOfMemory;
        }
        return EncodeStatus::Ok;
    }

    EncodeStatus EncodeFromCoefficients(const EncoderSetup& setup, const Coefficients& coefficients, ByteBuffer& output) noexcept
    {
        // TokenizeJpeg and OptimizeHuffmanCodes: every scan reads only the coefficients.
        const auto scanTokens = std::unique_ptr<ScanTokens[]>(new (std::nothrow) ScanTokens[MaxScans]);
        const auto histograms = std::unique_ptr<Histograms>(new (std::nothrow) Histograms());
        const auto coding     = std::unique_ptr<HuffmanCoding>(new (std::nothrow) HuffmanCoding());
        if (!scanTokens || !histograms || !coding)
            return EncodeStatus::OutOfMemory;
        for (std::int32_t i = 0; i < setup.scanCount; ++i) {
            if (!TokenizeScan(setup, coefficients, i, scanTokens[i]))
                return EncodeStatus::OutOfMemory;
            AddScanHistograms(setup, i, scanTokens[i], *histograms);
        }
        OptimizeHuffmanCodes(setup, *histograms, *coding);

        // jpegli_finish_compress: SOI (InitCompress), DQT and SOF (WriteFrameHeader), per scan
        // DHT/SOS and data, EOI. cjpegli writes no JFIF/Adobe marker and no ICC profile for PNM.
        bool         baseline     = false;
        bool         ok           = WriteSoi(output) && WriteDqt(output, setup.frame, setup.quant.data(), baseline) && WriteSof(output, setup.frame, false);
        std::int32_t nextDhtIndex = 0;
        for (std::int32_t i = 0; ok && i < setup.scanCount; ++i)
            ok = WriteScanHeader(setup, i, *coding, nextDhtIndex, output) && WriteScanData(setup, i, scanTokens[i], *coding, output);
        if (!ok || !WriteEoi(output))
            return EncodeStatus::OutOfMemory;
        return EncodeStatus::Ok;
    }

    EncodeStatus EncodeJpeg(const Image& image, const EncoderOptions& options, SimdLevel simd, ByteBuffer& output) noexcept
    {
        EncoderSetup       setup;
        Coefficients       coefficients;
        const EncodeStatus status = ComputeAllCoefficients(image, options, simd, setup, coefficients);
        if (status != EncodeStatus::Ok)
            return status;
        return EncodeFromCoefficients(setup, coefficients, output);
    }

    const wchar_t* EncodeStatusText(EncodeStatus status) noexcept
    {
        switch (status) {
            case EncodeStatus::Ok: return L"ok";
            case EncodeStatus::NotImplemented: return L"not implemented yet";
            case EncodeStatus::OutOfMemory: return L"out of memory";
            default: return L"invalid input image";
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
