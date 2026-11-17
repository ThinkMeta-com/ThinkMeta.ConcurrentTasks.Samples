// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "SharedSampleStream.h"

#include <cstddef>
#include <cstdint>

#include "SessionConfig.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    bool SharedSampleStream::Initialize(const SessionConfig& config, std::uint64_t maxInputSamples, ReplayGainBy replayGainBy, SimdLevel simd) noexcept
    {
        const bool gainTasks = replayGainBy == ReplayGainBy::Tasks && config.options.findReplayGain != 0;
        if (!stream_.Initialize(config, maxInputSamples, gainTasks, simd))
            return false;
        readers_ = gainTasks ? stream_.Gain()->Channels() : 0;
        pending_.store(1 + readers_, std::memory_order_relaxed);
        return true;
    }

    // --- the writer

    void SharedSampleStream::AddSamples(const std::int32_t* left, const std::int32_t* right, std::int32_t count)
    {
        stream_.AddInt(left, right, count);
        WakeGainReaders(false);
    }

    void SharedSampleStream::End() noexcept
    {
        if (writerEnded_)
            return;
        writerEnded_ = true;
        stream_.Finish();
        WakeGainReaders(true);
        ParticipantEnded();
    }

    // --- the gain readers

    SharedSampleStream::GainReader::GainReader(SharedSampleStream& owner, std::int32_t channel) noexcept
        : owner_(&owner),
          channel_(channel),
          position_(SampleStream::GainStart())
    {}

    bool SharedSampleStream::GainReader::NextPiece(Piece& piece) noexcept
    {
        // the stream frees 64 KB at once anyway, and what a reader has analyzed shares its cache line with the task that
        // frees: so a reader tells it only every few pieces
        constexpr std::int32_t ReportBatch = 16;

        SampleStream& stream = owner_->stream_;
        if (taken_ != 0) { // the piece taken last is analyzed
            position_ += taken_;
            taken_ = 0;
            if (++unreported_ == ReportBatch) {
                unreported_ = 0;
                stream.GainProgress(channel_, position_);
            }
        }
        for (;;) {
            const std::int32_t count = stream.GainPiece(position_);
            if (count > 0) {
                piece.samples = stream.SamplesOf(channel_, position_);
                piece.count   = static_cast<std::size_t>(count);
                taken_        = piece.count;
                return true;
            }
            stream.GainProgress(channel_, position_);
            unreported_ = 0;
            if (count < 0)
                break;
            // a Set before this wait leaves the event set, so no wake-up is lost
            const ProfileScope idle(owner_->gainIdle_);
            owner_->wake_[channel_].Wait();
        }
        owner_->ParticipantEnded();
        return false;
    }

    // --- Encode

    SharedSampleStream::StreamEnd SharedSampleStream::WaitForEnd() noexcept
    {
        while (pending_.load(std::memory_order_acquire) > 0) {
            const ProfileScope idle(encodeIdle_);
            ended_.Wait();
        }
        if (readers_ > 0)
            stream_.FinishGain(); // joins what the readers found; without readers the stream has done it in End

        StreamEnd end;
        end.encoderPadding = stream_.EncoderPadding();
        end.radioGain      = stream_.RadioGain();
        end.failed         = stream_.Failed();
        return end;
    }

    // --- private

    void SharedSampleStream::WakeGainReaders(bool ended) noexcept
    {
        if (readers_ == 0)
            return;
        // WakeBatch new pieces of FRAMESIZE samples, or the end with the last pieces: a piece is only about 25 us of
        // work, a wake-up per piece would cost more than the analysis
        const std::uint64_t pieces = ended ? ~0ull : stream_.GainAvailable() / FRAMESIZE;
        if (pieces < signaled_ + WakeBatch)
            return;
        signaled_ = pieces;
        for (std::int32_t i = 0; i < readers_; ++i)
            wake_[i].Set();
    }

    // The writer or a gain reader has ended; the last of them wakes Encode.
    void SharedSampleStream::ParticipantEnded() noexcept
    {
        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            ended_.Set();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
