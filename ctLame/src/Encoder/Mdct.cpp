// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Mdct.h"

#include <array>
#include <cstdint>

#include <Windows.h>

#include "MdctKernels.h"

// Port of newmdct.c (mdct_sub48 and the priming of lame_encode_frame_init). The constant tables
// are LAME's data (MdctTables.inc); the transform kernels are in MdctKernels.cpp.

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    namespace {

        // the window lengths of newmdct.c: a short block spans 12 subband samples, a long one 36
        // (MdctTables.inc sizes the windows with NL)
        constexpr std::int32_t NS = 12;
        constexpr std::int32_t NL = 36;

        // the subband samples of one subband in a granule: 18 x SBLIMIT = 576 values
        constexpr std::int32_t SamplesPerSubband = 18;

        // the polyphase window of a subband sample reaches 286 samples back, so the filterbank
        // of a frame starts 286 samples into its input (w0 + 286 in mdct_sub48)
        constexpr std::int32_t PolyphaseLookBack = 286;

#pragma warning(push)
#pragma warning(disable : 4305) // truncation from 'double' to 'float': the table literals are doubles, as in LAME

#include "MdctTables.inc"

#pragma warning(pop)

        // LAME keeps four more tables in the tail of win[SHORT_TYPE], whose short window uses
        // only the first 3 values: tantab_l, cx, ca and cs (the names of newmdct.c's macros)

        // tantab_l: the factors of the long block folding
        inline const float* TanTabLong() noexcept
        {
            return win[SHORT_TYPE].data() + 3;
        }

        // cx: the cosine factors of the long MDCT
        inline const float* CosLong() noexcept
        {
            return win[SHORT_TYPE].data() + 12;
        }

        // ca and cs: the factors of the aliasing reduction butterfly
        inline const float* AliasCa() noexcept
        {
            return win[SHORT_TYPE].data() + 20;
        }

        inline const float* AliasCs() noexcept
        {
            return win[SHORT_TYPE].data() + 28;
        }

        // the polyphase filterbank of one granule (the window_subband loop of mdct_sub48): 18
        // subband samples of the 32 bands from wk on; advances wk by 576 samples
        void PolyphaseGranule(const sample_t*& wk, float* samp, MdctKernels::WindowSubbandFunction windowSubband = MdctKernels::WindowSubband) noexcept
        {
            // two subband samples per pass
            for (std::int32_t k = 0; k < SamplesPerSubband / 2; k++) {
                windowSubband(enwindow.data(), wk, samp);
                windowSubband(enwindow.data(), wk + SBLIMIT, samp + SBLIMIT);
                samp += 2 * SBLIMIT;
                wk += 2 * SBLIMIT;
                // compensate for inversion in the analysis filter
                for (std::int32_t band = 1; band < SBLIMIT; band += 2)
                    samp[band - SBLIMIT] *= -1;
            }
        }

    } // namespace

    namespace {

        // mdct_sub48, the body of its channel loop; the polyphase output comes from the input or,
        // with polyphase set, was computed ahead. The band loop is kept close to LAME on purpose:
        // its index arithmetic is the definition of the folding, a restructured version would
        // only be harder to compare.
        void MdctChannelFrom(const AmpFilter& ampFilter, MdctChannelState& state, const sample_t* input, const MdctPolyphase* polyphase,
                             const std::array<std::int32_t, 2>& blockType, FrameLines& xr) noexcept
        {
            std::int32_t       gr, k;
            const sample_t*    wk       = input != nullptr ? input + PolyphaseLookBack : nullptr;
            const float* const tantab_l = TanTabLong();
            const float* const ca       = AliasCa();
            const float* const cs       = AliasCs();

            for (gr = 0; gr < 2; gr++) {
                std::int32_t band;
                float*       mdct_enc = xr[gr].data();

                if (polyphase != nullptr)
                    state.sb_sample[1 - gr] = polyphase->sb[gr];
                else
                    PolyphaseGranule(wk, state.sb_sample[1 - gr][0].data());

                // perform imdct of 18 previous subband samples + 18 current subband samples
                for (band = 0; band < SBLIMIT; band++, mdct_enc += SamplesPerSubband) {
                    const std::int32_t type  = blockType[gr]; // mixed blocks are never used
                    const float* const band0 = state.sb_sample[gr][0].data() + order[band];
                    float* const       band1 = state.sb_sample[1 - gr][0].data() + order[band];
                    if (ampFilter[band] < 1e-12) {
                        ZeroMemory(mdct_enc, SamplesPerSubband * sizeof(float));
                    } else {
                        if (ampFilter[band] < 1.0) {
                            for (k = 0; k < SamplesPerSubband; k++)
                                band1[k * SBLIMIT] *= ampFilter[band];
                        }
                        if (type == SHORT_TYPE) {
                            for (k = -NS / 4; k < 0; k++) {
                                const float w        = win[SHORT_TYPE][k + 3];
                                mdct_enc[k * 3 + 9]  = band0[(9 + k) * SBLIMIT] * w - band0[(8 - k) * SBLIMIT];
                                mdct_enc[k * 3 + 18] = band0[(14 - k) * SBLIMIT] * w + band0[(15 + k) * SBLIMIT];
                                mdct_enc[k * 3 + 10] = band0[(15 + k) * SBLIMIT] * w - band0[(14 - k) * SBLIMIT];
                                mdct_enc[k * 3 + 19] = band1[(2 - k) * SBLIMIT] * w + band1[(3 + k) * SBLIMIT];
                                mdct_enc[k * 3 + 11] = band1[(3 + k) * SBLIMIT] * w - band1[(2 - k) * SBLIMIT];
                                mdct_enc[k * 3 + 20] = band1[(8 - k) * SBLIMIT] * w + band1[(9 + k) * SBLIMIT];
                            }
                            MdctKernels::MdctShort(win[SHORT_TYPE].data(), mdct_enc);
                        } else {
                            std::array<float, SamplesPerSubband> work;
                            for (k = -NL / 4; k < 0; k++) {
                                float a, b;
                                a            = win[type][k + 27] * band1[(k + 9) * SBLIMIT] + win[type][k + 36] * band1[(8 - k) * SBLIMIT];
                                b            = win[type][k + 9] * band0[(k + 9) * SBLIMIT] - win[type][k + 18] * band0[(8 - k) * SBLIMIT];
                                work[k + 9]  = a - b * tantab_l[k + 9];
                                work[k + 18] = a * tantab_l[k + 9] + b;
                            }

                            MdctKernels::MdctLong(CosLong(), mdct_enc, work.data());
                        }
                    }
                    // perform aliasing reduction butterfly
                    if (type != SHORT_TYPE && band != 0) {
                        for (k = 7; k >= 0; --k) {
                            float bu, bd;
                            bu = mdct_enc[k] * ca[k] + mdct_enc[-1 - k] * cs[k];
                            bd = mdct_enc[k] * cs[k] - mdct_enc[-1 - k] * ca[k];

                            mdct_enc[-1 - k] = bu;
                            mdct_enc[k]      = bd;
                        }
                    }
                }
            }
        }

    } // namespace

    const float* MdctAnalysisWindow() noexcept
    {
        return enwindow.data();
    }

    void MdctChannel(const AmpFilter& ampFilter, MdctChannelState& state, const sample_t* input, const std::array<std::int32_t, 2>& blockType,
                     FrameLines& xr) noexcept
    {
        MdctChannelFrom(ampFilter, state, input, nullptr, blockType, xr);
    }

    void MdctPolyphaseFrame(const sample_t* input, MdctPolyphase& out, MdctKernels::WindowSubbandFunction windowSubband) noexcept
    {
        const sample_t* wk = input + PolyphaseLookBack;
        PolyphaseGranule(wk, out.sb[0][0].data(), windowSubband);
        PolyphaseGranule(wk, out.sb[1][0].data(), windowSubband);
    }

    void MdctCarry(const AmpFilter& ampFilter, const MdctPolyphase& frame, SubbandSamples& carry) noexcept
    {
        carry = frame.sb[1];
        for (std::int32_t band = 0; band < SBLIMIT; band++) {
            float* const band1 = carry[0].data() + order[band];
            if (ampFilter[band] >= 1e-12 && ampFilter[band] < 1.0) {
                for (std::int32_t k = 0; k < SamplesPerSubband; k++)
                    band1[k * SBLIMIT] *= ampFilter[band];
            }
        }
    }

    void MdctPrimeCarry(const AmpFilter& ampFilter, const sample_t* input, SubbandSamples& carry) noexcept
    {
        MdctChannelState state;
        MdctPrime(ampFilter, state, input);
        carry = state.sb_sample[0];
    }

    void MdctFromPolyphase(const AmpFilter& ampFilter, const SubbandSamples& carry, const MdctPolyphase& frame, const std::array<std::int32_t, 2>& blockType,
                           FrameLines& xr) noexcept
    {
        MdctChannelState state;
        state.sb_sample[0] = carry;
        MdctChannelFrom(ampFilter, state, nullptr, &frame, blockType, xr);
    }

    // LAME: lame_encode_frame_init (encoder.c), the priming of the MDCT/polyphase filterbank with a short block
    void MdctPrime(const AmpFilter& ampFilter, MdctChannelState& state, const sample_t* input) noexcept
    {
        // the look-back, a frame of zeros and the first granule of the input (mode_gr = 2)
        constexpr std::int32_t          PrimeSize = PolyphaseLookBack + FRAMESIZE + GranuleSize;
        std::array<sample_t, PrimeSize> primebuff = {};
        for (std::int32_t i = 0, j = 0; i < PrimeSize; ++i) {
            if (i < FRAMESIZE)
                primebuff[i] = 0;
            else
                primebuff[i] = input[j++];
        }
        const std::array<std::int32_t, 2> shortBlocks = {SHORT_TYPE, SHORT_TYPE};
        FrameLines                        xr;
        MdctChannel(ampFilter, state, primebuff.data(), shortBlocks, xr);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
