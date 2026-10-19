// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "FileIo.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    using Windows::Error;

    // --- BlockPipe -------------------------------------------------------------------------------

    BlockPipe::~BlockPipe()
    {
        if (memory_ != nullptr)
            VirtualFree(memory_, 0, MEM_RELEASE);
        if (sizes_ != nullptr)
            HeapFree(GetProcessHeap(), 0, sizes_);
    }

    bool BlockPipe::Initialize(std::uint32_t blockCount, std::uint32_t blockSize) noexcept
    {
        const std::size_t total = static_cast<std::size_t>(blockCount) * blockSize;
        memory_                 = static_cast<std::byte*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        sizes_                  = static_cast<std::uint32_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, blockCount * sizeof(std::uint32_t)));
        if (memory_ == nullptr || sizes_ == nullptr)
            return false;
        blockCount_ = blockCount;
        blockSize_  = blockSize;
        return true;
    }

    std::byte* BlockPipe::AcquireWriteBlock() noexcept
    {
        while (!closed_.load(std::memory_order_acquire) && writeAcquired_ - released_.load(std::memory_order_acquire) >= blockCount_)
            spaceAvailable_.Wait();
        if (closed_.load(std::memory_order_acquire))
            return nullptr;
        return memory_ + static_cast<std::size_t>(writeAcquired_++ % blockCount_) * blockSize_;
    }

    void BlockPipe::CommitWriteBlock(std::uint32_t size) noexcept
    {
        const std::int64_t index    = committed_.load(std::memory_order_relaxed); // only the producer moves it
        sizes_[index % blockCount_] = size;
        committed_.store(index + 1, std::memory_order_release);
        dataAvailable_.Set();
    }

    void BlockPipe::SetEndOfStream() noexcept
    {
        endOfStream_.store(true, std::memory_order_release);
        dataAvailable_.Set();
    }

    bool BlockPipe::AcquireReadBlock(ReadBlock& block) noexcept
    {
        for (;;) {
            // the end mark first: once it is set, every block has been committed before it
            const bool ended = endOfStream_.load(std::memory_order_acquire);
            if (committed_.load(std::memory_order_acquire) > readAcquired_)
                break;
            if (ended)
                return false;
            dataAvailable_.Wait();
        }
        const auto slot = static_cast<std::uint32_t>(readAcquired_++ % blockCount_);
        block.data      = memory_ + static_cast<std::size_t>(slot) * blockSize_;
        block.size      = sizes_[slot];
        return true;
    }

    void BlockPipe::ReleaseReadBlock() noexcept
    {
        released_.store(released_.load(std::memory_order_relaxed) + 1, std::memory_order_release); // only the consumer moves it
        spaceAvailable_.Set();
    }

    void BlockPipe::Close() noexcept
    {
        closed_.store(true, std::memory_order_release);
        spaceAvailable_.Set();
    }

    // --- FileReadTask ----------------------------------------------------------------------------

    FileReadTask::FileReadTask(BlockPipe& pipe, const wchar_t* path, std::uint64_t offset, std::uint32_t readsInFlight) noexcept : pipe_(pipe), path_(path)
    {
        options_.blockSize      = pipe.BlockSize();
        options_.blocksInFlight = readsInFlight < pipe.BlockCount() ? readsInFlight : pipe.BlockCount() - 1;
        options_.startOffset    = offset;
    }

    void FileReadTask::Run() noexcept
    {
        // a read error ends the input early, as a failing ReadFile does in the synchronous InputFile
        AsyncIo::AsyncFile::ReadFile(path_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, this, options_);
    }

    std::byte* FileReadTask::RequestWriteMemory(std::size_t) noexcept
    {
        return pipe_.AcquireWriteBlock(); // null once the consumer has closed the pipe: ReadFile ends
    }

    void FileReadTask::ReleaseWriteMemory(std::size_t size) noexcept
    {
        pipe_.CommitWriteBlock(static_cast<std::uint32_t>(size));
    }

    void FileReadTask::SetEndOfStream() noexcept
    {
        pipe_.SetEndOfStream();
    }

    // --- FileWriteTask ---------------------------------------------------------------------------

    FileWriteTask::FileWriteTask(BlockPipe& pipe, const wchar_t* path, std::uint32_t writesInFlight) noexcept : pipe_(pipe), path_(path)
    {
        options_.blockSize      = pipe.BlockSize();
        options_.blocksInFlight = writesInFlight < pipe.BlockCount() ? writesInFlight : pipe.BlockCount() - 1;
    }

    void FileWriteTask::Run() noexcept
    {
        // the file was created by FileIo::CreateOutput
        error_ = AsyncIo::AsyncFile::WriteFile(path_, GENERIC_WRITE, FILE_SHARE_READ, TRUNCATE_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, this, options_);
        // after an error: take the rest, so that the producer never waits on a full pipe
        BlockPipe::ReadBlock block{};
        while (pipe_.AcquireReadBlock(block))
            pipe_.ReleaseReadBlock();
    }

    const std::byte* FileWriteTask::RequestReadMemory(std::size_t& size) noexcept
    {
        BlockPipe::ReadBlock block{};
        if (!pipe_.AcquireReadBlock(block)) {
            size = 0;
            return nullptr;
        }
        size = block.size;
        return block.data;
    }

    void FileWriteTask::ReleaseReadMemory(std::size_t) noexcept
    {
        pipe_.ReleaseReadBlock(); // the blocks go back in the order they were taken
    }

    // --- FileIo ----------------------------------------------------------------------------------

    FileIo::~FileIo()
    {
        Finish();
        delete reader_;
        delete writer_;
    }

    bool FileIo::AttachInput(InputFile& fin, const wchar_t* path) noexcept
    {
        const std::int64_t position = path[0] == L'-' && path[1] == 0 ? -1 : fin.HandlePosition();
        if (position < 0 || !readPipe_.Initialize(ReadBlocks, ReadBlockSize))
            return false;
        reader_ = new (std::nothrow) FileReadTask(readPipe_, path, static_cast<std::uint64_t>(position), ReadsInFlight);
        if (reader_ == nullptr)
            return false;
        fin.Attach(this);
        return true;
    }

    std::uint32_t FileIo::CreateOutput(const wchar_t* path) noexcept
    {
        if (!writePipe_.Initialize(WriteBlocks, WriteBlockSize))
            return ERROR_NOT_ENOUGH_MEMORY;
        // created here, so that an error is reported before encoding; FileWrite opens it again
        const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return GetLastError();
        CloseHandle(file);
        writer_ = new (std::nothrow) FileWriteTask(writePipe_, path, WritesInFlight);
        return writer_ != nullptr ? ERROR_SUCCESS : static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY);
    }

    void FileIo::Start(ITaskScheduler& io) noexcept
    {
        if (reader_ != nullptr) {
            group_.AddTask(reader_);
            io.AddTask(reader_, TaskPriority::Normal);
        }
        if (writer_ != nullptr) {
            group_.AddTask(writer_);
            io.AddTask(writer_, TaskPriority::Normal);
        }
    }

    bool FileIo::Next(const std::byte*& data, std::size_t& size) noexcept
    {
        if (readHeld_) {
            readPipe_.ReleaseReadBlock();
            readHeld_ = false;
        }
        BlockPipe::ReadBlock block{};
        if (!readPipe_.AcquireReadBlock(block))
            return false;
        readHeld_ = true;
        data      = block.data;
        size      = block.size;
        return true;
    }

    bool FileIo::Write(const std::uint8_t* data, std::int32_t length) noexcept
    {
        const auto* src  = reinterpret_cast<const std::byte*>(data);
        auto        left = static_cast<std::uint32_t>(length);
        while (left > 0 && !writeFailed_) {
            if (writeBlock_ == nullptr) {
                writeBlock_ = writePipe_.AcquireWriteBlock();
                writeUsed_  = 0;
                if (writeBlock_ == nullptr) {
                    writeFailed_ = true;
                    break;
                }
            }
            const std::uint32_t room = writePipe_.BlockSize() - writeUsed_;
            const std::uint32_t take = left < room ? left : room;
            std::memcpy(writeBlock_ + writeUsed_, src, take);
            writeUsed_ += take;
            src += take;
            left -= take;
            if (writeUsed_ == writePipe_.BlockSize()) {
                writePipe_.CommitWriteBlock(writeUsed_);
                writeBlock_ = nullptr;
            }
        }
        return !writeFailed_;
    }

    void FileIo::EndOutput() noexcept
    {
        if (writer_ == nullptr || outputEnded_)
            return;
        outputEnded_ = true;
        if (writeBlock_ != nullptr && writeUsed_ > 0)
            writePipe_.CommitWriteBlock(writeUsed_);
        writeBlock_ = nullptr;
        writePipe_.SetEndOfStream();
    }

    bool FileIo::Finish() noexcept
    {
        if (finished_)
            return true;
        finished_ = true;
        if (reader_ != nullptr) {
            if (readHeld_) {
                readPipe_.ReleaseReadBlock();
                readHeld_ = false;
            }
            readPipe_.Close(); // the encoder takes no more input (it may stop before the end of the file)
        }
        EndOutput();
        group_.Wait();
        bool ok = true;
        if (writer_ != nullptr)
            ok = !writeFailed_ && Error::Succeeded(writer_->Error());
        return ok;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
