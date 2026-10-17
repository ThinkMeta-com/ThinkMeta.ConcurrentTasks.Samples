// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Stages.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "SessionConfig.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    void InputStage(const SessionConfig& config, const ChannelSamples& input, FrameData& frame) noexcept
    {
        frame.input[0] = input[0];
        frame.input[1] = config.channels_out == 2 ? input[1] : nullptr;

        // what lame_encode_mp3_frame initializes (pe, pe_MS); the side information is rewritten
        // by the stages, clearing it makes a frame independent of what its memory held before
        frame.pe   = {};
        frame.peMs = {};
        std::memset(&frame.side, 0, sizeof(frame.side));
        frame.outputSize    = 0;
        frame.mainDataValid = false;
    }

    void PsyPrepStage(const SessionConfig& config, const EncoderConstants& constants, FrameData& frame) noexcept
    {
        const std::int32_t channels = config.channels_out;

        // the model runs one granule ahead of the MDCT
        for (std::int32_t gr = 0; gr < 2; ++gr) {
            ChannelSamples granule;
            for (std::int32_t ch = 0; ch < channels; ++ch)
                granule[ch] = &frame.input[ch][576 + gr * 576 - FFTOFFSET];
            if (channels == 1)
                granule[1] = granule[0];
            PsyPrepare(config, constants.psy, constants.ath, granule, frame.psyPrep[gr]);
        }
    }

    void PsyStage(const SessionConfig& config, PsyStageState& state, FrameData& frame) noexcept
    {
        const std::int32_t      channels  = config.channels_out;
        const EncoderConstants& constants = *state.constants;

        // masking_lower: VBR_new_prepare sets it to this value for every frame, the first frame
        // sees LAME's initial 1
        const float maskingLower = state.firstFrame ? 1 : static_cast<float>(std::pow(10.0, config.mask_adjust * 0.1));
        state.firstFrame         = false;

        for (std::int32_t gr = 0; gr < 2; ++gr) {
            ChannelSamples granule;
            for (std::int32_t ch = 0; ch < channels; ++ch)
                granule[ch] = &frame.input[ch][576 + gr * 576 - FFTOFFSET];
            if (channels == 1)
                granule[1] = granule[0];

            std::array<std::int32_t, 2> blocktype;
            PsyResolve(config, constants.psy, state.psy, constants.ath, state.ath.factor, maskingLower, state.result, frame.psyPrep[gr], granule, gr,
                       frame.maskingLr, frame.maskingMs, frame.pe[gr], frame.peMs[gr], frame.energy[gr], blocktype);

            for (std::int32_t ch = 0; ch < channels; ++ch) {
                frame.side.tt[gr][ch].block_type       = blocktype[ch];
                frame.side.tt[gr][ch].mixed_block_flag = 0;
            }
        }

        // automatic ATH adjustment, useful for low volume; the quantization of this frame uses it
        AdjustAth(config, constants.ath, state.ath, state.result);
        frame.athAdjustFactor = state.ath.factor;
    }

    void MdctPolyphaseStage(const SessionConfig& config, const EncoderConstants& constants, FrameData& frame) noexcept
    {
        for (std::int32_t ch = 0; ch < config.channels_out; ++ch)
            MdctPolyphaseFrame(frame.input[ch], frame.polyphase[ch], constants.windowSubband);
    }

    void MdctCarryStage(const SessionConfig& config, MdctStageState& state, FrameData& frame) noexcept
    {
        for (std::int32_t ch = 0; ch < config.channels_out; ++ch) {
            if (frame.frameNumber == 0)
                MdctPrimeCarry(config.amp_filter, frame.input[ch], frame.mdctCarry[ch]);
            else
                frame.mdctCarry[ch] = state.carry[ch];
            MdctCarry(config.amp_filter, frame.polyphase[ch], state.carry[ch]);
        }
    }

    void MdctStage(const SessionConfig& config, FrameData& frame) noexcept
    {
        const std::int32_t channels = config.channels_out;

        for (std::int32_t ch = 0; ch < channels; ++ch) {
            const std::array<std::int32_t, 2> blockType = {frame.side.tt[0][ch].block_type, frame.side.tt[1][ch].block_type};
            FrameLines                        xr;
            MdctFromPolyphase(config.amp_filter, frame.mdctCarry[ch], frame.polyphase[ch], blockType, xr);
            frame.side.tt[0][ch].xr = xr[0];
            frame.side.tt[1][ch].xr = xr[1];
        }
    }

    void StereoStage(const SessionConfig& config, FrameData& frame) noexcept
    {
        frame.modeExt = MPG_MD_LR_LR;
        if (config.force_ms) {
            frame.modeExt = MPG_MD_MS_LR;
        } else if (config.mode == JointStereo) {
            // M/S if it would not use much more bits than L/R (by PE) and both channels have the
            // same block types
            float sumPeMs = 0;
            float sumPeLr = 0;
            for (std::int32_t gr = 0; gr < 2; ++gr) {
                for (std::int32_t ch = 0; ch < config.channels_out; ++ch) {
                    sumPeMs += frame.peMs[gr][ch];
                    sumPeLr += frame.pe[gr][ch];
                }
            }
            if (sumPeMs <= 1.00 * sumPeLr) {
                const auto& gi0 = frame.side.tt[0];
                const auto& gi1 = frame.side.tt[1];
                if (gi0[0].block_type == gi0[1].block_type && gi1[0].block_type == gi1[1].block_type)
                    frame.modeExt = MPG_MD_MS_LR;
            }
        }
    }

    void QuantSearchStage(const SessionConfig& config, const EncoderConstants& constants, FrameData& frame) noexcept
    {
        QuantSearch(config, constants.quantize, constants.ath, constants.psy.decay, frame);
        PrepareMainData(config, frame);
    }

    bool QuantizeStage(const SessionConfig& config, QuantizeStageState& state, FrameData& frame) noexcept
    {
        return QuantCommit(config, state.constants->quantize, state.quantize, frame);
    }

    namespace {

        // updateStats: histograms of bitrates, stereo modes and block types
        void UpdateStats(const SessionConfig& config, EncoderResult& r, const FrameData& frame) noexcept
        {
            r.bitrate_channelmode_hist[r.bitrate_index][4]++;
            r.bitrate_channelmode_hist[15][4]++;
            if (config.channels_out == 2) {
                r.bitrate_channelmode_hist[r.bitrate_index][r.mode_ext]++;
                r.bitrate_channelmode_hist[15][r.mode_ext]++;
            }
            for (std::int32_t gr = 0; gr < 2; ++gr) {
                for (std::int32_t ch = 0; ch < config.channels_out; ++ch) {
                    const GrInfo&      gi = frame.side.tt[gr][ch];
                    const std::int32_t bt = gi.mixed_block_flag ? 4 : gi.block_type;
                    r.bitrate_blocktype_hist[r.bitrate_index][bt]++;
                    r.bitrate_blocktype_hist[r.bitrate_index][5]++;
                    r.bitrate_blocktype_hist[15][bt]++;
                    r.bitrate_blocktype_hist[15][5]++;
                }
            }
        }

    } // namespace

    void BitstreamStage(const SessionConfig& config, BitstreamStageState& state, FrameData& frame) noexcept
    {
        FormatBitstream(config, state.bitstream, *state.reservoir, frame);

        // copy_buffer: the complete bytes, counted for the music CRC and the stream size
        frame.outputSize = BitstreamCopy(state.bitstream, frame.output.data(), MaxFrameBytes);
        if (frame.outputSize > 0)
            AddMusicBytes(state.tag, frame.output.data(), frame.outputSize);

        if (config.write_lame_tag)
            AddVbrFrame(state.tag, frame.bitrateIndex);

        state.result.bitrate_index = frame.bitrateIndex;
        state.result.mode_ext      = frame.modeExt;
        ++state.result.frame_number;
        UpdateStats(config, state.result, frame);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
