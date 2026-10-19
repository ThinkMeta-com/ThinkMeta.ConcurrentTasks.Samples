//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#include "ChunkedDeflate.h"

#include "BlockRules.h"
#include "Checksums.h"
#include "Compressor.h"
#include "Gzip.h"

#include <memory>

// New in ctDeflate, not in libdeflate (see ChunkedDeflate.h).

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    namespace {

        class StoredLogs final : public IChunkLogs
        {
        public:
            explicit StoredLogs(const std::vector<PathLog>& logs) noexcept : logs_(logs) {}
            const PathLog& AwaitLog(std::size_t chunk) override { return logs_[chunk]; }

        private:
            const std::vector<PathLog>& logs_;
        };

        class CollectBlocks final : public IBlockSink
        {
        public:
            void                       OnBlock(ResolvedBlock&& block) override { blocks.push_back(std::move(block)); }
            std::vector<ResolvedBlock> blocks;
        };

    } // namespace

    std::vector<std::size_t> PlanChunks(std::size_t inSize, std::size_t chunkSize)
    {
        constexpr std::size_t    MinLastChunk = 1024;
        std::vector<std::size_t> starts{0};
        if (chunkSize == 0)
            return starts;
        for (std::size_t s = chunkSize; s + MinLastChunk <= inSize; s += chunkSize)
            starts.push_back(s);
        return starts;
    }

    std::uint32_t ChunkMinLens(const LevelParams& params, const std::uint8_t* in, std::size_t inSize, std::size_t start, std::uint32_t recent,
                               bool scheduleOnly) noexcept
    {
        if (params.mode == ParseMode::Fastest)
            return AllMinLens;
        if (scheduleOnly)
            return ScheduleBit;
        const std::uint32_t depth    = params.maxSearchDepth;
        const std::uint32_t cap      = depth < 5 ? 4 : depth < 10 ? 5 : depth < 16 ? 7 : 9;
        std::uint32_t       possible = 0;
        for (std::uint32_t m = MinMatchLen; m <= cap; ++m)
            possible |= MinLenBit(m);
        // The schedule path follows the block logic from the chunk start; the constant ones what the last blocks used and,
        // as the recalculation within a block tends to raise it, the 4 KiB estimate and the next higher value.
        const std::uint32_t estimate = CalculateMinMatchLen(in + start, inSize - start, depth);
        return ScheduleBit | ((recent | MinLenBit(estimate) | MinLenBit(estimate + 1)) & possible);
    }

    void ChunkParser::Init(const LevelParams& params, const std::uint8_t* in, std::size_t inSize)
    {
        params_ = params;
        in_     = in;
        inSize_ = inSize;
        parser_.Init(params, in, inSize);
        if (params.mode != ParseMode::Fastest)
            multi_.Init(params, in, inSize);
    }

    void ChunkParser::Parse(const std::vector<std::size_t>& starts, std::size_t k, std::size_t overlap, std::uint32_t minLens, PathLog& out)
    {
        const std::size_t begin = starts[k];
        std::size_t       limit = k + 1 < starts.size() ? starts[k + 1] + overlap : inSize_;
        if (limit > inSize_)
            limit = inSize_;
        if (params_.mode == ParseMode::Fastest) {
            out.Reset(begin, AllMinLens);
            parser_.StartSpeculative(begin);
            parser_.Run(limit, out.lanes[0]);
            out.Finish();
        } else if (minLens == ScheduleBit) {
            // Only the schedule path: the single-path parser runs it faster (same iterations, same minimum match length
            // events), and stops where the MultiParser's paths stop.
            out.Reset(begin, ScheduleBit);
            const std::size_t tailStop = inSize_ > PathTailStop ? inSize_ - PathTailStop : 0;
            if (limit > tailStop)
                limit = tailStop;
            if (begin < limit) {
                StepLog& lane = out.lanes[ScheduleLane];
                lane.steps.reserve((limit - begin) / 5 + 64);
                parser_.StartSpeculative(begin);
                parser_.Run(limit, lane);
            }
            out.Finish();
        } else {
            multi_.Run(begin, limit, minLens, out);
        }
    }

    void StitchBlock(BitWriter& os, const std::uint8_t* in, const ResolvedBlock& block, const EncodedBlock& encoded) noexcept
    {
        if ((encoded.storedMask >> os.BitCount()) & 1)
            BlockWriter::WriteStored(os, in + block.begin, block.length, block.isFinal);
        else
            BlockWriter::AppendBits(os, encoded.bits.data(), encoded.bitCount);
    }

    std::size_t CompressGzipChunked(std::int32_t level, const std::uint8_t* in, std::size_t inSize, std::uint8_t* out, const ChunkOptions& options,
                                    ResolveStats* stats)
    {
        const LevelParams params = LevelParams::For(level);
        if (params.mode == ParseMode::None || inSize <= params.maxPassthroughSize)
            return Compressor::Create(level)->CompressGzip(in, inSize, out);

        const auto           starts = PlanChunks(inSize, options.chunkSize);
        std::vector<PathLog> logs(starts.size());
        auto                 parser = std::make_unique<ChunkParser>();
        parser->Init(params, in, inSize);
        for (std::size_t k = 0; k < starts.size(); ++k) {
            parser->Parse(starts, k, options.overlap, ChunkMinLens(params, in, inSize, starts[k], 0), logs[k]);
        }

        StoredLogs    storedLogs(logs);
        CollectBlocks sink;
        Resolver      resolver(params, in, inSize);
        const auto    resolveStats = resolver.Run(starts, storedLogs, sink);
        if (stats != nullptr)
            *stats = resolveStats;

        std::size_t                  pos = WriteGzipHeader(level, out);
        BitWriter                    os(out + pos, out + Compressor::GzipBound(inSize));
        auto                         writer = std::make_unique<BlockWriter>();
        EncodedBlock                 encoded;
        std::vector<DeflateSequence> sequences;
        DeflateFreqs                 freqs;
        for (auto& block : sink.blocks) {
            BuildBlockSymbols(in, block, sequences, freqs);
            writer->Encode(in + block.begin, static_cast<std::uint32_t>(block.length), sequences.data(), freqs, block.isFinal, encoded);
            StitchBlock(os, in, block, encoded);
        }
        os.AlignToByte();
        pos = static_cast<std::size_t>(os.Next() - out);
        pos += WriteGzipTrailer(Crc32(0, in, inSize), inSize, out + pos);
        return pos;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
