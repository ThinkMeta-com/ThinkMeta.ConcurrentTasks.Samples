// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "GainAnalysis.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "GainKernels.h"
#include "Memory.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        // The filter coefficients for 48, 44.1, 32, 24, 22.05, 16, 12, 11.025 and 8 kHz. LAME keeps them as Float_t,
        // so the double constants are rounded to float; ChannelGain::Initialize does that for the row in use.

        // Yule-Walker: b[20], b[18], ..., b[0] for the inputs, then a[19], a[17], ..., a[1] for the outputs
        constexpr double ABYule[9][2 * 10 + 1]
            = {{0.00288463683916,  0.00012025322027,  0.00306428023191,   0.00594298065125,  -0.02074045215285,  0.02161526843274,  -0.01655260341619,
                -0.00009291677959, -0.00123395316851, -0.02160367184185,  0.03857599435200,  0.13919314567432,   -0.86984376593551, 2.75465861874613,
                -5.87257861775999, 9.48293806319790,  -12.28759895145294, 13.05504219327545, -11.34170355132042, 7.81501653005538,  -3.84664617118067},
               {-0.00187763777362, 0.00674613682247,  -0.00240879051584, 0.01624864962975, -0.02596338512915, 0.02245293253339,  -0.00834990904936,
                -0.00851165645469, -0.00848709379851, -0.02911007808948, 0.05418656406430, 0.13149317958808,  -0.75104302451432, 2.19611684890774,
                -4.39470996079559, 6.85401540936998,  -8.81498681370155, 9.47693607801280, -8.54751527471874, 6.36317777566148,  -3.47845948550071},
               {-0.00881362733839, 0.00651420667831,  -0.01390589421898, 0.03174092540049, 0.00222312597743,  0.04781476674921,  -0.05588393329856,
                0.02163541888798,  -0.06247880153653, -0.09331049056315, 0.15457299681924, 0.02347897407020,  -0.05032077717131, 0.16378164858596,
                -0.45953458054983, 1.00595954808547,  -1.67148153367602, 2.23697657451713, -2.64577170229825, 2.84868151156327,  -2.37898834973084},
               {-0.02950134983287, 0.00205861885564,  -0.00000828086748, 0.06276101321749,  -0.00584456039913, -0.02364141202522, -0.00915702933434,
                0.03282930172664,  -0.08587323730772, -0.22613988682123, 0.30296907319327,  0.00302439095741,  0.02005851806501,  0.04500235387352,
                -0.22138138954925, 0.39120800788284,  -0.22638893773906, -0.16276719120440, -0.25656257754070, 1.07977492259970,  -1.61273165137247},
               {-0.01760176568150, -0.01635381384540, 0.00832043980773,  0.05724228140351,  -0.00589500224440, -0.00469977914380, -0.07834489609479,
                0.11921148675203,  -0.11828570177555, -0.25572241425570, 0.33642304856132,  0.02977207319925,  -0.04237348025746, 0.08333755284107,
                -0.04067510197014, -0.12453458140019, 0.47854794562326,  -0.80774944671438, 0.12205022308084,  0.87350271418188,  -1.49858979367799},
               {0.00541907748707,  -0.03193428438915, -0.01863887810927, 0.10478503600251, 0.04097565135648,  -0.12398163381748, 0.04078262797139,
                -0.01419140100551, -0.22784394429749, -0.14351757464547, 0.44915256608450, 0.03222754072173,  0.05784820375801,  0.06747620744683,
                0.00613424350682,  0.22199650564824,  -0.42029820170918, 0.00213767857124, -0.37256372942400, 0.29661783706366,  -0.62820619233671},
               {-0.00588215443421, -0.03788984554840, 0.08647503780351,  0.00647310677246, -0.27562961986224, 0.30931782841830, -0.18901604199609,
                0.16744243493672,  0.16242137742230,  -0.75464456939302, 0.56619470757641, 0.01807364323573,  0.01639907836189, -0.04784254229033,
                0.06739368333110,  -0.33032403314006, 0.45054734505008,  0.00819999645858, -0.26806001042947, 0.29156311971249, -1.04800335126349},
               {-0.00749618797172, -0.03721611395801, 0.06920467763959,  0.01628462406333, -0.25344790059353, 0.15558449135573,  0.02377945217615,
                0.17520704835522,  -0.14289799034253, -0.53174909058578, 0.58100494960553, 0.01818801111503,  0.02442357316099,  -0.02505961724053,
                -0.05246019024463, -0.23313271880868, 0.38952639978999,  0.14728154134330, -0.20256413484477, -0.31863563325245, -0.51035327095184},
               {-0.02217936801134, 0.04788665548180,  -0.04060034127000, -0.11202315195388, -0.02459864859345, 0.14590772289388,  -0.10214864179676,
                0.04267842219415,  -0.00275953611929, -0.42163034350696, 0.53648789255105,  0.04704409688120,  0.05477720428674,  -0.18823009262115,
                -0.17556493366449, 0.15113130533216,  0.26408300200955,  -0.04678328784242, -0.03424681017675, -0.43193942311114, -0.25049871956020}};

        // Butterworth high pass: b[2], a[2], b[1], a[1], b[0]
        constexpr double ABButter[9][2 * 2 + 1] = {{0.98621192462708, 0.97261396931306, -1.97242384925416, -1.97223372919527, 0.98621192462708},
                                                   {0.98500175787242, 0.97022847566350, -1.97000351574484, -1.96977855582618, 0.98500175787242},
                                                   {0.97938932735214, 0.95920349965459, -1.95877865470428, -1.95835380975398, 0.97938932735214},
                                                   {0.97531843204928, 0.95124613669835, -1.95063686409857, -1.95002759149878, 0.97531843204928},
                                                   {0.97316523498161, 0.94705070426118, -1.94633046996323, -1.94561023566527, 0.97316523498161},
                                                   {0.96454515552826, 0.93034775234268, -1.92909031105652, -1.92783286977036, 0.96454515552826},
                                                   {0.96009142950541, 0.92177618768381, -1.92018285901082, -1.91858953033784, 0.96009142950541},
                                                   {0.95856916599601, 0.91885558323625, -1.91713833199203, -1.91542108074780, 0.95856916599601},
                                                   {0.94597685600279, 0.89487434461664, -1.89195371200558, -1.88903307939452, 0.94597685600279}};

        constexpr double PinkRef       = 64.82; // calibration value for 89 dB
        constexpr double RmsPercentile = 0.95;  // percentile which is louder than the proposed level

        // The rest of filterYule and all of filterButter, in one loop and LAME's order: yule[0] = s1 - s2 with s2 from
        // yule[-10 .. -1]; the Butterworth filter of the same sample needs only yule[-2 .. 0]. So the two serial chains,
        // through yule[-1] (xH, the last term of s2) and through butter[-1], overlap.
        void Feedback(const sample_t* s1, sample_t* yule, sample_t* butter, std::size_t count, const float* yuleKernel, const float* butterKernel) noexcept
        {
            // the history in locals, so the chains do not run through memory: yN = yule[i - N], bN = butter[i - N]
            const float* const k  = yuleKernel;
            const float* const kb = butterKernel;
            sample_t           y1 = yule[-1], y2 = yule[-2], y3 = yule[-3], y4 = yule[-4], y5 = yule[-5];
            sample_t           y6 = yule[-6], y7 = yule[-7], y8 = yule[-8], y9 = yule[-9], y10 = yule[-10];
            sample_t           b1 = butter[-1], b2 = butter[-2];
            for (std::size_t i = 0; i < count; ++i) {
                const sample_t x1 = y10 * k[11] + y9 * k[12];
                const sample_t x5 = y8 * k[13] + y7 * k[14];
                const sample_t x9 = y6 * k[15] + y5 * k[16];
                const sample_t xD = y4 * k[17] + y3 * k[18];
                const sample_t xH = y2 * k[19] + y1 * k[20];
                const sample_t s2 = x1 + x5 + x9 + xD + xH;
                const sample_t y0 = s1[i] - s2;
                yule[i]           = y0;

                // filterButter: ButterFir on yule[i - 2 .. i], then the feedback
                const sample_t bs1 = y2 * kb[0] + y1 * kb[2] + y0 * kb[4];
                const sample_t bs2 = b2 * kb[1] + b1 * kb[3];
                const sample_t b0  = bs1 - bs2;
                butter[i]          = b0;

                y10 = y9;
                y9  = y8;
                y8  = y7;
                y7  = y6;
                y6  = y5;
                y5  = y4;
                y4  = y3;
                y3  = y2;
                y2  = y1;
                y1  = y0;
                b2  = b1;
                b1  = b0;
            }
        }

        // LAME: analyzeResult()
        float AnalyzeResult(const std::uint32_t* array, std::size_t length) noexcept
        {
            std::uint32_t elems = 0;
            for (std::size_t i = 0; i < length; i++)
                elems += array[i];
            if (elems == 0)
                return GainAnalysis::NotEnoughSamples;

            const auto    upper = static_cast<std::uint32_t>(std::ceil(elems * (1. - RmsPercentile)));
            std::uint32_t sum   = 0;
            std::size_t   i     = length;
            while (i-- > 0) {
                sum += array[i];
                if (sum >= upper)
                    break;
            }
            return static_cast<float>(PinkRef) - static_cast<float>(i) / static_cast<float>(100);
        }

    } // namespace

    void GainYuleFirScalar(const sample_t* input, sample_t* s1, std::size_t count, const float* kernel) noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
            s1[i] = YuleFir(input + i, kernel);
    }

    GainFirFunction GainYuleFirFor(SimdLevel level) noexcept
    {
        switch (level) {
            case SimdLevel::Sse41: return GainYuleFirSse41;
            case SimdLevel::Avx2: return GainYuleFirAvx2;
            default: return GainYuleFirScalar;
        }
    }

    ChannelGain::~ChannelGain()
    {
        Shared::Heap::Free(sums_);
    }

    bool ChannelGain::Initialize(std::int32_t samplerate, SimdLevel simd) noexcept
    {
        // ResetSampleFrequency
        std::int32_t freqindex = 0;
        switch (samplerate) {
            case 48000: freqindex = 0; break;
            case 44100: freqindex = 1; break;
            case 32000: freqindex = 2; break;
            case 24000: freqindex = 3; break;
            case 22050: freqindex = 4; break;
            case 16000: freqindex = 5; break;
            case 12000: freqindex = 6; break;
            case 11025: freqindex = 7; break;
            case 8000: freqindex = 8; break;
            default: return false;
        }
        for (std::size_t i = 0; i < yule_.size(); ++i)
            yule_[i] = static_cast<float>(ABYule[freqindex][i]);
        for (std::size_t i = 0; i < butter_.size(); ++i)
            butter_[i] = static_cast<float>(ABButter[freqindex][i]);
        yuleFir_ = GainYuleFirFor(simd);
        // the filters start from silence (LAME clears the first MAX_ORDER values of its buffers)
        in_.fill(0);
        yuleOut_.fill(0);
        butterOut_.fill(0);
        sampleWindow_ = (samplerate * 1 + 20 - 1) / 20; // RMS_WINDOW_TIME 1/20 s
        sum_          = 0.;
        totsamp_      = 0;
        windows_      = 0;
        return true;
    }

    // AnalyzeSamples for one channel. LAME filters each piece in segments (up to the end of a window, the first
    // MAX_ORDER samples from its pre-buffer) and keeps the last MAX_ORDER samples of each filter's input and output
    // across segments, windows and calls: so each filter runs over the channel as if in one go, and only the sums
    // follow the segments. Here the filters run over the whole piece (the input part of the Yule filter with the SIMD
    // kernels, the rest serially in one loop), then the sums are taken segment by segment as in LAME.
    void ChannelGain::Analyze(const sample_t* samples, std::size_t count) noexcept
    {
        if (count == 0)
            return;
        if (count > MaxPiece) {
            failed_ = true; // LAME's pieces have at most FRAMESIZE samples
            return;
        }

        // the filters: in_ and yuleOut_ hold MaxOrder samples of history, butterOut_ two
        std::memcpy(in_.data() + MaxOrder, samples, count * sizeof(sample_t));
        yuleFir_(in_.data() + MaxOrder, s1_.data(), count, yule_.data());
        Feedback(s1_.data(), yuleOut_.data() + MaxOrder, butterOut_.data() + ButterOrder, count, yule_.data(), butter_.data());
        const sample_t* const out = butterOut_.data() + ButterOrder;

        // the sums of the squared values, segment by segment: first the remainder of 4, then groups of four
        auto         batch = static_cast<std::int64_t>(count);
        std::int64_t pos   = 0;
        while (batch > 0) {
            std::int64_t cursamples = batch > sampleWindow_ - totsamp_ ? sampleWindow_ - totsamp_ : batch;
            if (pos < MaxOrder && cursamples > MaxOrder - pos)
                cursamples = MaxOrder - pos;

            const sample_t* cur = out + pos;
            sample_t        sum = 0;
            std::int64_t    i   = cursamples & 0x03;
            while (i-- > 0) {
                const sample_t s = *cur++;
                sum += s * s;
            }
            i = cursamples / 4;
            while (i-- > 0) {
                const sample_t s0 = cur[0] * cur[0];
                const sample_t s1 = cur[1] * cur[1];
                const sample_t s2 = cur[2] * cur[2];
                const sample_t s3 = cur[3] * cur[3];
                const sample_t s  = s0 + s1 + s2 + s3;
                sum += s;
                cur += 4;
            }
            sum_ += sum;

            batch -= cursamples;
            pos += cursamples;
            totsamp_ += cursamples;
            if (totsamp_ == sampleWindow_) {
                // the window is complete: its sum goes to GainAnalysis::TitleGain; without memory for it the
                // analysis has failed, but the filters run on, so the stream can still be read to its end
                if (windows_ == capacity_) {
                    const std::size_t capacity = capacity_ == 0 ? 4096 : 2 * capacity_;
                    void* const       grown    = sums_ == nullptr ? Shared::Heap::Allocate(capacity * sizeof(double))
                                                                  : Shared::Heap::Reallocate(sums_, capacity_ * sizeof(double), capacity * sizeof(double));
                    if (grown != nullptr) {
                        sums_     = static_cast<double*>(grown);
                        capacity_ = capacity;
                    } else {
                        failed_ = true;
                    }
                }
                if (windows_ < capacity_)
                    sums_[windows_++] = sum_;
                sum_     = 0.;
                totsamp_ = 0;
            }
        }

        // the history of the filters for the next piece
        std::memmove(in_.data(), in_.data() + count, MaxOrder * sizeof(sample_t));
        std::memmove(yuleOut_.data(), yuleOut_.data() + count, MaxOrder * sizeof(sample_t));
        std::memmove(butterOut_.data(), butterOut_.data() + count, ButterOrder * sizeof(sample_t));
    }

    bool GainAnalysis::Initialize(std::int32_t samplerate, std::int32_t channels, SimdLevel simd) noexcept
    {
        channelCount_ = channels == 1 ? 1 : 2;
        for (std::int32_t ch = 0; ch < channelCount_; ++ch) {
            if (!channels_[ch].Initialize(samplerate, simd))
                return false;
        }
        return true;
    }

    void GainAnalysis::Analyze(const sample_t* left, const sample_t* right, std::size_t count) noexcept
    {
        channels_[0].Analyze(left, count);
        if (channelCount_ == 2)
            channels_[1].Analyze(right, count);
    }

    float GainAnalysis::TitleGain() noexcept
    {
        // AnalyzeSamples at the end of each window: the RMS of both channels into the histogram (one channel counts
        // twice, as LAME analyzes it as left and right)
        const ChannelGain& left  = channels_[0];
        const ChannelGain& right = channels_[channelCount_ - 1];
        if (left.Failed() || right.Failed())
            return NotEnoughSamples; // out of memory: no gain rather than a wrong one
        const std::size_t windows = left.Windows() < right.Windows() ? left.Windows() : right.Windows();
        const auto        totsamp = static_cast<double>(left.SampleWindow());
        histogram_.fill(0);
        for (std::size_t k = 0; k < windows; ++k) {
            const double lsum = left.WindowSums()[k];
            const double rsum = right.WindowSums()[k];
            const double val  = StepsPerDb * 10. * std::log10((lsum + rsum) / totsamp * 0.5 + 1.e-37);
            std::size_t  ival = val <= 0 ? 0 : static_cast<std::size_t>(val);
            if (ival >= histogram_.size())
                ival = histogram_.size() - 1;
            histogram_[ival]++;
        }
        return AnalyzeResult(histogram_.data(), histogram_.size());
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame