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
#include <cstddef>
#include <cstdint>
#include <cstdlib>

// headers with imports last
#include "InputWindow.h"

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>
    /// Edge "output": Stitch (or SequentialCompress) → Write. The bitstream in a reserved address range: the producer
    /// commits it ahead of the bitstream and reports how far it is final (CompleteProduce); Write writes it segment by
    /// segment with overlapped writes and decommits what is written. A buffer in memory can be attached instead; then
    /// there is no Write node and nothing is written.
    /// </summary>
    class OutputWindow
    {
    public:
        /// <summary>Final output the producer may run ahead of the completed writes (more than the writes in flight).</summary>
        static constexpr std::size_t MaxUnwritten = 16 * SegmentSize;

        OutputWindow() = default;
        ~OutputWindow()
        {
            if (base_ != nullptr && !attached_)
                VirtualFree(base_, 0, MEM_RELEASE);
        }
        OutputWindow(const OutputWindow&)            = delete;
        OutputWindow& operator=(const OutputWindow&) = delete;

        /// <summary>Creates the file and reserves bound bytes of address space for the output.</summary>
        std::uint32_t Open(const wchar_t* path, std::size_t bound) noexcept
        {
            using Windows::Error;
            const std::uint32_t error = file_.Create(path, GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN);
            if (Error::Failed(error))
                return error;
            reserved_ = (bound + SegmentSize - 1) / SegmentSize * SegmentSize + SegmentSize;
            base_     = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, reserved_, MEM_RESERVE, PAGE_READWRITE));
            return base_ != nullptr ? Error::Success : Error::GetLastError();
        }

        /// <summary>The whole output goes to out (its bound); Commit and CompleteProduce do nothing.</summary>
        void Attach(std::uint8_t* out) noexcept
        {
            base_     = out;
            attached_ = true;
        }

        bool          Attached() const noexcept { return attached_; }
        std::uint8_t* Data() const noexcept { return base_; }

        // --- Stitch, SequentialCompress (one producer at a time)

        /// <summary>Commits the output up to end.</summary>
        void Commit(std::size_t end) noexcept
        {
            if (attached_)
                return;
            end = (std::min)((end + SegmentSize - 1) / SegmentSize * SegmentSize, reserved_);
            if (end > committed_) {
                // Out of memory: the bitstream cannot continue. The sample ends the process instead of unwinding the
                // graph (see the README, "Error handling").
                if (VirtualAlloc(base_ + committed_, end - committed_, MEM_COMMIT, PAGE_READWRITE) == nullptr)
                    std::abort();
                committed_ = end;
            }
        }

        /// <summary>
        /// The output below end is final: Write may write its whole segments. Waits while more than MaxUnwritten bytes are
        /// final but not yet written, so a slow disk bounds the memory instead of letting the output pile up.
        /// </summary>
        void CompleteProduce(std::size_t end) noexcept
        {
            if (attached_ || end / SegmentSize * SegmentSize <= signaled_)
                return;
            signaled_ = end / SegmentSize * SegmentSize;
            produced_.store(signaled_, std::memory_order_seq_cst);
            producedMoved_.Set();
            while (signaled_ - written_.load(std::memory_order_seq_cst) > MaxUnwritten)
                writtenMoved_.Wait();
        }

        // --- the graph, at the end

        /// <summary>The whole output ends at end: Write writes the rest and ends.</summary>
        void CompleteAll(std::size_t end) noexcept
        {
            finalEnd_ = end;
            final_.store(true, std::memory_order_seq_cst);
            producedMoved_.Set();
        }

        /// <summary>The first error of a write (after Write ended).</summary>
        std::uint32_t Error() const noexcept { return error_; }

        // --- Write

        /// <summary>Waits until more whole segments are final, or the end is known; returns the end Write may write to.</summary>
        std::size_t AcquireWrite(bool& final) noexcept
        {
            for (;;) {
                final = final_.load(std::memory_order_seq_cst);
                if (final)
                    return finalEnd_;
                const std::size_t end = produced_.load(std::memory_order_seq_cst);
                if (end > issued_)
                    return end;
                producedMoved_.Wait();
            }
        }

        /// <summary>More output is final than issued, or the end is known.</summary>
        bool MoreToWrite() const noexcept { return final_.load(std::memory_order_seq_cst) || produced_.load(std::memory_order_seq_cst) > issued_; }

        AsyncIo::AsyncFile& File() noexcept { return file_; }
        std::size_t         Issued() const noexcept { return issued_; }

        /// <summary>Write issued the bytes [Issued(), end).</summary>
        void IssueWrite(std::size_t end) noexcept { issued_ = end; }

        /// <summary>
        /// The writes below end are done (in order), also a failed one, so the producer never waits for a write that
        /// will not come: decommits the whole segments below end.
        /// </summary>
        void CompleteWriteTo(std::size_t end, std::uint32_t error) noexcept
        {
            if (error_ == 0 && error != 0)
                error_ = error;
            const std::size_t whole = end / SegmentSize * SegmentSize;
            if (whole > decommitted_) {
                VirtualFree(base_ + decommitted_, whole - decommitted_, MEM_DECOMMIT);
                decommitted_ = whole;
            }
            written_.store(end, std::memory_order_seq_cst);
            writtenMoved_.Set();
        }

        void Close() noexcept
        {
            if (!attached_)
                file_.Close();
        }

    private:
        AsyncIo::AsyncFile                file_;
        std::uint8_t*                     base_      = nullptr;
        bool                              attached_  = false;
        std::size_t                       reserved_  = 0;
        std::size_t                       committed_ = 0; // the producer's
        std::size_t                       signaled_  = 0; // the producer's
        std::atomic<std::size_t>          produced_{0};
        std::atomic<bool>                 final_{false};
        std::size_t                       finalEnd_ = 0;
        Synchronization::AutoResetEvent_s producedMoved_;   // one waiter: Write
        std::size_t                       issued_      = 0; // Write's
        std::uint32_t                     error_       = 0; // Write's
        std::size_t                       decommitted_ = 0; // Write's
        std::atomic<std::size_t>          written_{0};      // the writes below are complete
        Synchronization::AutoResetEvent_s writtenMoved_;    // one waiter: the producer (Stitch holds its flag, or SequentialCompress)
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
