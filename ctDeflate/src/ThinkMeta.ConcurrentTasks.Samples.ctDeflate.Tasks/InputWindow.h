//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>Commit granule of the input and the output, and the default block of a read or write.</summary>
    constexpr std::size_t SegmentSize = std::size_t{1} << 20;

    /// <summary>The shape of the overlapped I/O of Read or Write: block size and requests in flight.</summary>
    struct IoShape {
        static constexpr std::uint32_t MaxDepth = 16;

        std::size_t   block = SegmentSize; // bytes per request (with grow: the largest)
        std::uint32_t depth = 4;           // requests in flight, at most MaxDepth
        bool          grow  = false;       // start with 64 KB and double the block with every request up to block
    };

    /// <summary>
    /// Edge "input": Read → Parse (read further by Resolve, Encode, Stitch and SequentialCompress). The input bytes at
    /// their file positions. From a file they live in a reserved address range: Read commits and reads it segment by
    /// segment up to the end Resolve allows (Allow), and marks each chunk whose input is loaded; Stitch decommits what no
    /// stage reads any more. So positions stay absolute while the memory stays bounded. Bytes already in memory are
    /// attached instead; then there is no Read node and nothing waits.
    /// </summary>
    class InputWindow
    {
    public:
        InputWindow() = default;
        ~InputWindow()
        {
            if (base_ != nullptr)
                VirtualFree(base_, 0, MEM_RELEASE);
        }
        InputWindow(const InputWindow&)            = delete;
        InputWindow& operator=(const InputWindow&) = delete;

        /// <summary>Streams the input from a file.</summary>
        std::uint32_t Open(const wchar_t* path) noexcept
        {
            using Windows::Error;
            std::uint32_t error = file_.Create(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
            if (Error::Failed(error))
                return error;
            AsyncIo::AsyncFile::FileSize size = 0;
            error                             = file_.GetSize(size);
            if (Error::Failed(error))
                return error;
            size_ = static_cast<std::size_t>(size);
            // One spare segment: the last one is committed whole, so reads a little past the end stay valid.
            base_ = static_cast<std::uint8_t*>(
                VirtualAlloc(nullptr, (size_ + SegmentSize - 1) / SegmentSize * SegmentSize + SegmentSize, MEM_RESERVE, PAGE_READWRITE));
            data_      = base_;
            path_      = path;
            streaming_ = true;
            return base_ != nullptr ? Error::Success : Error::GetLastError();
        }

        /// <summary>The whole input is in memory already.</summary>
        void Attach(const std::uint8_t* data, std::size_t size) noexcept
        {
            data_      = data;
            size_      = size;
            streaming_ = false;
        }

        bool                Streaming() const noexcept { return streaming_; }
        const wchar_t*      Path() const noexcept { return path_; }
        const std::uint8_t* Data() const noexcept { return data_; }
        std::size_t         Size() const noexcept { return size_; }

        /// <summary>needs[k]: the input end chunk k's parse may read; Parse waits until it is loaded.</summary>
        void PlanChunks(std::vector<std::size_t> needs)
        {
            needs_ = std::move(needs);
            if (streaming_)
                chunkReady_ = std::make_unique<Synchronization::ManualResetEvent[]>(needs_.size());
        }

        std::size_t Need(std::size_t k) const noexcept { return needs_[k]; }

        // --- Resolve (or the graph, before the start): how far Read may run ahead

        /// <summary>Read may load the input up to end (it only grows).</summary>
        void Allow(std::size_t end) noexcept
        {
            allowed_.store(end, std::memory_order_seq_cst);
            allowedMoved_.Set();
        }

        // --- Read

        AsyncIo::AsyncFile& File() noexcept { return file_; }
        std::uint8_t*       Writable() noexcept { return base_; }

        /// <summary>The input end Read may load now.</summary>
        std::size_t AcquireRead() const noexcept { return allowed_.load(std::memory_order_seq_cst); }

        /// <summary>Waits until Allow moved the end (or did since the last wait).</summary>
        void AwaitAllowance() noexcept { allowedMoved_.Wait(); }

        /// <summary>Commits the segment at pos (a multiple of SegmentSize).</summary>
        bool CommitSegment(std::size_t pos) noexcept { return VirtualAlloc(base_ + pos, SegmentSize, MEM_COMMIT, PAGE_READWRITE) != nullptr; }

        /// <summary>The bytes before loaded are in memory: wakes the parses whose input is complete.</summary>
        void CompleteRead(std::size_t loaded) noexcept
        {
            while (readyNext_ < needs_.size() && needs_[readyNext_] <= loaded)
                chunkReady_[readyNext_++].Set();
            loaded_.store(loaded, std::memory_order_seq_cst);
            loadedMoved_.Set();
        }

        /// <summary>After a failed read the graph still finishes over whatever is in the pages; the result reports the error.</summary>
        void          SetReadError(std::uint32_t error) noexcept { readError_.store(error, std::memory_order_relaxed); }
        std::uint32_t ReadError() const noexcept { return readError_.load(std::memory_order_relaxed); }

        // --- Parse

        /// <summary>Waits until chunk k's input is loaded.</summary>
        void AwaitChunk(std::size_t k) noexcept
        {
            if (streaming_)
                chunkReady_[k].Wait();
        }

        // --- SequentialCompress (one waiter)

        /// <summary>Waits until the input below end is loaded.</summary>
        void AwaitLoaded(std::size_t end) noexcept
        {
            if (streaming_)
                while (loaded_.load(std::memory_order_seq_cst) < end)
                    loadedMoved_.Wait();
        }

        // --- Stitch, SequentialCompress

        /// <summary>Decommits the whole segments below pos.</summary>
        void ReleaseBelow(std::size_t pos) noexcept
        {
            pos = pos / SegmentSize * SegmentSize;
            if (pos > released_) {
                VirtualFree(base_ + released_, pos - released_, MEM_DECOMMIT);
                released_ = pos;
            }
        }

    private:
        AsyncIo::AsyncFile                                   file_;
        std::uint8_t*                                        base_      = nullptr;
        const std::uint8_t*                                  data_      = nullptr;
        const wchar_t*                                       path_      = nullptr;
        std::size_t                                          size_      = 0;
        bool                                                 streaming_ = false;
        std::vector<std::size_t>                             needs_;
        std::atomic<std::size_t>                             allowed_{0};
        Synchronization::AutoResetEvent                      allowedMoved_;
        std::unique_ptr<Synchronization::ManualResetEvent[]> chunkReady_;
        std::size_t                                          readyNext_ = 0; // Read's
        std::atomic<std::size_t>                             loaded_{0};
        Synchronization::AutoResetEvent                      loadedMoved_;
        std::atomic<std::uint32_t>                           readError_{0};
        std::size_t                                          released_ = 0; // Stitch's
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
