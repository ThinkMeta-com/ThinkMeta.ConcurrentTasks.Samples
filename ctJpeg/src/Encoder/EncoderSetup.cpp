// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "EncoderSetup.h"

#include <cmath>
#include <cstdint>

#include "JpegliTables.h"

// Port of the parameter part of jpegli (encode.cc: jpegli_set_defaults, jpegli_set_colorspace,
// jpegli_set_distance, jpegli_quality_to_distance, SetDefaultScanScript, ProcessCompressionParams;
// quant.cc: SetQuantMatrices, InitQuantizer, QuantValsToDistance). The float expressions keep
// jpegli's operand types and order, including the float/double mix of std::pow.

// The implicit double -> float conversions are jpegli's and part of the bit-exact result.
#pragma warning(disable : 4244 4305)

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    namespace {

        constexpr float        GlobalScaleYCbCr = 1.73966010f; // kGlobalScaleYCbCr
        const float            GlobalScale420   = 1.22;        // k420GlobalScale
        constexpr float        Dist0            = 1.5f;        // kDist0
        constexpr std::int32_t QuantMaxBaseline = 255;         // cjpegli calls jpegli_set_distance with force_baseline = TRUE

        // std::min / std::max of jpegli, without <algorithm>: same operand order and tie behavior.
        float MinF(float a, float b) noexcept
        {
            return b < a ? b : a;
        }
        float MaxF(float a, float b) noexcept
        {
            return a < b ? b : a;
        }

        std::int32_t DivCeil(std::int32_t a, std::int32_t b) noexcept
        {
            return (a + b - 1) / b;
        }

        // jpegli: DistanceToScale() (quant.cc).
        float DistanceToScale(float distance, std::int32_t k) noexcept
        {
            if (distance < Dist0)
                return distance;
            const float exp = DistanceExponent[static_cast<std::size_t>(k)];
            const float mul = std::pow(Dist0, 1.0 - exp);
            return MaxF(0.5f * distance, mul * std::pow(distance, exp));
        }

        // jpegli: ScaleToDistance() (quant.cc).
        float ScaleToDistance(float scale, std::int32_t k) noexcept
        {
            if (scale < Dist0)
                return scale;
            const float exp = 1.0 / DistanceExponent[static_cast<std::size_t>(k)];
            const float mul = std::pow(Dist0, 1.0 - exp);
            return MinF(2.0f * scale, mul * std::pow(scale, exp));
        }

        // jpegli: QuantValsToDistance() (quant.cc); no PQ/HLG transfer function for PNM input.
        float QuantValsToDistance(const EncoderSetup& s) noexcept
        {
            const float        globalScale = GlobalScaleYCbCr;
            const std::int32_t quantMax    = QuantMaxBaseline;
            constexpr float    DistMax     = 10000.0f;
            float              distMin     = 0.0f;
            float              distMax     = DistMax;
            for (std::int32_t c = 0; c < s.frame.components; ++c) {
                const std::int32_t quantIdx = s.frame.component[static_cast<std::size_t>(c)].quantTable;
                const auto&        quantval = s.quant[static_cast<std::size_t>(quantIdx)].values;
                const float*       baseQm   = BaseQuantMatrixYCbCr.data() + quantIdx * DctSize2;
                for (std::int32_t k = 0; k < DctSize2; ++k) {
                    float              dmin = 0.0;
                    float              dmax = DistMax;
                    float              invq = 1.0f / baseQm[k] / globalScale;
                    const std::int32_t qval = quantval[static_cast<std::size_t>(k)];
                    if (qval > 1) {
                        float scaleMin = (qval - 0.5f) * invq;
                        dmin           = ScaleToDistance(scaleMin, k);
                    }
                    if (qval < quantMax) {
                        float scaleMax = (qval + 0.5f) * invq;
                        dmax           = ScaleToDistance(scaleMax, k);
                    }
                    if (dmin <= distMax)
                        distMin = MaxF(dmin, distMin);
                    if (dmax >= distMin)
                        distMax = MinF(distMax, dmax);
                }
            }
            if (distMin == 0)
                return distMax;
            if (distMax == DistMax)
                return distMin;
            return 0.5f * (distMin + distMax);
        }

        // jpegli: SetQuantMatrices() (quant.cc) with add_two_chroma_tables = true, YCbCr/grayscale, no std tables.
        void SetQuantMatrices(EncoderSetup& s, float distance, bool isYuv420) noexcept
        {
            float globalScale = GlobalScaleYCbCr;
            if (isYuv420)
                globalScale *= GlobalScale420;
            for (std::int32_t quantIdx = 0; quantIdx < 3; ++quantIdx) {
                const float* baseQm = BaseQuantMatrixYCbCr.data() + quantIdx * DctSize2;
                for (std::int32_t k = 0; k < DctSize2; ++k) {
                    float scale = globalScale;
                    scale *= DistanceToScale(distance, k);
                    if (isYuv420 && quantIdx > 0)
                        scale *= Rescale420[static_cast<std::size_t>(k)];
                    const std::int32_t qval = static_cast<std::int32_t>(std::round(scale * baseQm[k]));
                    s.quant[static_cast<std::size_t>(quantIdx)].values[static_cast<std::size_t>(k)]
                        = static_cast<std::uint16_t>(qval < 1 ? 1 : (qval > QuantMaxBaseline ? QuantMaxBaseline : qval));
                }
            }
        }

        // jpegli: InitQuantizer(NO_SEARCH) (quant.cc): multipliers and zero bias.
        void InitQuantizer(EncoderSetup& s) noexcept
        {
            for (std::int32_t c = 0; c < s.frame.components; ++c) {
                const auto& table = s.quant[static_cast<std::size_t>(s.frame.component[static_cast<std::size_t>(c)].quantTable)];
                for (std::int32_t k = 0; k < DctSize2; ++k)
                    s.quantMul[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] = 8.0f / table.values[static_cast<std::size_t>(k)];
            }
            if (s.adaptiveQuant) {
                for (std::int32_t c = 0; c < s.frame.components; ++c) {
                    for (std::int32_t k = 0; k < DctSize2; ++k) {
                        s.zeroBiasMul[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)]    = k == 0 ? 0.0f : 0.5f;
                        s.zeroBiasOffset[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] = k == 0 ? 0.0f : 0.5f;
                    }
                }
            }
            if (!s.ycbcr)
                return;
            static const float ZeroBiasOffsetAC[3] = {0.59082f, 0.58146f, 0.57988f}; // kZeroBiasOffsetYCbCrAC; DC is 0
            if (s.adaptiveQuant) {
                const float        distance = QuantValsToDistance(s);
                static const float DistHQ   = 1.0f; // kDistHQ
                static const float DistLQ   = 3.0f; // kDistLQ
                float              mix0     = (distance - DistHQ) / (DistLQ - DistHQ);
                mix0                        = MaxF(0.0f, MinF(1.0f, mix0));
                const float mix1            = 1.0f - mix0;
                for (std::int32_t c = 0; c < s.frame.components; ++c) {
                    for (std::int32_t k = 0; k < DctSize2; ++k) {
                        const float mul0 = ZeroBiasMulYCbCrLQ[static_cast<std::size_t>(c * DctSize2 + k)];
                        const float mul1 = ZeroBiasMulYCbCrHQ[static_cast<std::size_t>(c * DctSize2 + k)];
                        s.zeroBiasMul[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)]    = mix0 * mul0 + mix1 * mul1;
                        s.zeroBiasOffset[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] = k == 0 ? 0.0f : ZeroBiasOffsetAC[c];
                    }
                }
            } else {
                for (std::int32_t c = 0; c < s.frame.components; ++c)
                    for (std::int32_t k = 0; k < DctSize2; ++k)
                        s.zeroBiasOffset[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] = k == 0 ? 0.0f : ZeroBiasOffsetAC[c];
            }
        }

        // jpegli: SetDefaultScanScript() (level 2) and the AC context offsets of ProcessCompressionParams() (encode.cc).
        void SetScanScript(EncoderSetup& s) noexcept
        {
            struct ProgressiveScan {
                std::int32_t ss, se, ah, al;
                bool         interleaved;
            };
            const bool            interleaveDc = s.maxH == 1 && s.maxV == 1;
            const ProgressiveScan mode[] = {{0, 0, 0, 0, interleaveDc}, {1, 2, 0, 0, false}, {3, 63, 0, 2, false}, {3, 63, 2, 1, false}, {3, 63, 1, 0, false}};
            s.scanCount                  = 0;
            for (const ProgressiveScan& scan : mode) {
                const std::int32_t comps = scan.interleaved ? MaxComponents : 1;
                for (std::int32_t c = 0; c < s.frame.components; c += comps) {
                    ScanInfo& info      = s.scans[static_cast<std::size_t>(s.scanCount++)];
                    info.ss             = scan.ss;
                    info.se             = scan.se;
                    info.ah             = scan.ah;
                    info.al             = scan.al;
                    info.componentCount = comps < s.frame.components - c ? comps : s.frame.components - c;
                    for (std::int32_t j = 0; j < info.componentCount; ++j)
                        info.components[static_cast<std::size_t>(j)] = c + j;
                }
            }
            std::int32_t acContexts = 0;
            for (std::int32_t i = 0; i < s.scanCount; ++i) {
                s.acContextOffset[static_cast<std::size_t>(i)] = 4 + acContexts;
                if (s.scans[static_cast<std::size_t>(i)].se > 0)
                    acContexts += s.scans[static_cast<std::size_t>(i)].componentCount;
            }
            s.contextCount = 4 + acContexts;
        }

    } // namespace

    // jpegli: jpegli_quality_to_distance() (encode.cc).
    float QualityToDistance(std::int32_t quality) noexcept
    {
        return (quality >= 100      ? 0.01f
                    : quality >= 30 ? 0.1f + (100 - quality) * 0.09f
                                    : 53.0f / 3000.0f * quality * quality - 23.0f / 20.0f * quality + 25.0f);
    }

    // jpegli: jpegli_set_defaults(), jpegli_set_colorspace(), jpegli_set_distance() (encode.cc), ProcessCompressionParams()
    // (encode.cc) and InitQuantizer() (quant.cc), as cjpegli calls them.
    bool SetupEncoder(std::int32_t width, std::int32_t height, std::int32_t components, const EncoderOptions& options, EncoderSetup& s) noexcept
    {
        if (width < 1 || height < 1 || width > 65500 || height > 65500 || (components != 1 && components != 3))
            return false;
        s                   = EncoderSetup{};
        s.ycbcr             = components == 3;
        s.frame.width       = width;
        s.frame.height      = height;
        s.frame.components  = components;
        s.frame.progressive = true;

        // jpegli_set_colorspace: ids 1..n, Y on table 0, chroma on 1; then jpegli_set_distance with
        // add_two_chroma_tables moves Cr to table 2. cjpegli resets the default 2x2 luma sampling to
        // 1x1 unless --chroma_subsampling 420 asks for it; one component is always 1x1.
        for (std::int32_t c = 0; c < components; ++c) {
            ComponentInfo& comp = s.frame.component[static_cast<std::size_t>(c)];
            comp.id             = static_cast<std::uint8_t>(c + 1);
            comp.hSamp = comp.vSamp = 1;
            comp.quantTable         = c;
        }
        if (s.ycbcr && options.subsampling == ChromaSubsampling::Yuv420) {
            s.frame.component[0].hSamp = 2;
            s.frame.component[0].vSamp = 2;
        }
        const bool isYuv420 = s.ycbcr && s.frame.component[0].hSamp == 2 && s.frame.component[0].vSamp == 2;

        s.distance = options.useQuality ? QualityToDistance(options.quality) : options.distance;
        SetQuantMatrices(s, s.distance, isYuv420);

        // ProcessCompressionParams: layout in blocks
        s.maxH = s.maxV = 1;
        for (std::int32_t c = 0; c < components; ++c) {
            const ComponentInfo& comp = s.frame.component[static_cast<std::size_t>(c)];
            s.maxH                    = comp.hSamp > s.maxH ? comp.hSamp : s.maxH;
            s.maxV                    = comp.vSamp > s.maxV ? comp.vSamp : s.maxV;
        }
        s.iMcuCols    = DivCeil(width, DctSize * s.maxH);
        s.iMcuRows    = DivCeil(height, DctSize * s.maxV);
        s.xsizeBlocks = s.iMcuCols * s.maxH;
        s.ysizeBlocks = s.iMcuRows * s.maxV;
        for (std::int32_t c = 0; c < components; ++c) {
            const ComponentInfo& comp                     = s.frame.component[static_cast<std::size_t>(c)];
            s.hFactor[static_cast<std::size_t>(c)]        = s.maxH / comp.hSamp;
            s.vFactor[static_cast<std::size_t>(c)]        = s.maxV / comp.vSamp;
            const std::int32_t downsampledWidth           = DivCeil(width, s.hFactor[static_cast<std::size_t>(c)]);
            const std::int32_t downsampledHeight          = DivCeil(height, s.vFactor[static_cast<std::size_t>(c)]);
            s.widthInBlocks[static_cast<std::size_t>(c)]  = DivCeil(downsampledWidth, DctSize);
            s.heightInBlocks[static_cast<std::size_t>(c)] = DivCeil(downsampledHeight, DctSize);
        }
        // adaptive quantization is off for a subsampled luma channel
        s.adaptiveQuant = s.frame.component[0].hSamp == s.maxH && s.frame.component[0].vSamp == s.maxV;
        s.yQuant01      = s.quant[static_cast<std::size_t>(s.frame.component[0].quantTable)].values[1];

        SetScanScript(s);
        InitQuantizer(s);
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
