// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "SchedulerHandle.h"
#include "WavInput.h"

// headers with imports last
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>
    /// A bounded pipe of fixed-size blocks between one producer and one consumer, which may run on
    /// different schedulers: the edges FileRead → encoder and encoder → FileWrite. Both sides may hold
    /// several blocks at once (overlapped I/O into or out of the pipe) but commit and release them in
    /// the order they acquired them. The producer ends the stream (SetEndOfStream); the consumer can
    /// close the pipe early (Close), after which the producer gets no more blocks.
    /// </summary>
    class BlockPipe
    {
    public:
        struct ReadBlock {
            const std::byte* data;
            std::uint32_t    size;
        };

        BlockPipe() noexcept = default;
        ~BlockPipe();

        BlockPipe(const BlockPipe&)            = delete;
        BlockPipe& operator=(const BlockPipe&) = delete;

        /// <summary>Allocates the blocks; false if the memory is not available.</summary>
        bool Initialize(std::uint32_t blockCount, std::uint32_t blockSize) noexcept;

        std::uint32_t BlockCount() const noexcept { return blockCount_; }
        std::uint32_t BlockSize() const noexcept { return blockSize_; }

        // --- producer

        /// <summary>The next free block; waits while all blocks are in use. Null once the consumer has closed the pipe.</summary>
        std::byte* AcquireWriteBlock() noexcept;
        /// <summary>Hands the oldest acquired block with size bytes to the consumer.</summary>
        void CommitWriteBlock(std::uint32_t size) noexcept;
        /// <summary>No more blocks will be committed.</summary>
        void SetEndOfStream() noexcept;

        // --- consumer

        /// <summary>The next committed block; waits for it. False once the stream has ended.</summary>
        bool AcquireReadBlock(ReadBlock& block) noexcept;
        /// <summary>Returns the oldest acquired block to the producer.</summary>
        void ReleaseReadBlock() noexcept;
        /// <summary>The consumer takes no more blocks: a waiting producer gets null and ends.</summary>
        void Close() noexcept;

    private:
        std::byte*     memory_     = nullptr;
        std::uint32_t* sizes_      = nullptr;
        std::uint32_t  blockCount_ = 0;
        std::uint32_t  blockSize_  = 0;

        // producer side
        std::int64_t writeAcquired_ = 0;
        alignas(64) std::atomic<std::int64_t> committed_{0};
        // consumer side
        std::int64_t readAcquired_ = 0;
        alignas(64) std::atomic<std::int64_t> released_{0};
        alignas(64) std::atomic<bool> endOfStream_{false};
        std::atomic<bool>                 closed_{false};

        Synchronization::AutoResetEvent_s dataAvailable_;  // one waiter: the consumer
        Synchronization::AutoResetEvent_s spaceAvailable_; // one waiter: the producer
    };

    /// <summary>
    /// Node FileRead (I/O scheduler): the library's AsyncFile::ReadFile reads the file from an offset
    /// to its end into the pipe, BulkIoOptions blocks of BlockSize, several overlapped reads in flight
    /// (the core's I/O completion port). When the consumer closes the pipe, ReadFile gets no more
    /// memory and ends after the reads in flight.
    /// </summary>
    class FileReadTask final : public Tasks::Task, Memory::IWriteSequentialMemory
    {
    public:
        FileReadTask(BlockPipe& pipe, const wchar_t* path, std::uint64_t offset, std::uint32_t readsInFlight) noexcept;

        void Run() noexcept override;

    private:
        void       Initialize(std::size_t) noexcept override {}
        std::byte* RequestWriteMemory(std::size_t size) noexcept override;
        void       ReleaseWriteMemory(std::size_t size) noexcept override;
        void       SetEndOfStream() noexcept override;

        BlockPipe&             pipe_;
        const wchar_t*         path_;
        AsyncIo::BulkIoOptions options_;
    };

    /// <summary>
    /// Node FileWrite (I/O scheduler): the library's AsyncFile::WriteFile writes the blocks of the pipe
    /// to the file front to back, several overlapped writes in flight. After an error it takes the
    /// rest of the blocks, so the producer never waits on a full pipe.
    /// </summary>
    class FileWriteTask final : public Tasks::Task, Memory::IReadSequentialMemory
    {
    public:
        FileWriteTask(BlockPipe& pipe, const wchar_t* path, std::uint32_t writesInFlight) noexcept;

        void Run() noexcept override;

        /// <summary>ERROR_SUCCESS or the error that ended the writing.</summary>
        std::uint32_t Error() const noexcept { return error_; }

    private:
        std::size_t      GetSize() const noexcept override { return 0; }
        const std::byte* RequestReadMemory(std::size_t& size) noexcept override;
        void             ReleaseReadMemory(std::size_t size) noexcept override;

        BlockPipe&             pipe_;
        const wchar_t*         path_;
        AsyncIo::BulkIoOptions options_;
        std::uint32_t          error_ = ERROR_SUCCESS;
    };

    /// <summary>
    /// The asynchronous file I/O of one encoding: FileRead → read pipe → the encoder's InputFile, and
    /// the encoder's Ogg pages → write pipe → FileWrite, both nodes on the I/O scheduler. Either side is
    /// optional (stdin and stdout stay synchronous). Create and Start it on a task.
    /// </summary>
    class FileIo final : InputSource
    {
    public:
        static constexpr std::uint32_t ReadBlockSize  = 1024 * 1024;
        static constexpr std::uint32_t ReadBlocks     = 8;
        static constexpr std::uint32_t ReadsInFlight  = 5;
        static constexpr std::uint32_t WriteBlockSize = 64 * 1024;
        static constexpr std::uint32_t WriteBlocks    = 8;
        static constexpr std::uint32_t WritesInFlight = 2;

        FileIo() noexcept = default;
        ~FileIo();

        FileIo(const FileIo&)            = delete;
        FileIo& operator=(const FileIo&) = delete;

        /// <summary>
        /// Reads the input file at path behind what fin has buffered; false (fin unchanged, it goes on
        /// reading synchronously) for stdin or a pipe.
        /// </summary>
        bool AttachInput(InputFile& fin, const wchar_t* path) noexcept;

        /// <summary>Creates the output file, written by FileWrite; the Windows error code (0 on success).</summary>
        std::uint32_t CreateOutput(const wchar_t* path) noexcept;

        bool HasOutput() const noexcept { return writer_ != nullptr; }

        /// <summary>Starts the attached nodes on the running I/O scheduler.</summary>
        void Start(ITaskScheduler& io) noexcept;

        /// <summary>Appends bytes to the output (the encoder's write callback); false after a failure.</summary>
        bool Write(const std::uint8_t* data, std::int32_t length) noexcept;

        /// <summary>The end of the output: hands over the last block (the encoder's close callback).</summary>
        void EndOutput() noexcept;

        /// <summary>Ends both sides and waits for the nodes; true if all bytes were written.</summary>
        bool Finish() noexcept;

    private:
        bool Next(const std::byte*& data, std::size_t& size) noexcept override;

        BlockPipe                  readPipe_;
        FileReadTask*              reader_   = nullptr;
        bool                       readHeld_ = false; // the consumer holds a read block
        BlockPipe                  writePipe_;
        FileWriteTask*             writer_      = nullptr;
        std::byte*                 writeBlock_  = nullptr;
        std::uint32_t              writeUsed_   = 0;
        bool                       writeFailed_ = false;
        bool                       outputEnded_ = false;
        bool                       finished_    = false;
        Synchronization::TaskGroup group_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
