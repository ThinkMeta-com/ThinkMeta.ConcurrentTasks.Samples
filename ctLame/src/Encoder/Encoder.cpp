// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Encoder.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "SessionConfig.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        // LAME allocates its state with calloc
        template <typename T>
        void Zero(T& value) noexcept
        {
            static_assert(std::is_trivially_copyable_v<T>);
            std::memset(&value, 0, sizeof(T));
        }

        // the counter of a stage, null while profiling is off
        std::atomic<std::int64_t>* Counter(StageProfile* profile, ProfileStage stage) noexcept
        {
            return profile != nullptr ? profile->Counter(stage) : nullptr;
        }

    } // namespace

    void Encoder::Initialize(SessionConfig& config, SimdLevel simd) noexcept
    {
        config_ = &config;

        Zero(constants_);
        Zero(psy_);
        Zero(mdct_);
        Zero(quantize_);
        Zero(bitstream_);
        Zero(frame_);
        frames_                         = 0;
        psy_.constants                  = &constants_;
        psy_.firstFrame                 = true;
        quantize_.constants             = &constants_;
        bitstream_.reservoir            = &quantize_.quantize.reservoir;
        bitstream_.result.encoder_delay = ENCDELAY;

        // init_bit_stream_w and lame_init_bitstream
        BitstreamInit(bitstream_.bitstream);
        if (config.write_lame_tag)
            InitVbrTag(config, bitstream_.tag, bitstream_.bitstream);

        // iteration_init (main_data_begin starts at 0) and psymodel_init
        QuantizeInit(config, constants_.quantize, constants_.ath, quantize_.quantize);
        (void)PsyInit(config, constants_.psy, psy_.psy, constants_.ath, psy_.ath);

        // the SIMD kernels give the same bits as the scalar ones
        constants_.psy.fft.fht      = FhtFor(simd);
        constants_.psy.attackPrep   = AttackPrepFor(simd);
        constants_.psy.maskChains   = MaskChainsFor(simd);
        constants_.psy.maskingLong  = MaskingLongFor(simd);
        constants_.quantize.kernels = QuantizeKernelsFor(simd);
        constants_.windowSubband    = MdctKernels::WindowSubbandFor(simd);
    }

    std::int32_t Encoder::TakeHeaderBytes(std::byte* buffer, std::int32_t size) noexcept
    {
        return BitstreamCopy(bitstream_.bitstream, buffer, size);
    }

    void Encoder::Prepare(const ChannelSamples& input, FrameData& frame) const noexcept
    {
        const SessionConfig& config = *config_;

        {
            ProfileScope time(Counter(profile_, ProfileStage::Input));
            InputStage(config, input, frame);
        }
        {
            ProfileScope time(Counter(profile_, ProfileStage::Psy));
            PsyPrepStage(config, constants_, frame);
        }
        {
            ProfileScope time(Counter(profile_, ProfileStage::Mdct));
            MdctPolyphaseStage(config, constants_, frame);
        }
    }

    void Encoder::Resolve(FrameData& frame) noexcept
    {
        ProfileScope time(Counter(profile_, ProfileStage::PsyResolve));
        PsyStage(*config_, psy_, frame);
        MdctCarryStage(*config_, mdct_, frame);
    }

    void Encoder::Search(FrameData& frame) const noexcept
    {
        const SessionConfig& config = *config_;

        {
            ProfileScope time(Counter(profile_, ProfileStage::Mdct));
            MdctStage(config, frame);
        }
        {
            ProfileScope time(Counter(profile_, ProfileStage::Stereo));
            StereoStage(config, frame);
        }
        {
            ProfileScope time(Counter(profile_, ProfileStage::Search));
            QuantSearchStage(config, constants_, frame);
        }
    }

    bool Encoder::Encode(FrameData& frame) noexcept
    {
        const SessionConfig& config = *config_;

        {
            ProfileScope time(Counter(profile_, ProfileStage::Quantize));
            if (!QuantizeStage(config, quantize_, frame))
                return false;
        }
        {
            ProfileScope time(Counter(profile_, ProfileStage::Bitstream));
            BitstreamStage(config, bitstream_, frame);
        }
        if (profile_ != nullptr)
            profile_->frames.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::int32_t Encoder::RunAllStages(const ChannelSamples& input, std::byte* buffer, std::int32_t size) noexcept
    {
        frame_.frameNumber = frames_++;
        Prepare(input, frame_);
        Resolve(frame_);
        Search(frame_);
        if (!Encode(frame_))
            return -2;

        if (frame_.outputSize < 0 || frame_.outputSize > size)
            return -1;
        std::memcpy(buffer, frame_.output.data(), static_cast<std::size_t>(frame_.outputSize));
        return frame_.outputSize;
    }

    std::int32_t Encoder::Flush(std::byte* buffer, std::int32_t size) noexcept
    {
        FlushBitstream(*config_, bitstream_.bitstream, *bitstream_.reservoir, bitstream_.result.bitrate_index);
        const std::int32_t bytes = BitstreamCopy(bitstream_.bitstream, buffer, size);
        if (bytes > 0)
            AddMusicBytes(bitstream_.tag, buffer, bytes); // music CRC and stream size for the LAME tag
        return bytes;
    }

    std::size_t Encoder::GetLameTagFrame(std::byte* buffer, std::size_t size) const noexcept
    {
        return Lame::GetLameTagFrame(*config_, bitstream_.tag, bitstream_.result, buffer, size);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
