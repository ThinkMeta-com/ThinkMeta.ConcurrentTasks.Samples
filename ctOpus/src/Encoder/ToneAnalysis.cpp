// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "ToneAnalysis.h"

#include <cmath>
#include <cstring>

#include "CeltMath.h"
#include "IntMath.h"
#include "KissFft.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

#include "ToneAnalysisMlp.inc"

        // analysis.c does not see the M_PI of math.h (no _USE_MATH_DEFINES) and uses its own
        constexpr double Pi = 3.141592653;

        constexpr std::int32_t TransitionPenalty = 10;
        constexpr std::int32_t NbTonalSkipBands  = 9;
        constexpr float        LeakageOffset     = 2.5f;
        constexpr float        LeakageSlope      = 2.f;
        constexpr float        WeightsScale      = 1.f / 128;
        constexpr float        CeltSigScale      = 32768.f;

        constexpr float DctTable[128] = {
            0.250000f,  0.250000f,  0.250000f,  0.250000f,  0.250000f,  0.250000f,  0.250000f,  0.250000f,  0.250000f,  0.250000f,  0.250000f,  0.250000f,
            0.250000f,  0.250000f,  0.250000f,  0.250000f,  0.351851f,  0.338330f,  0.311806f,  0.273300f,  0.224292f,  0.166664f,  0.102631f,  0.034654f,
            -0.034654f, -0.102631f, -0.166664f, -0.224292f, -0.273300f, -0.311806f, -0.338330f, -0.351851f, 0.346760f,  0.293969f,  0.196424f,  0.068975f,
            -0.068975f, -0.196424f, -0.293969f, -0.346760f, -0.346760f, -0.293969f, -0.196424f, -0.068975f, 0.068975f,  0.196424f,  0.293969f,  0.346760f,
            0.338330f,  0.224292f,  0.034654f,  -0.166664f, -0.311806f, -0.351851f, -0.273300f, -0.102631f, 0.102631f,  0.273300f,  0.351851f,  0.311806f,
            0.166664f,  -0.034654f, -0.224292f, -0.338330f, 0.326641f,  0.135299f,  -0.135299f, -0.326641f, -0.326641f, -0.135299f, 0.135299f,  0.326641f,
            0.326641f,  0.135299f,  -0.135299f, -0.326641f, -0.326641f, -0.135299f, 0.135299f,  0.326641f,  0.311806f,  0.034654f,  -0.273300f, -0.338330f,
            -0.102631f, 0.224292f,  0.351851f,  0.166664f,  -0.166664f, -0.351851f, -0.224292f, 0.102631f,  0.338330f,  0.273300f,  -0.034654f, -0.311806f,
            0.293969f,  -0.068975f, -0.346760f, -0.196424f, 0.196424f,  0.346760f,  0.068975f,  -0.293969f, -0.293969f, 0.068975f,  0.346760f,  0.196424f,
            -0.196424f, -0.346760f, -0.068975f, 0.293969f,  0.273300f,  -0.166664f, -0.338330f, 0.034654f,  0.351851f,  0.102631f,  -0.311806f, -0.224292f,
            0.224292f,  0.311806f,  -0.102631f, -0.351851f, -0.034654f, 0.338330f,  0.166664f,  -0.273300f,
        };

        constexpr float AnalysisWindow[240] = {
            0.000043f, 0.000171f, 0.000385f, 0.000685f, 0.001071f, 0.001541f, 0.002098f, 0.002739f, 0.003466f, 0.004278f, 0.005174f, 0.006156f, 0.007222f,
            0.008373f, 0.009607f, 0.010926f, 0.012329f, 0.013815f, 0.015385f, 0.017037f, 0.018772f, 0.020590f, 0.022490f, 0.024472f, 0.026535f, 0.028679f,
            0.030904f, 0.033210f, 0.035595f, 0.038060f, 0.040604f, 0.043227f, 0.045928f, 0.048707f, 0.051564f, 0.054497f, 0.057506f, 0.060591f, 0.063752f,
            0.066987f, 0.070297f, 0.073680f, 0.077136f, 0.080665f, 0.084265f, 0.087937f, 0.091679f, 0.095492f, 0.099373f, 0.103323f, 0.107342f, 0.111427f,
            0.115579f, 0.119797f, 0.124080f, 0.128428f, 0.132839f, 0.137313f, 0.141849f, 0.146447f, 0.151105f, 0.155823f, 0.160600f, 0.165435f, 0.170327f,
            0.175276f, 0.180280f, 0.185340f, 0.190453f, 0.195619f, 0.200838f, 0.206107f, 0.211427f, 0.216797f, 0.222215f, 0.227680f, 0.233193f, 0.238751f,
            0.244353f, 0.250000f, 0.255689f, 0.261421f, 0.267193f, 0.273005f, 0.278856f, 0.284744f, 0.290670f, 0.296632f, 0.302628f, 0.308658f, 0.314721f,
            0.320816f, 0.326941f, 0.333097f, 0.339280f, 0.345492f, 0.351729f, 0.357992f, 0.364280f, 0.370590f, 0.376923f, 0.383277f, 0.389651f, 0.396044f,
            0.402455f, 0.408882f, 0.415325f, 0.421783f, 0.428254f, 0.434737f, 0.441231f, 0.447736f, 0.454249f, 0.460770f, 0.467298f, 0.473832f, 0.480370f,
            0.486912f, 0.493455f, 0.500000f, 0.506545f, 0.513088f, 0.519630f, 0.526168f, 0.532702f, 0.539230f, 0.545751f, 0.552264f, 0.558769f, 0.565263f,
            0.571746f, 0.578217f, 0.584675f, 0.591118f, 0.597545f, 0.603956f, 0.610349f, 0.616723f, 0.623077f, 0.629410f, 0.635720f, 0.642008f, 0.648271f,
            0.654508f, 0.660720f, 0.666903f, 0.673059f, 0.679184f, 0.685279f, 0.691342f, 0.697372f, 0.703368f, 0.709330f, 0.715256f, 0.721144f, 0.726995f,
            0.732807f, 0.738579f, 0.744311f, 0.750000f, 0.755647f, 0.761249f, 0.766807f, 0.772320f, 0.777785f, 0.783203f, 0.788573f, 0.793893f, 0.799162f,
            0.804381f, 0.809547f, 0.814660f, 0.819720f, 0.824724f, 0.829673f, 0.834565f, 0.839400f, 0.844177f, 0.848895f, 0.853553f, 0.858151f, 0.862687f,
            0.867161f, 0.871572f, 0.875920f, 0.880203f, 0.884421f, 0.888573f, 0.892658f, 0.896677f, 0.900627f, 0.904508f, 0.908321f, 0.912063f, 0.915735f,
            0.919335f, 0.922864f, 0.926320f, 0.929703f, 0.933013f, 0.936248f, 0.939409f, 0.942494f, 0.945503f, 0.948436f, 0.951293f, 0.954072f, 0.956773f,
            0.959396f, 0.961940f, 0.964405f, 0.966790f, 0.969096f, 0.971321f, 0.973465f, 0.975528f, 0.977510f, 0.979410f, 0.981228f, 0.982963f, 0.984615f,
            0.986185f, 0.987671f, 0.989074f, 0.990393f, 0.991627f, 0.992778f, 0.993844f, 0.994826f, 0.995722f, 0.996534f, 0.997261f, 0.997902f, 0.998459f,
            0.998929f, 0.999315f, 0.999615f, 0.999829f, 0.999957f, 1.000000f,
        };

        constexpr std::int32_t TBands[18 + 1] = {4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 136, 160, 192, 240};

        constexpr float StdFeatureBias[9] = {5.684947f, 3.475288f, 1.770634f, 1.599784f, 3.773215f, 2.163313f, 1.260756f, 1.116868f, 1.918795f};

        // the double-precision libm calls of the C code (a float argument is promoted)
        inline float LogD(float x) noexcept
        {
            return static_cast<float>(std::log(static_cast<double>(x)));
        }
        inline float SqrtD(double x) noexcept
        {
            return static_cast<float>(std::sqrt(x));
        }

        // --- mlp.c ----------------------------------------------------------------------------

        struct DenseLayer {
            const std::int8_t* bias;
            const std::int8_t* inputWeights;
            std::int32_t       nbInputs;
            std::int32_t       nbNeurons;
            bool               sigmoid;
        };

        struct GruLayer {
            const std::int8_t* bias;
            const std::int8_t* inputWeights;
            const std::int8_t* recurrentWeights;
            std::int32_t       nbInputs;
            std::int32_t       nbNeurons;
        };

        constexpr DenseLayer Layer0 = {Layer0Bias, Layer0Weights, 25, 32, false};
        constexpr GruLayer   Layer1 = {Layer1Bias, Layer1Weights, Layer1RecurWeights, 32, 24};
        constexpr DenseLayer Layer2 = {Layer2Bias, Layer2Weights, 24, 2, true};

        // libopus: tansig_approx() (src/mlp.c)
        inline float TansigApprox(float x) noexcept
        {
            constexpr float n0  = 952.52801514f;
            constexpr float n1  = 96.39235687f;
            constexpr float n2  = 0.60863042f;
            constexpr float d0  = 952.72399902f;
            constexpr float d1  = 413.36801147f;
            constexpr float d2  = 11.88600922f;
            const float     x2  = x * x;
            float           num = (n2 * x2 + n1) * x2 + n0;
            const float     den = (d2 * x2 + d1) * x2 + d0;
            num                 = num * x / den;
            return Max32(-1.f, Min32(1.f, num));
        }

        // libopus: sigmoid_approx() (src/mlp.c)
        inline float SigmoidApprox(float x) noexcept
        {
            return .5f + .5f * TansigApprox(.5f * x);
        }

        // libopus: gemm_accum() (src/mlp.c)
        void GemmAccum(float* out, const std::int8_t* weights, std::int32_t rows, std::int32_t cols, std::int32_t colStride, const float* x) noexcept
        {
            for (std::int32_t i = 0; i < rows; ++i) {
                for (std::int32_t j = 0; j < cols; ++j)
                    out[i] += weights[j * colStride + i] * x[j];
            }
        }

        // libopus: analysis_compute_dense() (src/mlp.c)
        void ComputeDense(const DenseLayer& layer, float* output, const float* input) noexcept
        {
            const std::int32_t n = layer.nbNeurons;
            for (std::int32_t i = 0; i < n; ++i)
                output[i] = layer.bias[i];
            GemmAccum(output, layer.inputWeights, n, layer.nbInputs, n, input);
            for (std::int32_t i = 0; i < n; ++i)
                output[i] *= WeightsScale;
            for (std::int32_t i = 0; i < n; ++i)
                output[i] = layer.sigmoid ? SigmoidApprox(output[i]) : TansigApprox(output[i]);
        }

        // libopus: analysis_compute_gru() (src/mlp.c)
        void ComputeGru(const GruLayer& gru, float* state, const float* input) noexcept
        {
            constexpr std::int32_t maxNeurons = 32;
            float                  tmp[maxNeurons];
            float                  z[maxNeurons];
            float                  r[maxNeurons];
            float                  h[maxNeurons];
            const std::int32_t     m      = gru.nbInputs;
            const std::int32_t     n      = gru.nbNeurons;
            const std::int32_t     stride = 3 * n;
            // update gate
            for (std::int32_t i = 0; i < n; ++i)
                z[i] = gru.bias[i];
            GemmAccum(z, gru.inputWeights, n, m, stride, input);
            GemmAccum(z, gru.recurrentWeights, n, n, stride, state);
            for (std::int32_t i = 0; i < n; ++i)
                z[i] = SigmoidApprox(WeightsScale * z[i]);
            // reset gate
            for (std::int32_t i = 0; i < n; ++i)
                r[i] = gru.bias[n + i];
            GemmAccum(r, &gru.inputWeights[n], n, m, stride, input);
            GemmAccum(r, &gru.recurrentWeights[n], n, n, stride, state);
            for (std::int32_t i = 0; i < n; ++i)
                r[i] = SigmoidApprox(WeightsScale * r[i]);
            // output
            for (std::int32_t i = 0; i < n; ++i)
                h[i] = gru.bias[2 * n + i];
            for (std::int32_t i = 0; i < n; ++i)
                tmp[i] = state[i] * r[i];
            GemmAccum(h, &gru.inputWeights[2 * n], n, m, stride, input);
            GemmAccum(h, &gru.recurrentWeights[2 * n], n, n, stride, tmp);
            for (std::int32_t i = 0; i < n; ++i)
                h[i] = z[i] * state[i] + (1 - z[i]) * TansigApprox(WeightsScale * h[i]);
            for (std::int32_t i = 0; i < n; ++i)
                state[i] = h[i];
        }

        // --- analysis.c helpers ---------------------------------------------------------------

        /// <summary>silk_resampler_down2_hp (float): halves the rate and returns the high-band energy.</summary>
        float ResamplerDown2Hp(float* s, float* out, const float* in, std::int32_t inLen) noexcept
        {
            const std::int32_t len2   = inLen / 2;
            float              hpEner = 0;
            for (std::int32_t k = 0; k < len2; ++k) {
                // all-pass section for the even input sample
                float in32  = in[2 * k];
                float y     = in32 - s[0];
                float x     = 0.6074371f * y;
                float out32 = s[0] + x;
                s[0]        = in32 + x;
                float outHp = out32;
                // all-pass section for the odd input sample, added to the output of the previous one
                in32  = in[2 * k + 1];
                y     = in32 - s[1];
                x     = 0.15063f * y;
                out32 = out32 + s[1];
                out32 = out32 + x;
                s[1]  = in32 + x;

                y     = -in32 - s[2];
                x     = 0.15063f * y;
                outHp = outHp + s[2];
                outHp = outHp + x;
                s[2]  = -in32 + x;

                hpEner += outHp * outHp;
                out[k] = .5f * out32;
            }
            return hpEner;
        }

        /// <summary>downmix_float of opus_encoder.c.</summary>
        void DownmixFloat(const float* x, float* y, std::int32_t subframe, std::int32_t offset, std::int32_t c1, std::int32_t c2,
                          std::int32_t channels) noexcept
        {
            for (std::int32_t j = 0; j < subframe; ++j)
                y[j] = x[(j + offset) * channels + c1] * CeltSigScale;
            if (c2 > -1) {
                for (std::int32_t j = 0; j < subframe; ++j)
                    y[j] += x[(j + offset) * channels + c2] * CeltSigScale;
            } else if (c2 == -2) {
                for (std::int32_t c = 1; c < channels; ++c) {
                    for (std::int32_t j = 0; j < subframe; ++j)
                        y[j] += x[(j + offset) * channels + c] * CeltSigScale;
                }
            }
            // cap the signal to +6 dBFS against problems in the analysis
            for (std::int32_t j = 0; j < subframe; ++j) {
                if (y[j] < -65536.f)
                    y[j] = -65536.f;
                if (y[j] > 65536.f)
                    y[j] = 65536.f;
                if (IsNan(y[j]))
                    y[j] = 0;
            }
        }

        inline float BinEnergy(const FftComplex* out, std::int32_t n, std::int32_t i) noexcept
        {
            return out[i].r * out[i].r + out[n - i].r * out[n - i].r + out[i].i * out[i].i + out[n - i].i * out[n - i].i;
        }

    } // namespace

    // libopus: tonality_analysis_init() (src/analysis.c)
    void ToneAnalysis::Init(std::int32_t fs) noexcept
    {
        fs_ = fs;
        Reset();
    }

    // libopus: tonality_analysis_reset() (src/analysis.c)
    void ToneAnalysis::Reset() noexcept
    {
        const std::int32_t fs      = fs_;
        AnalysisInfo*      history = history_;
        std::memset(static_cast<void*>(this), 0, sizeof(*this));
        fs_      = fs;
        history_ = history;
    }

    const AnalysisInfo& ToneAnalysis::EntryAt(const View& view, std::int32_t pos) const noexcept
    {
        if (history_ == nullptr)
            return info_[pos];
        // the ring position of the oldest entry is the write position: the absolute entry number
        // is view.entries - DetectSize + (pos - writePos) mod DetectSize
        std::int32_t back = view.writePos - pos;
        if (back <= 0)
            back += DetectSize;
        // not written yet: a cleared entry, as in the ring after the reset
        static constexpr AnalysisInfo cleared = {};
        const std::int64_t            entry   = view.entries - back;
        return entry < 0 ? cleared : history_[static_cast<std::size_t>(entry & (HistorySize - 1))];
    }

    // libopus: downmix_and_resample() (src/analysis.c)
    float ToneAnalysis::DownmixAndResample(const float* x, float* y, std::int32_t subframe, std::int32_t offset, std::int32_t c1, std::int32_t c2,
                                           std::int32_t channels) noexcept
    {
        if (subframe == 0)
            return 0;
        if (fs_ == 48000) {
            subframe *= 2;
            offset *= 2;
        } else if (fs_ == 16000) {
            subframe = subframe * 2 / 3;
            offset   = offset * 2 / 3;
        }
        float tmp[960];
        DownmixFloat(x, tmp, subframe, offset, c1, c2, channels);
        if ((c2 == -2 && channels == 2) || c2 > -1) {
            for (std::int32_t j = 0; j < subframe; ++j)
                tmp[j] = .5f * tmp[j];
        }
        float ret = 0;
        if (fs_ == 48000) {
            ret = ResamplerDown2Hp(downmixState_, y, tmp, subframe);
        } else if (fs_ == 24000) {
            std::memcpy(y, tmp, static_cast<std::size_t>(subframe) * sizeof(float));
        } else if (fs_ == 16000) {
            // a crude 3x upsampling, enough for the analysis (aliasing above 8 kHz does not matter)
            float tmp3x[960];
            for (std::int32_t j = 0; j < subframe; ++j) {
                tmp3x[3 * j]     = tmp[j];
                tmp3x[3 * j + 1] = tmp[j];
                tmp3x[3 * j + 2] = tmp[j];
            }
            ResamplerDown2Hp(downmixState_, y, tmp3x, 3 * subframe);
        }
        ret *= 1.f / 32768 / 32768;
        return ret;
    }

    // libopus: tonality_get_info() (src/analysis.c)
    void ToneAnalysis::GetInfo(AnalysisInfo& infoOut, std::int32_t len) noexcept
    {
        GetInfoFrom(*this, CurrentView(), infoOut, len);
    }

    void ToneAnalysis::PeekInfoFrom(const ToneAnalysis& chain, const View* views, AnalysisInfo* infos, std::int32_t count, std::int32_t len) const noexcept
    {
        // a reader with this read position (GetInfoFrom reads nothing else of it)
        ToneAnalysis reader;
        reader.fs_           = fs_;
        reader.readPos_      = readPos_;
        reader.readSubframe_ = readSubframe_;
        for (std::int32_t i = 0; i < count; ++i)
            reader.GetInfoFrom(chain, views[i], infos[i], len);
    }

    // libopus: tonality_get_info() (src/analysis.c), reading the entries of chain
    void ToneAnalysis::GetInfoFrom(const ToneAnalysis& chain, const View& view, AnalysisInfo& infoOut, std::int32_t len) noexcept
    {
        const std::int32_t writePos      = view.writePos;
        std::int32_t       pos           = readPos_;
        std::int32_t       currLookahead = writePos - readPos_;
        if (currLookahead < 0)
            currLookahead += DetectSize;

        readSubframe_ += len / (fs_ / 400);
        while (readSubframe_ >= 8) {
            readSubframe_ -= 8;
            ++readPos_;
        }
        if (readPos_ >= DetectSize)
            readPos_ -= DetectSize;

        // on long frames, look at the second analysis window rather than the first
        if (len > fs_ / 50 && pos != writePos) {
            ++pos;
            if (pos == DetectSize)
                pos = 0;
        }
        if (pos == writePos)
            --pos;
        if (pos < 0)
            pos = DetectSize - 1;
        const std::int32_t pos0 = pos;
        infoOut                 = chain.EntryAt(view, pos);
        if (infoOut.valid == 0)
            return;
        float        tonalityMax   = infoOut.tonality;
        float        tonalityAvg   = infoOut.tonality;
        std::int32_t tonalityCount = 1;
        // the largest bandwidth of the neighbouring frames (to be safe)
        std::int32_t bandwidthSpan = 6;
        // look ahead for a tone, against the delay of the tone detector
        for (std::int32_t i = 0; i < 3; ++i) {
            ++pos;
            if (pos == DetectSize)
                pos = 0;
            if (pos == writePos)
                break;
            tonalityMax = Max32(tonalityMax, chain.EntryAt(view, pos).tonality);
            tonalityAvg += chain.EntryAt(view, pos).tonality;
            ++tonalityCount;
            infoOut.bandwidth = Imax(infoOut.bandwidth, chain.EntryAt(view, pos).bandwidth);
            --bandwidthSpan;
        }
        pos = pos0;
        // look back for a wider bandwidth than in the current frame
        for (std::int32_t i = 0; i < bandwidthSpan; ++i) {
            --pos;
            if (pos < 0)
                pos = DetectSize - 1;
            if (pos == writePos)
                break;
            infoOut.bandwidth = Imax(infoOut.bandwidth, chain.EntryAt(view, pos).bandwidth);
        }
        infoOut.tonality = Max32(tonalityAvg / tonalityCount, tonalityMax - .2f);

        std::int32_t mpos = pos0;
        std::int32_t vpos = pos0;
        // with enough look-ahead, compensate the ~5 frame delay of the music probability and the
        // ~1 frame delay of the VAD probability
        if (currLookahead > 15) {
            mpos += 5;
            if (mpos >= DetectSize)
                mpos -= DetectSize;
            vpos += 1;
            if (vpos >= DetectSize)
                vpos -= DetectSize;
        }

        // The thresholds for which switching now is optimal (see the "badness function" in
        // analysis.c): music_prob_min for speech to music, music_prob_max for music to speech.
        float       probMin   = 1.f;
        float       probMax   = 0.f;
        const float vadProb   = chain.EntryAt(view, vpos).activityProbability;
        float       probCount = Max32(.1f, vadProb);
        float       probAvg   = Max32(.1f, vadProb) * chain.EntryAt(view, mpos).musicProb;
        while (true) {
            ++mpos;
            if (mpos == DetectSize)
                mpos = 0;
            if (mpos == writePos)
                break;
            ++vpos;
            if (vpos == DetectSize)
                vpos = 0;
            if (vpos == writePos)
                break;
            const float posVad = chain.EntryAt(view, vpos).activityProbability;
            probMin            = Min32((probAvg - TransitionPenalty * (vadProb - posVad)) / probCount, probMin);
            probMax            = Max32((probAvg + TransitionPenalty * (vadProb - posVad)) / probCount, probMax);
            probCount += Max32(.1f, posVad);
            probAvg += Max32(.1f, posVad) * chain.EntryAt(view, mpos).musicProb;
        }
        infoOut.musicProb = probAvg / probCount;
        probMin           = Min32(probAvg / probCount, probMin);
        probMax           = Max32(probAvg / probCount, probMax);
        probMin           = Max32(probMin, 0.f);
        probMax           = Min32(probMax, 1.f);

        // without enough look-ahead, do the best for a decent decision
        if (currLookahead < 10) {
            float pmin = probMin;
            float pmax = probMax;
            pos        = pos0;
            // min/max in the past
            for (std::int32_t i = 0; i < Imin(view.count - 1, 15); ++i) {
                --pos;
                if (pos < 0)
                    pos = DetectSize - 1;
                pmin = Min32(pmin, chain.EntryAt(view, pos).musicProb);
                pmax = Max32(pmax, chain.EntryAt(view, pos).musicProb);
            }
            // bias against switching on active audio
            pmin = Max32(0.f, pmin - .1f * vadProb);
            pmax = Min32(1.f, pmax + .1f * vadProb);
            probMin += (1.f - .1f * currLookahead) * (pmin - probMin);
            probMax += (1.f - .1f * currLookahead) * (pmax - probMax);
        }
        infoOut.musicProbMin = probMin;
        infoOut.musicProbMax = probMax;
    }

    // libopus: tonality_analysis() (src/analysis.c) as run_analysis() calls it (AnalyzeFrame), with the entry kept
    // for the readers of a chain
    void ToneAnalysis::Analyze(const float* x, std::int32_t len, std::int32_t offset, std::int32_t c1, std::int32_t c2, std::int32_t channels,
                               std::int32_t lsbDepth) noexcept
    {
        // at most one new entry per call; a chain keeps a copy once it is finished
        const std::int32_t writePos = writePos_;
        AnalyzeFrame(x, len, offset, c1, c2, channels, lsbDepth);
        if (writePos_ != writePos) {
            if (history_ != nullptr)
                history_[static_cast<std::size_t>(entries_ & (HistorySize - 1))] = info_[writePos];
            ++entries_;
        }
    }

    // libopus: tonality_analysis() (src/analysis.c)
    void ToneAnalysis::AnalyzeFrame(const float* x, std::int32_t len, std::int32_t offset, std::int32_t c1, std::int32_t c2, std::int32_t channels,
                                    std::int32_t lsbDepth) noexcept
    {
        constexpr std::int32_t n   = 480;
        constexpr std::int32_t n2  = 240;
        const float            pi4 = static_cast<float>(Pi * Pi * Pi * Pi);

        if (initialized_ == 0) {
            memFill_     = 240;
            initialized_ = 1;
        }
        const float alpha  = 1.f / Imin(10, 1 + count_);
        const float alphaE = 1.f / Imin(25, 1 + count_);
        // noise floor decay for the bandwidth detection: -2.2 dB/second
        float alphaE2 = 1.f / Imin(100, 1 + count_);
        if (count_ <= 1)
            alphaE2 = 1;

        if (fs_ == 48000) {
            // len and offset are now at 24 kHz
            len /= 2;
            offset /= 2;
        } else if (fs_ == 16000) {
            len    = 3 * len / 2;
            offset = 3 * offset / 2;
        }

        hpEnerAccum_ += DownmixAndResample(x, &inmem_[memFill_], Imin(len, AnalysisBufSize - memFill_), offset, c1, c2, channels);
        if (memFill_ + len < AnalysisBufSize) {
            // not enough to update the analysis
            memFill_ += len;
            return;
        }
        const float   hpEner = hpEnerAccum_;
        AnalysisInfo& info   = info_[writePos_++];
        if (writePos_ >= DetectSize)
            writePos_ -= DetectSize;

        const bool isSilence = IsDigitalSilence(inmem_, AnalysisBufSize, 1, lsbDepth);

        FftComplex in[n];
        FftComplex out[n];
        float      tonality[240];
        float      noisiness[240];
        float      tonality2[240];
        for (std::int32_t i = 0; i < n2; ++i) {
            const float w   = AnalysisWindow[i];
            in[i].r         = w * inmem_[i];
            in[i].i         = w * inmem_[n2 + i];
            in[n - i - 1].r = w * inmem_[n - i - 1];
            in[n - i - 1].i = w * inmem_[n + n2 - i - 1];
        }
        std::memmove(inmem_, inmem_ + AnalysisBufSize - 240, 240 * sizeof(float));
        const std::int32_t remaining = len - (AnalysisBufSize - memFill_);
        hpEnerAccum_                 = DownmixAndResample(x, &inmem_[240], remaining, offset + AnalysisBufSize - memFill_, c1, c2, channels);
        memFill_                     = 240 + remaining;
        if (isSilence) {
            // on silence, copy the previous analysis
            std::int32_t prevPos = writePos_ - 2;
            if (prevPos < 0)
                prevPos += DetectSize;
            info = info_[prevPos];
            return;
        }
        Fft(Fft48000(0), in, out);
        // a NaN in the input makes the whole output NaN, so one value suffices
        if (IsNan(out[0].r)) {
            info.valid = 0;
            return;
        }

        for (std::int32_t i = 1; i < n2; ++i) {
            const float x1r = out[i].r + out[n - i].r;
            const float x1i = out[i].i - out[n - i].i;
            const float x2r = out[i].i + out[n - i].i;
            const float x2i = out[n - i].r - out[i].r;

            const float angle   = static_cast<float>(.5f / Pi) * FastAtan2f(x1i, x1r);
            const float dAngle  = angle - angle_[i];
            const float d2Angle = dAngle - dAngle_[i];

            const float angle2   = static_cast<float>(.5f / Pi) * FastAtan2f(x2i, x2r);
            const float dAngle2  = angle2 - angle;
            const float d2Angle2 = dAngle2 - dAngle;

            float mod1   = d2Angle - static_cast<float>(Float2Int(d2Angle));
            noisiness[i] = std::fabs(mod1);
            mod1 *= mod1;
            mod1 *= mod1;

            float mod2 = d2Angle2 - static_cast<float>(Float2Int(d2Angle2));
            noisiness[i] += std::fabs(mod2);
            mod2 *= mod2;
            mod2 *= mod2;

            const float avgMod = .25f * (d2Angle_[i] + mod1 + 2 * mod2);
            // an extra delay of 2 frames in the detection
            tonality[i] = 1.f / (1.f + 40.f * 16.f * pi4 * avgMod) - .015f;
            // no delay, but less reliable
            tonality2[i] = 1.f / (1.f + 40.f * 16.f * pi4 * mod2) - .015f;

            angle_[i]   = angle2;
            dAngle_[i]  = dAngle2;
            d2Angle_[i] = mod2;
        }
        for (std::int32_t i = 2; i < n2 - 1; ++i) {
            const float tt = Min32(tonality2[i], Max32(tonality2[i - 1], tonality2[i + 1]));
            tonality[i]    = .9f * Max32(tonality[i], tt - .1f);
        }
        float frameTonality     = 0;
        float maxFrameTonality  = 0;
        info.activity           = 0;
        float frameNoisiness    = 0;
        float frameStationarity = 0;
        if (count_ == 0) {
            for (std::int32_t b = 0; b < NbTBands; ++b) {
                lowE_[b]  = 1e10f;
                highE_[b] = -1e10f;
            }
        }
        float relativeE     = 0;
        float frameLoudness = 0;
        float bandLog2[NbTBands + 1];
        float logE[NbTBands];
        float bandTonality[NbTBands];
        float slope = 0;
        // the energy of the first band is special because of DC
        {
            const float x1r = 2 * out[0].r;
            const float x2r = 2 * out[0].i;
            float       e   = x1r * x1r + x2r * x2r;
            for (std::int32_t i = 1; i < 4; ++i)
                e += BinEnergy(out, n, i);
            e           = (1.f / 32768 / 32768) * e;
            bandLog2[0] = .5f * 1.442695f * LogD(e + 1e-10f);
        }
        for (std::int32_t b = 0; b < NbTBands; ++b) {
            float e  = 0;
            float tE = 0;
            float nE = 0;
            for (std::int32_t i = TBands[b]; i < TBands[b + 1]; ++i) {
                const float binE = (1.f / 32768 / 32768) * BinEnergy(out, n, i);
                e += binE;
                tE += binE * Max32(0, tonality[i]);
                nE += binE * 2.f * (.5f - noisiness[i]);
            }
            // extreme band energies could cause NaNs later
            if (!(e < 1e9f) || IsNan(e)) {
                info.valid = 0;
                return;
            }

            e_[eCount_][b] = e;
            frameNoisiness += nE / (1e-15f + e);

            frameLoudness += SqrtD(e + 1e-10f);
            logE[b]           = LogD(e + 1e-10f);
            bandLog2[b + 1]   = .5f * 1.442695f * LogD(e + 1e-10f);
            logE_[eCount_][b] = logE[b];
            if (count_ == 0)
                highE_[b] = lowE_[b] = logE[b];
            if (highE_[b] > lowE_[b] + 7.5) {
                if (highE_[b] - logE[b] > logE[b] - lowE_[b])
                    highE_[b] -= .01f;
                else
                    lowE_[b] += .01f;
            }
            if (logE[b] > highE_[b]) {
                highE_[b] = logE[b];
                lowE_[b]  = Max32(highE_[b] - 15, lowE_[b]);
            } else if (logE[b] < lowE_[b]) {
                lowE_[b]  = logE[b];
                highE_[b] = Min32(lowE_[b] + 15, highE_[b]);
            }
            relativeE += (logE[b] - lowE_[b]) / (1e-5f + (highE_[b] - lowE_[b]));

            float l1 = 0;
            float l2 = 0;
            for (std::int32_t i = 0; i < NbFrames; ++i) {
                l1 += SqrtD(e_[i][b]);
                l2 += e_[i][b];
            }

            float stationarity = Min32(0.99f, l1 / SqrtD(1e-15 + NbFrames * l2));
            stationarity *= stationarity;
            stationarity *= stationarity;
            frameStationarity += stationarity;
            bandTonality[b] = Max32(tE / (1e-15f + e), stationarity * prevBandTonality_[b]);
            frameTonality += bandTonality[b];
            if (b >= NbTBands - NbTonalSkipBands)
                frameTonality -= bandTonality[b - NbTBands + NbTonalSkipBands];
            maxFrameTonality = Max32(maxFrameTonality, (1.f + .03f * (b - NbTBands)) * frameTonality);
            slope += bandTonality[b] * (b - 8);
            prevBandTonality_[b] = bandTonality[b];
        }

        float leakageFrom[NbTBands + 1];
        float leakageTo[NbTBands + 1];
        leakageFrom[0] = bandLog2[0];
        leakageTo[0]   = bandLog2[0] - LeakageOffset;
        for (std::int32_t b = 1; b < NbTBands + 1; ++b) {
            const float leakSlope = LeakageSlope * (TBands[b] - TBands[b - 1]) / 4;
            leakageFrom[b]        = Min32(leakageFrom[b - 1] + leakSlope, bandLog2[b]);
            leakageTo[b]          = Max32(leakageTo[b - 1] - leakSlope, bandLog2[b] - LeakageOffset);
        }
        for (std::int32_t b = NbTBands - 2; b >= 0; --b) {
            const float leakSlope = LeakageSlope * (TBands[b + 1] - TBands[b]) / 4;
            leakageFrom[b]        = Min32(leakageFrom[b + 1] + leakSlope, leakageFrom[b]);
            leakageTo[b]          = Max32(leakageTo[b + 1] - leakSlope, leakageTo[b]);
        }
        {
            // leak_boost: the boost a weak band b needs against the analysis leakage of louder
            // neighbours, plus the boost of a loud band whose quantisation noise leaks into weaker ones
            std::int32_t b = 0;
            for (; b < NbTBands + 1; ++b) {
                const float boost = Max32(0, leakageTo[b] - bandLog2[b]) + Max32(0, bandLog2[b] - (leakageFrom[b] + LeakageOffset));
                info.leakBoost[b] = static_cast<std::uint8_t>(Imin(255, static_cast<std::int32_t>(std::floor(.5 + 64.f * boost))));
            }
            for (; b < LeakBands; ++b)
                info.leakBoost[b] = 0;
        }

        float specVariability = 0;
        for (std::int32_t i = 0; i < NbFrames; ++i) {
            float mindist = 1e15f;
            for (std::int32_t j = 0; j < NbFrames; ++j) {
                float dist = 0;
                for (std::int32_t k = 0; k < NbTBands; ++k) {
                    const float tmp = logE_[i][k] - logE_[j][k];
                    dist += tmp * tmp;
                }
                if (j != i)
                    mindist = Min32(mindist, dist);
            }
            specVariability += mindist;
        }
        specVariability            = SqrtD(specVariability / NbFrames / NbTBands);
        float        bandwidthMask = 0;
        std::int32_t bandwidth     = 0;
        float        maxE          = 0;
        float        noiseFloor    = 5.7e-4f / (1 << (Imax(0, lsbDepth - 8)));
        noiseFloor *= noiseFloor;
        float        belowMaxPitch = 0;
        float        aboveMaxPitch = 0;
        bool         isMasked[NbTBands + 1];
        std::int32_t b = 0;
        for (; b < NbTBands; ++b) {
            // a margin of 300 Hz for aliasing
            const std::int32_t bandStart = TBands[b];
            const std::int32_t bandEnd   = TBands[b + 1];
            float              e         = 0;
            for (std::int32_t i = bandStart; i < bandEnd; ++i)
                e += BinEnergy(out, n, i);
            e    = (1.f / 32768 / 32768) * e;
            maxE = Max32(maxE, e);
            if (bandStart < 64)
                belowMaxPitch += e;
            else
                aboveMaxPitch += e;
            meanE_[b]      = Max32((1 - alphaE2) * meanE_[b], e);
            const float em = Max32(e, meanE_[b]);
            // the band is active only if less than 90 dB below the peak band and above the PCM
            // quantisation noise floor (b+1 because the first CELT band is not in TBands)
            if (e * 1e9f > maxE && (em > 3 * noiseFloor * (bandEnd - bandStart) || e > noiseFloor * (bandEnd - bandStart)))
                bandwidth = b + 1;
            // masked (see below)
            isMasked[b] = e < (prevBandwidth_ >= b + 1 ? .01f : .05f) * bandwidthMask;
            // a simple follower with a 13 dB/Bark slope as spreading function
            bandwidthMask = Max32(.05f * bandwidthMask, e);
        }
        // The last two bands have no spectrum, only the energy above 12 kHz. The high-pass leaks
        // some LF energy, so the threshold is higher, without cutting off the band by accident.
        if (fs_ == 48000) {
            const float e          = hpEner * (1.f / (60 * 60));
            const float noiseRatio = prevBandwidth_ == 20 ? 10.f : 30.f;
            aboveMaxPitch += e;
            meanE_[b]      = Max32((1 - alphaE2) * meanE_[b], e);
            const float em = Max32(e, meanE_[b]);
            if (em > 3 * noiseRatio * noiseFloor * 160 || e > noiseRatio * noiseFloor * 160)
                bandwidth = 20;
            isMasked[b] = e < (prevBandwidth_ == 20 ? .01f : .05f) * bandwidthMask;
        }
        if (aboveMaxPitch > belowMaxPitch)
            info.maxPitchRatio = belowMaxPitch / aboveMaxPitch;
        else
            info.maxPitchRatio = 1;
        // resampling aliasing can put a little energy into the first band being cut: if the last
        // band is masked, it is not included
        if (bandwidth == 20 && isMasked[NbTBands])
            bandwidth -= 2;
        else if (bandwidth > 0 && bandwidth <= NbTBands && isMasked[bandwidth - 1])
            --bandwidth;
        if (count_ <= 2)
            bandwidth = 20;
        frameLoudness = 20 * static_cast<float>(std::log10(static_cast<double>(frameLoudness)));
        eTracker_     = Max32(eTracker_ - .003f, frameLoudness);
        lowECount_ *= (1 - alphaE);
        if (frameLoudness < eTracker_ - 30)
            lowECount_ += alphaE;

        float bfcc[8];
        float midE[8];
        for (std::int32_t i = 0; i < 8; ++i) {
            float sum = 0;
            for (std::int32_t k = 0; k < 16; ++k)
                sum += DctTable[i * 16 + k] * logE[k];
            bfcc[i] = sum;
        }
        for (std::int32_t i = 0; i < 8; ++i) {
            float sum = 0;
            for (std::int32_t k = 0; k < 16; ++k)
                sum += DctTable[i * 16 + k] * .5f * (highE_[k] + lowE_[k]);
            midE[i] = sum;
        }

        frameStationarity /= NbTBands;
        relativeE /= NbTBands;
        if (count_ < 10)
            relativeE = .5f;
        frameNoisiness /= NbTBands;
        info.activity = frameNoisiness + (1 - frameNoisiness) * relativeE;
        frameTonality = (maxFrameTonality / (NbTBands - NbTonalSkipBands));
        frameTonality = Max32(frameTonality, prevTonality_ * .8f);
        prevTonality_ = frameTonality;

        slope /= 8 * 8;
        info.tonalitySlope = slope;

        eCount_       = (eCount_ + 1) % NbFrames;
        count_        = Imin(count_ + 1, AnalysisCountMax);
        info.tonality = frameTonality;

        float features[25];
        for (std::int32_t i = 0; i < 4; ++i)
            features[i] = -0.12299f * (bfcc[i] + mem_[i + 24]) + 0.49195f * (mem_[i] + mem_[i + 16]) + 0.69693f * mem_[i + 8] - 1.4349f * cmean_[i];

        for (std::int32_t i = 0; i < 4; ++i)
            cmean_[i] = (1 - alpha) * cmean_[i] + alpha * bfcc[i];

        for (std::int32_t i = 0; i < 4; ++i)
            features[4 + i] = 0.63246f * (bfcc[i] - mem_[i + 24]) + 0.31623f * (mem_[i] - mem_[i + 16]);
        for (std::int32_t i = 0; i < 3; ++i)
            features[8 + i] = 0.53452f * (bfcc[i] + mem_[i + 24]) - 0.26726f * (mem_[i] + mem_[i + 16]) - 0.53452f * mem_[i + 8];

        if (count_ > 5) {
            for (std::int32_t i = 0; i < 9; ++i)
                std_[i] = (1 - alpha) * std_[i] + alpha * features[i] * features[i];
        }
        for (std::int32_t i = 0; i < 4; ++i)
            features[i] = bfcc[i] - midE[i];

        for (std::int32_t i = 0; i < 8; ++i) {
            mem_[i + 24] = mem_[i + 16];
            mem_[i + 16] = mem_[i + 8];
            mem_[i + 8]  = mem_[i];
            mem_[i]      = bfcc[i];
        }
        for (std::int32_t i = 0; i < 9; ++i)
            features[11 + i] = SqrtD(std_[i]) - StdFeatureBias[i];
        features[18] = specVariability - 0.78f;
        features[20] = info.tonality - 0.154723f;
        features[21] = info.activity - 0.724643f;
        features[22] = frameStationarity - 0.743717f;
        features[23] = info.tonalitySlope + 0.069216f;
        features[24] = lowECount_ - 0.067930f;

        float layerOut[MaxNeurons];
        float frameProbs[2];
        ComputeDense(Layer0, layerOut, features);
        ComputeGru(Layer1, rnnState_, layerOut);
        ComputeDense(Layer2, frameProbs, rnnState_);

        // the probability of speech or music vs noise
        info.activityProbability = frameProbs[1];
        info.musicProb           = frameProbs[0];

        info.bandwidth = bandwidth;
        prevBandwidth_ = bandwidth;
        info.noisiness = frameNoisiness;
        info.valid     = 1;
    }

    // libopus: run_analysis() (src/analysis.c)
    void ToneAnalysis::Run(const float* pcm, std::int32_t analysisFrameSize, std::int32_t frameSize, std::int32_t c1, std::int32_t c2, std::int32_t channels,
                           std::int32_t lsbDepth, AnalysisInfo& info) noexcept
    {
        Advance(pcm, analysisFrameSize, frameSize, c1, c2, channels, lsbDepth);
        GetInfo(info, frameSize);
    }

    // libopus: run_analysis() (src/analysis.c), the analysis loop without tonality_get_info()
    void ToneAnalysis::Advance(const float* pcm, std::int32_t analysisFrameSize, std::int32_t frameSize, std::int32_t c1, std::int32_t c2,
                               std::int32_t channels, std::int32_t lsbDepth) noexcept
    {
        analysisFrameSize -= analysisFrameSize & 1;
        if (pcm != nullptr) {
            // against overflow/wrap-around of the analysis buffer
            analysisFrameSize   = Imin((DetectSize - 5) * fs_ / 50, analysisFrameSize);
            std::int32_t pcmLen = analysisFrameSize - analysisOffset_;
            std::int32_t offset = analysisOffset_;
            while (pcmLen > 0) {
                Analyze(pcm, Imin(fs_ / 50, pcmLen), offset, c1, c2, channels, lsbDepth);
                offset += fs_ / 50;
                pcmLen -= fs_ / 50;
            }
            analysisOffset_ = analysisFrameSize - frameSize;
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
