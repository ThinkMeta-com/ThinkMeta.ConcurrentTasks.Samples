//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "BlockWriter.h"
#include "Checksums.h"
#include "ChunkedDeflate.h"
#include "Compressor.h"
#include "Gzip.h"
#include "Resolver.h"

// headers with imports last
#include "BlockRing.h"
#include "ChunkLogs.h"
#include "InputWindow.h"
#include "OutputWindow.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Windows.Error;

// The nodes of DeflateGraph (DeflateGraph.h). Each class names the node it is.
namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    using DeflateClock = std::chrono::steady_clock;

    /// <summary>How far each stage reads before the oldest position it still works on (DeflateGraph and the nodes).</summary>
    struct InputReach {
        static constexpr std::size_t MatchWindow = MaxMatchOffset; // DEFLATE's match distance: the matchfinder looks this far back
        static constexpr std::size_t Parse       = 65536; // a parse may read this far past its limit (a match crossing it)
    };

    /// <summary>
    /// Node Stitch: appends the encoded blocks to the bitstream in order, hands the final output to Write and frees what
    /// no stage needs any more (logs, input pages). It has no task of its own: the Encode that completes a block runs it,
    /// one at a time (the stitch flag); the others go on encoding.
    /// </summary>
    class Stitch
    {
    public:
        /// <summary>Input kept before the oldest position still in use: the matchfinder window and a parse's reach.</summary>
        static constexpr std::size_t KeepBehind = InputReach::MatchWindow + InputReach::Parse;

        /// <summary>Output committed past a block's worst case: the block header and the bits still in the writer.</summary>
        static constexpr std::size_t CommitSlack = 1024;

        Stitch(InputWindow& input, ChunkLogs& logs, BlockRing& ring, OutputWindow& output, BitWriter& os) noexcept
            : input_(input),
              logs_(logs),
              ring_(ring),
              output_(output),
              os_(os)
        {}

        /// <summary>Input position below which every block is in the bitstream (any thread).</summary>
        std::size_t AppendedTo() const noexcept { return appendedTo_.load(std::memory_order_acquire); }

        /// <summary>Appends the encoded blocks that are next in order; one caller at a time, the others return.</summary>
        void TryStitch() noexcept
        {
            const std::uint8_t* in  = input_.Data();
            std::uint8_t*       out = output_.Data();
            for (;;) {
                if (busy_.exchange(true, std::memory_order_seq_cst))
                    return;
                std::size_t       s     = ring_.Stitched();
                const std::size_t first = s;
                while (ring_.IsStitchReady(s)) {
                    ResolvedBlock& block = ring_.StitchBlock(s);
                    output_.Commit(static_cast<std::size_t>(os_.Next() - out) + ring_.Encoded(s).bits.size() + block.length + CommitSlack);
                    StitchBlock(os_, in, block, ring_.Encoded(s));
                    stitchedTo_ = block.begin + block.length;
                    ring_.CompleteStitch(s);
                    ++s;
                }
                if (s != first) {
                    appendedTo_.store(stitchedTo_, std::memory_order_release);
                    ring_.SignalRoom();
                    ReleaseBehind();
                }
                busy_.store(false, std::memory_order_seq_cst);
                // A block may have become ready while the flag was held by this caller: the Encode that completed it saw
                // the flag set and returned, so check again (seq_cst on both sides, no wake-up is lost).
                if (!ring_.NextStitchReady())
                    return;
            }
        }

    private:
        void ReleaseBehind() noexcept
        {
            // A log is done once Resolve left it and the bitstream is past every piece taken from it.
            logs_.FreeBelow(stitchedTo_);
            if (input_.Streaming()) {
                // Resolve (re-parses, block statistics) and Encode work at or after stitchedTo; a parse reads its
                // matchfinder window before the chunk start.
                std::size_t       low     = (std::min)(stitchedTo_, logs_.OldestParseStart());
                const std::size_t overlap = logs_.Overlap();
                low                       = low > overlap + KeepBehind ? low - overlap - KeepBehind : 0;
                input_.ReleaseBelow(low);
            }
            output_.CompleteProduce(static_cast<std::size_t>(os_.Next() - output_.Data()));
        }

        InputWindow&      input_;
        ChunkLogs&        logs_;
        BlockRing&        ring_;
        OutputWindow&     output_;
        BitWriter&        os_; // the fields below and os belong to the flag holder
        std::atomic<bool>        busy_{false};
        std::size_t              stitchedTo_ = 0; // input position the bitstream covers
        std::atomic<std::size_t> appendedTo_{0};  // the same, published for the resolver
    };

    /// <summary>The work of node Encode, with the buffers of one task: encodes the next published block, then stitches.</summary>
    class EncodeWork
    {
    public:
        EncodeWork(const std::uint8_t* in, BlockRing& ring, Stitch& stitch) : in_(in), ring_(ring), stitch_(stitch), writer_(std::make_unique<BlockWriter>()) {}

        /// <summary>Encodes the next published block, if any is unclaimed.</summary>
        bool TryEncode() noexcept
        {
            std::size_t i;
            if (!ring_.TryAcquireEncode(i))
                return false;
            const ResolvedBlock& block = ring_.Block(i);
            BuildBlockSymbols(in_, block, sequences_, freqs_);
            writer_->Encode(in_ + block.begin, static_cast<std::uint32_t>(block.length), sequences_.data(), freqs_, block.isFinal, ring_.Encoded(i));
            ring_.CompleteEncode(i);
            stitch_.TryStitch();
            ring_.SignalEncoded();
            return true;
        }

    private:
        const std::uint8_t*          in_;
        BlockRing&                   ring_;
        Stitch&                      stitch_;
        std::unique_ptr<BlockWriter> writer_;
        std::vector<DeflateSequence> sequences_;
        DeflateFreqs                 freqs_;
    };

    /// <summary>
    /// Node Read: reads the input with overlapped AsyncFile reads (Depth segments in flight) up to the end Resolve allows,
    /// and marks the chunks whose input is loaded.
    /// </summary>
    class ReadTask : public Tasks::Task
    {
    public:
        static constexpr std::size_t Block = SegmentSize; // bytes per read
        static constexpr std::size_t Depth = 4;           // reads in flight

        explicit ReadTask(InputWindow& input) noexcept : input_(input) {}

        void Run() noexcept override
        {
            using Windows::Error;
            std::uint8_t*     data         = input_.Writable();
            const std::size_t size         = input_.Size();
            std::size_t       issuedTo     = 0;
            std::size_t       loaded       = 0;
            std::uint64_t     issued       = 0;
            std::uint64_t     completed    = 0;
            std::size_t       committed    = 0;
            std::size_t       bytes[Depth] = {};
            std::uint32_t     error        = Error::Success;

            while (loaded < size) {
                const std::size_t allowed = input_.ReadAllowance();
                while (issued - completed < Depth && issuedTo < size && issuedTo < allowed) {
                    const std::size_t slot = issued % Depth;
                    auto&             req  = requests_[slot];
                    bytes[slot]            = (std::min)(Block, size - issuedTo);
                    req.Reset();
                    req.SetOffset(issuedTo);
                    std::uint32_t result = Error::Success;
                    for (; committed < issuedTo + bytes[slot] && Error::Succeeded(result); committed += SegmentSize)
                        if (!input_.CommitSegment(committed))
                            result = Error::GetLastError();
                    if (Error::Succeeded(result))
                        result = Error::Succeeded(error) ? input_.File().ReadAsync(data + issuedTo, static_cast<unsigned long>(bytes[slot]), req) : error;
                    if (Error::Failed(result) && !Error::IsPending(result))
                        req.RequestCompleted(result, 0);
                    issuedTo += bytes[slot];
                    ++issued;
                }
                if (completed == issued) {
                    input_.AwaitAllowance();
                    continue;
                }
                const std::size_t slot = completed % Depth;
                auto&             req  = requests_[slot];
                req.Wait();
                if (Error::Succeeded(error) && (Error::Failed(req.GetError()) || req.GetTransferredBytes() != bytes[slot])) {
                    error = Error::Failed(req.GetError()) ? req.GetError() : ERROR_HANDLE_EOF;
                    input_.SetReadError(error);
                }
                loaded += bytes[slot];
                ++completed;
                input_.CompleteRead(loaded);
            }
        }

    private:
        InputWindow&                          input_;
        AsyncIo::WaitableAsyncFileIoRequest_s requests_[Depth]; // one waiter: this task
    };

    /// <summary>
    /// Nodes Parse and Encode (one worker task per thread): encodes the next published block first (and stitches),
    /// else parses the next chunk of the parse window, else waits for work.
    /// <para>
    /// The workers sleep on one semaphore. Every change that can give a worker something to do releases it: Resolve
    /// publishes a block (one count), the parse window moves (one count per new chunk), Resolve ends (one count per
    /// worker). A worker re-checks all its work before it waits again, so a count too many only costs a loop, and no
    /// change is missed.
    /// </para>
    /// </summary>
    class WorkerTask : public Tasks::Task
    {
    public:
        WorkerTask(const LevelParams& params, InputWindow& input, ChunkLogs& logs, BlockRing& ring, Stitch& stitch, Synchronization::Semaphore& work) noexcept
            : params_(params),
              input_(input),
              logs_(logs),
              ring_(ring),
              stitch_(stitch),
              work_(work)
        {}

        void Run() noexcept override
        {
            EncodeWork                   encode(input_.Data(), ring_, stitch_);
            std::unique_ptr<ChunkParser> parser;
            for (;;) {
                if (encode.TryEncode())
                    continue;
                const std::size_t k = logs_.NextParse();
                if (k < logs_.Chunks() && logs_.MayParse(k)) {
                    if (logs_.TryAcquireParse(k))
                        Parse(k, parser);
                    continue;
                }
                if (logs_.ParsesDone(k) && ring_.EncodesDone())
                    break;
                work_.Wait();
            }
        }

    private:
        /// <summary>Node Parse: the speculative parse of chunk k into its step log, and the CRC-32 of its input.</summary>
        void Parse(std::size_t k, std::unique_ptr<ChunkParser>& parser)
        {
            input_.AwaitChunk(k);
            const std::uint8_t* in = input_.Data();
            if (!parser) {
                parser = std::make_unique<ChunkParser>();
                parser->Init(params_, in, input_.Size());
            }
            const std::uint32_t minLens = logs_.MinLens(params_, in, k);
            parser->Parse(logs_.Starts(), k, logs_.Overlap(), minLens, logs_.Log(k));
            const std::size_t start = logs_.Starts()[k];
            logs_.CompleteParse(k, Crc32(0, in + start, logs_.End(k) - start));
        }

        const LevelParams&          params_;
        InputWindow&                input_;
        ChunkLogs&                  logs_;
        BlockRing&                  ring_;
        Stitch&                     stitch_;
        Synchronization::Semaphore& work_; // many waiters: the workers
    };

    /// <summary>
    /// Node Resolve (serial, in order): turns the chunk logs into the exact libdeflate blocks and publishes them into the
    /// ring; moves the parse window and the read allowance as it goes. When it is done it helps Encode until every block
    /// is stitched. It is not a task: DeflateGraph::Run calls Run on the task that runs the graph, so Resolve has a
    /// thread of its own beside the workers' threads.
    /// </summary>
    class ResolveNode : IChunkLogs, IBlockSink
    {
    public:
        ResolveNode(const LevelParams& params, InputWindow& input, ChunkLogs& logs, BlockRing& ring, Stitch& stitch, Synchronization::Semaphore& work,
                    std::uint32_t workers) noexcept
            : params_(params),
              input_(input),
              logs_(logs),
              ring_(ring),
              stitch_(stitch),
              work_(work),
              workers_(workers)
        {}

        void Run() noexcept
        {
            Resolver resolver(params_, input_.Data(), input_.Size());
            stats_ = resolver.Run(logs_.Starts(), *this, *this, logs_.RecentMinLens());
            logs_.CompleteResolve();
            ring_.CompleteAllResolved();
            work_.Release(workers_);
            if (input_.Streaming())
                input_.Allow(input_.Size());
            resolved_ = DeflateClock::now();

            // Help with the remaining blocks until all are appended.
            EncodeWork        encode(input_.Data(), ring_, stitch_);
            const std::size_t total = ring_.Published();
            while (ring_.Stitched() < total) {
                if (!encode.TryEncode()) {
                    stitch_.TryStitch();
                    if (ring_.Stitched() < total)
                        ring_.AwaitEncoded();
                }
            }
        }

        /// <summary>Input end the parse window can need while Resolve waits for chunk.</summary>
        static std::size_t WindowEnd(const InputWindow& input, const ChunkLogs& logs, std::size_t chunk) noexcept
        {
            const std::size_t last = chunk + logs.Ahead();
            return last >= logs.Chunks() ? input.Size() : input.Need(last);
        }

        const ResolveStats&      Stats() const noexcept { return stats_; }
        double                   Waited() const noexcept { return waited_; }
        DeflateClock::time_point Resolved() const noexcept { return resolved_; }

    private:
        // IChunkLogs: the edge logs, seen from the resolver
        const PathLog& AwaitLog(std::size_t chunk) override
        {
            if (const std::size_t moved = logs_.MoveWindow(chunk); moved != 0) {
                // The parse window moves: more chunks may be claimed, more input read.
                work_.Release((std::min)(moved, logs_.Ahead()));
                if (input_.Streaming())
                    input_.Allow(WindowEnd(input_, logs_, chunk));
            }
            const auto     t0  = DeflateClock::now();
            const PathLog& log = logs_.AcquireLog(chunk);
            waited_ += std::chrono::duration<double>(DeflateClock::now() - t0).count();
            return log;
        }

        void Release(std::size_t chunk, bool misses) override { logs_.CompleteLog(chunk, misses); }

        // IBlockSink: the edge ring, seen from the resolver
        void OnBlock(ResolvedBlock&& block) override
        {
            ring_.AcquireResolve() = std::move(block);
            ring_.CompleteResolve();
            work_.Release();
        }

        std::size_t AppendedTo() const noexcept override { return stitch_.AppendedTo(); }

        const LevelParams&          params_;
        InputWindow&                input_;
        ChunkLogs&                  logs_;
        BlockRing&                  ring_;
        Stitch&                     stitch_;
        Synchronization::Semaphore& work_;
        std::uint32_t               workers_;
        ResolveStats                stats_;
        double                      waited_ = 0;
        DeflateClock::time_point    resolved_;
    };

    /// <summary>
    /// Node Write: writes the final output with overlapped AsyncFile writes (Depth segments in flight), or hands it to the
    /// sink segment by segment, and decommits what is written; ends when the graph has set the end of the output.
    /// </summary>
    class WriteTask : public Tasks::Task
    {
    public:
        static constexpr std::size_t Block = SegmentSize; // bytes per write
        static constexpr std::size_t Depth = 4;           // writes in flight

        explicit WriteTask(OutputWindow& output) noexcept : output_(output) {}

        void Run() noexcept override
        {
            using Windows::Error;
            std::uint8_t* const base = output_.Data();
            for (;;) {
                // Nothing new to write: complete the writes in flight first, so their memory is freed at once.
                while (completed_ < issued_ && !output_.MoreToWrite())
                    CompleteOldest();
                bool              final = false;
                const std::size_t end   = output_.AcquireWrite(final);
                while (output_.Issued() < end) {
                    const std::size_t pos   = output_.Issued();
                    const std::size_t bytes = (std::min)(Block, end - pos);
                    output_.IssueWrite(pos + bytes);
                    if (IOutputSink* sink = output_.Sink(); sink != nullptr) {
                        sink->Write(base + pos, bytes); // may suspend this task
                        output_.CompleteWriteTo(pos + bytes, Error::Success);
                        continue;
                    }
                    if (issued_ - completed_ == Depth)
                        CompleteOldest();
                    const std::size_t slot = issued_ % Depth;
                    auto&             req  = requests_[slot];
                    req.Reset();
                    req.SetOffset(pos);
                    ranges_[slot] = {pos, bytes};
                    const std::uint32_t result
                        = Error::Succeeded(output_.Error()) ? output_.File().WriteAsync(base + pos, static_cast<unsigned long>(bytes), req) : output_.Error();
                    if (Error::Failed(result) && !Error::IsPending(result))
                        req.RequestCompleted(result, 0);
                    ++issued_;
                }
                if (final)
                    break;
            }
            while (completed_ < issued_)
                CompleteOldest();
            output_.Close();
        }

    private:
        struct Range {
            std::size_t pos;
            std::size_t bytes;
        };

        void CompleteOldest() noexcept
        {
            using Windows::Error;
            const std::size_t slot  = completed_ % Depth;
            auto&             req   = requests_[slot];
            std::uint32_t     error = Error::Success;
            req.Wait();
            if (Error::Failed(req.GetError()))
                error = req.GetError();
            else if (req.GetTransferredBytes() != ranges_[slot].bytes)
                error = ERROR_WRITE_FAULT;
            output_.CompleteWriteTo(ranges_[slot].pos + ranges_[slot].bytes, error);
            ++completed_;
        }

        OutputWindow&                         output_;
        std::uint64_t                         issued_    = 0;
        std::uint64_t                         completed_ = 0;
        AsyncIo::WaitableAsyncFileIoRequest_s requests_[Depth]; // one waiter: this task
        Range                                 ranges_[Depth] = {};
    };

    /// <summary>
    /// Node SequentialCompress (the sequential graph): the libdeflate port, one task. It streams: before each block it
    /// lets Read run ReadAhead bytes ahead and waits for the block's input, after each block it hands the final output to
    /// Write; the input behind the matchfinder window is freed. So it overlaps with the file I/O, and the memory stays
    /// bounded, while the compressor itself is the same as for a buffer in memory.
    /// </summary>
    class SequentialCompressTask : public Tasks::Task, ICompressProgress
    {
    public:
        static constexpr std::size_t ReadAhead  = 8 * SegmentSize; // input Read may load past the block being compressed
        static constexpr std::size_t KeepBehind = 65536;           // input kept before the block: more than the matchfinder window
        static constexpr std::size_t BlockSlack = 65536;           // output committed past a block's worst case (headers)

        SequentialCompressTask(std::int32_t level, InputWindow& input, OutputWindow& output) noexcept : level_(level), input_(input), output_(output) {}

        void Run() noexcept override
        {
            const std::uint8_t* in   = input_.Data();
            const std::size_t   size = input_.Size();
            std::uint8_t*       out  = output_.Data();
            output_.Commit(SegmentSize);
            headerSize_           = WriteGzipHeader(level_, out);
            const auto compressor = Compressor::Create(level_);
            compressor->SetProgress(this);
            std::size_t end = headerSize_ + compressor->CompressDeflate(in, size, out + headerSize_);
            crc_            = Crc32(crc_, in + crcPos_, size - crcPos_);
            output_.Commit(end + GzipTrailerSize);
            end += WriteGzipTrailer(crc_, size, out + end);
            outSize_ = end;
        }

        std::size_t   OutSize() const noexcept { return outSize_; }
        std::uint32_t Crc() const noexcept { return crc_; }

    private:
        // ICompressProgress: the edges input and output, seen from the compressor
        void BeforeBlock(std::size_t inPos, std::size_t inputEnd, std::size_t outPos) override
        {
            if (input_.Streaming()) {
                input_.Allow((std::min)(input_.Size(), inputEnd + ReadAhead));
                input_.AwaitLoaded(inputEnd);
                // The CRC up to the block start, then the input further back than the matchfinder window is free.
                crc_    = Crc32(crc_, input_.Data() + crcPos_, inPos - crcPos_);
                crcPos_ = inPos;
                input_.ReleaseBelow(inPos > KeepBehind ? inPos - KeepBehind : 0);
            }
            // A block's output is at most its input plus the block headers.
            output_.Commit(headerSize_ + outPos + 2 * (inputEnd - inPos) + BlockSlack);
        }

        void AfterBlock(std::size_t, std::size_t outEnd) override { output_.CompleteProduce(headerSize_ + outEnd); }

        std::int32_t  level_;
        InputWindow&  input_;
        OutputWindow& output_;
        std::size_t   headerSize_ = 0;
        std::size_t   crcPos_     = 0; // the CRC covers the input below
        std::uint32_t crc_        = 0;
        std::size_t   outSize_    = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
