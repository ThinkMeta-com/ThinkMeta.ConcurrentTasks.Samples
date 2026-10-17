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
#include "Resolver.h"

// headers with imports last
#include "BlockRing.h"
#include "ChunkLogs.h"
#include "InputWindow.h"
#include "OutputWindow.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Memory.SequentialMemory;
import ThinkMeta.ConcurrentTasks.Windows.Error;

// The nodes of DeflateGraph (DeflateGraph.h). Each class names the node it is.
namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    using DeflateClock = std::chrono::steady_clock;

    /// <summary>
    /// Node Stitch: appends the encoded blocks to the bitstream in order, hands the final output to Write and frees what
    /// no stage needs any more (logs, input pages). It has no task of its own: the Encode that completes a block runs it, one at a time
    /// (the stitch flag); the others go on encoding.
    /// </summary>
    class Stitch
    {
    public:
        /// <summary>Input kept before the oldest position still in use: the matchfinder window and a parse's reach.</summary>
        static constexpr std::size_t KeepBehind = 32768 + 65536;

        Stitch(InputWindow& input, ChunkLogs& logs, BlockRing& ring, OutputWindow& output, BitWriter& os) noexcept
            : input_(input),
              logs_(logs),
              ring_(ring),
              output_(output),
              os_(os)
        {}

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
                while (ring_.AcquireStitch(s)) {
                    ResolvedBlock& block = ring_.StitchBlock(s);
                    output_.Commit(static_cast<std::size_t>(os_.Next() - out) + ring_.Encoded(s).bits.size() + block.length + 1024);
                    StitchBlock(os_, in, block, ring_.Encoded(s));
                    stitchedTo_ = block.begin + block.length;
                    ring_.CompleteStitch(s);
                    ++s;
                }
                if (s != first) {
                    ring_.SignalRoom();
                    ReleaseBehind();
                }
                busy_.store(false, std::memory_order_seq_cst);
                // A block may have become ready while the flag was held by this caller.
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
        std::atomic<bool> busy_{false};
        std::size_t       stitchedTo_ = 0; // input position the bitstream covers
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
            if (!ring_.AcquireEncode(i))
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
    /// Node Read: reads the input with overlapped AsyncFile reads (a few segments in flight) up to the end Resolve allows,
    /// and marks the chunks whose input is loaded.
    /// </summary>
    class ReadTask : public Tasks::Task
    {
    public:
        ReadTask(InputWindow& input, const IoShape& shape) noexcept : input_(input), shape_(shape) {}

        void Run() noexcept override
        {
            using Windows::Error;
            std::uint8_t*     data                     = input_.Writable();
            const std::size_t size                     = input_.Size();
            std::size_t       issuedTo                 = 0;
            std::size_t       loaded                   = 0;
            std::uint64_t     issued                   = 0;
            std::uint64_t     completed                = 0;
            std::size_t       committed                = 0;
            std::size_t       block                    = shape_.grow ? (std::min)(shape_.block, std::size_t{65536}) : shape_.block;
            const std::size_t depth                    = shape_.depth;
            std::size_t       bytes[IoShape::MaxDepth] = {};
            std::uint32_t     error                    = Error::Success;

            while (loaded < size) {
                const std::size_t allowed = input_.AcquireRead();
                while (issued - completed < depth && issuedTo < size && issuedTo < allowed) {
                    const std::size_t slot = issued % depth;
                    auto&             req  = requests_[slot];
                    bytes[slot]            = (std::min)(block, size - issuedTo);
                    if (shape_.grow)
                        block = (std::min)(block * 2, shape_.block);
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
                const std::size_t slot = completed % depth;
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
        InputWindow&                        input_;
        IoShape                             shape_;
        AsyncIo::WaitableAsyncFileIoRequest requests_[IoShape::MaxDepth];
    };

    /// <summary>
    /// Nodes Parse and Encode (one worker task per thread): encodes the next published block first (and stitches),
    /// else parses the next chunk of the parse window, else waits for work.
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
                    if (logs_.AcquireParse(k))
                        Parse(k, parser);
                    continue;
                }
                if (logs_.ParsesDone(k) && ring_.EncodesDone())
                    break;
                work_.Wait();
            }
        }

    private:
        /// <summary>Node Parse: the speculative parse of chunk k into its step log.</summary>
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
        Synchronization::Semaphore& work_; // a block was published, the parse window moved, or Resolve finished
    };

    /// <summary>
    /// Node Resolve (serial, in order): turns the chunk logs into the exact libdeflate blocks and publishes them into the
    /// ring; moves the parse window and the read allowance as it goes. When it is done it helps Encode until every block
    /// is stitched. DeflateGraph runs it on the calling task, a thread of its own beside the workers' threads.
    /// </summary>
    class ResolveTask : public Tasks::Task, IChunkLogs, IBlockSink
    {
    public:
        ResolveTask(const LevelParams& params, InputWindow& input, ChunkLogs& logs, BlockRing& ring, Stitch& stitch, Synchronization::Semaphore& work,
                    std::uint32_t workers) noexcept
            : params_(params),
              input_(input),
              logs_(logs),
              ring_(ring),
              stitch_(stitch),
              work_(work),
              workers_(workers)
        {}

        void Run() noexcept override
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
        const PathLog& Wait(std::size_t chunk) override
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
    /// Node Write: writes the final output with overlapped AsyncFile writes (a few segments in flight), or hands it to the
    /// sink, and decommits what is written; ends when the graph has set the end of the output.
    /// </summary>
    class WriteTask : public Tasks::Task
    {
    public:
        WriteTask(OutputWindow& output, const IoShape& shape) noexcept
            : output_(output),
              shape_(shape),
              block_(shape.grow ? (std::min)(shape.block, std::size_t{65536}) : shape.block)
        {}

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
                    const std::size_t bytes = (std::min)(block_, end - pos);
                    if (shape_.grow)
                        block_ = (std::min)(block_ * 2, shape_.block);
                    output_.IssueWrite(pos + bytes);
                    if (IDeflateSink* sink = output_.Sink(); sink != nullptr) {
                        sink->Write(base + pos, bytes);
                        output_.CompleteWriteTo(pos + bytes, Error::Success);
                        continue;
                    }
                    if (issued_ - completed_ == shape_.depth)
                        CompleteOldest();
                    const std::size_t slot = issued_ % shape_.depth;
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
            const std::size_t slot  = completed_ % shape_.depth;
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

        OutputWindow&                       output_;
        IoShape                             shape_;
        std::size_t                         block_;
        std::uint64_t                       issued_    = 0;
        std::uint64_t                       completed_ = 0;
        AsyncIo::WaitableAsyncFileIoRequest requests_[IoShape::MaxDepth];
        Range                               ranges_[IoShape::MaxDepth] = {};
    };

    /// <summary>
    /// Node Read with the library's AsyncFile::ReadFile (--library-io, for comparison; its default, 1 MB x 5) into the
    /// input window, bounded by the read allowance (at most blocksInFlight blocks past it, see RequestWriteMemory).
    /// </summary>
    class LibraryReadTask : public Tasks::Task, Memory::IWriteSequentialMemory
    {
    public:
        explicit LibraryReadTask(InputWindow& input) noexcept : input_(input) {}

        void Run() noexcept override
        {
            const std::uint32_t error
                = AsyncIo::AsyncFile::ReadFile(input_.Path(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, this);
            if (error != 0 || loaded_ < input_.Size()) {
                input_.SetReadError(error != 0 ? error : ERROR_HANDLE_EOF);
                input_.CompleteRead(input_.Size()); // wakes every waiter; the result reports the error
            }
        }

    private:
        // IWriteSequentialMemory: the input window, seen from ReadFile
        void       Initialize(std::size_t) noexcept override {}
        std::byte* RequestWriteMemory(std::size_t size) noexcept override
        {
            // Wait only while every byte the window allows has arrived: a read still in flight below the allowance may be
            // what the window waits for, and ReadFile completes it only after this call. So the reads run at most
            // blocksInFlight blocks past the allowance, also when the window is smaller than that (small chunks).
            for (;;) {
                const std::size_t allowed = input_.AcquireRead();
                if (requested_ < allowed || loaded_ < allowed)
                    break;
                input_.AwaitAllowance();
            }
            while (committed_ < requested_ + size) {
                if (!input_.CommitSegment(committed_))
                    return nullptr; // ReadFile ends with ERROR_NOT_ENOUGH_MEMORY
                committed_ += SegmentSize;
            }
            std::uint8_t* p = input_.Writable() + requested_;
            requested_ += size;
            return reinterpret_cast<std::byte*>(p);
        }
        void ReleaseWriteMemory(std::size_t size) noexcept override
        {
            loaded_ += size;
            input_.CompleteRead(loaded_);
        }
        void SetEndOfStream() noexcept override {}

        InputWindow& input_;
        std::size_t  requested_ = 0;
        std::size_t  committed_ = 0;
        std::size_t  loaded_    = 0;
    };

    /// <summary>
    /// Node Write with the library's AsyncFile::WriteFile (--library-io, for comparison; its default, 5 writes of up to 1 MB) in
    /// flight from the output window.
    /// </summary>
    class LibraryWriteTask : public Tasks::Task, Memory::IReadSequentialMemory
    {
    public:
        explicit LibraryWriteTask(OutputWindow& output) noexcept : output_(output) {}

        void Run() noexcept override
        {
            const std::uint32_t error = AsyncIo::AsyncFile::WriteFile(output_.Path(), GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, this);
            output_.CompleteWriteTo(released_, error);
        }

    private:
        // IReadSequentialMemory: the output window, seen from WriteFile
        std::size_t      GetSize() const noexcept override { return 0; }
        const std::byte* RequestReadMemory(std::size_t& size) noexcept override
        {
            bool              final = false;
            const std::size_t end   = output_.AcquireWrite(final);
            const std::size_t pos   = output_.Issued();
            if (pos >= end) {
                size = 0;
                return nullptr;
            }
            size = (std::min)(size, end - pos);
            output_.IssueWrite(pos + size);
            return reinterpret_cast<const std::byte*>(output_.Data() + pos);
        }
        void ReleaseReadMemory(std::size_t size) noexcept override
        {
            released_ += size;
            output_.CompleteWriteTo(released_, 0);
        }

        OutputWindow& output_;
        std::size_t   released_ = 0;
    };

    /// <summary>
    /// Node SequentialCompress (the sequential graph): the libdeflate port, one task. It streams: before each block it
    /// lets Read run a few segments ahead and waits for the block's input, after each block it hands the final output to
    /// Write; the input behind the matchfinder window is freed. So it overlaps with the file I/O, and the memory stays
    /// bounded, while the compressor itself is the same as for a buffer in memory.
    /// </summary>
    class SequentialCompressTask : public Tasks::Task, ICompressProgress
    {
    public:
        static constexpr std::size_t ReadAhead  = 8 * SegmentSize; // input Read may load past the block being compressed
        static constexpr std::size_t KeepBehind = 65536;           // input kept before the block: the matchfinder window

        SequentialCompressTask(std::int32_t level, bool gzip, InputWindow& input, OutputWindow& output, std::size_t readAhead) noexcept
            : readAhead_(readAhead),
              level_(level),
              gzip_(gzip),
              input_(input),
              output_(output)
        {}

        void Run() noexcept override
        {
            const std::uint8_t* in   = input_.Data();
            const std::size_t   size = input_.Size();
            std::uint8_t*       out  = output_.Data();
            output_.Commit(SegmentSize);
            headerSize_           = gzip_ ? WriteGzipHeader(level_, out) : 0;
            const auto compressor = Compressor::Create(level_);
            compressor->SetProgress(this);
            std::size_t end = headerSize_ + compressor->CompressDeflate(in, size, out + headerSize_);
            crc_            = Crc32(crc_, in + crcPos_, size - crcPos_);
            if (gzip_) {
                output_.Commit(end + 16);
                end += WriteGzipTrailer(crc_, size, out + end);
            }
            outSize_ = end;
        }

        std::size_t   OutSize() const noexcept { return outSize_; }
        std::uint32_t Crc() const noexcept { return crc_; }

    private:
        // ICompressProgress: the edges input and output, seen from the compressor
        void BeforeBlock(std::size_t inPos, std::size_t inputEnd, std::size_t outPos) override
        {
            if (input_.Streaming()) {
                input_.Allow((std::min)(input_.Size(), inputEnd + readAhead_));
                input_.AwaitLoaded(inputEnd);
                // The CRC up to the block start, then the input further back than the matchfinder window is free.
                crc_    = Crc32(crc_, input_.Data() + crcPos_, inPos - crcPos_);
                crcPos_ = inPos;
                input_.ReleaseBelow(inPos > KeepBehind ? inPos - KeepBehind : 0);
            }
            // A block's output is at most its input plus the block headers.
            output_.Commit(headerSize_ + outPos + 2 * (inputEnd - inPos) + 65536);
        }

        void AfterBlock(std::size_t, std::size_t outEnd) override { output_.CompleteProduce(headerSize_ + outEnd); }

        std::size_t   readAhead_;
        std::int32_t  level_;
        bool          gzip_;
        InputWindow&  input_;
        OutputWindow& output_;
        std::size_t   headerSize_ = 0;
        std::size_t   crcPos_     = 0; // the CRC covers the input below
        std::uint32_t crc_        = 0;
        std::size_t   outSize_    = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
