// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "WavReaderTask.h"

#include <cstddef>
#include <cstdint>

import ThinkMeta.ConcurrentTasks.Platform.Error;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    using Platform::Error;

    namespace {

        // Opens the file for reading front to back; others may write it meanwhile, like with LAME's fopen.
        std::uint32_t OpenForReading(AsyncIo::AsyncFile& file, const Shared::Char* path) noexcept
        {
#if defined(_WIN32)
            return file.Create(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
#else
            return file.Create(path, Platform::FileOptions{.shareWrite = true});
#endif
        }

    } // namespace

    WavReaderTask::WavReaderTask(BlockPipe& pcm, const Settings& settings) noexcept : pcm_(pcm), settings_(settings)
    {}

    void WavReaderTask::Run() noexcept
    {
        TaskProfile* const profile = settings_.profile;
        const ProfiledRun  run(profile);

        AsyncIo::AsyncFile input;
        error_ = OpenForReading(input, settings_.path);

        // the sample data: from dataOffset to the end of the file, at most byteLimit bytes
        std::uint64_t position = settings_.dataOffset;
        std::uint64_t end      = position;
        if (Error::Succeeded(error_)) {
            AsyncIo::AsyncFile::FileSize size = 0;
            error_                            = input.GetSize(size);
            if (Error::Succeeded(error_) && size > position)
                end = size - position > settings_.byteLimit ? position + settings_.byteLimit : size;
        }

        // block after block into the pipe
        while (Error::Succeeded(error_) && position < end) {
            std::byte* const block = pcm_.AcquireWriteBlock();

            const std::uint64_t                 rest  = end - position;
            const AsyncIo::AsyncFile::ByteCount bytes = rest < pcm_.BlockSize() ? static_cast<AsyncIo::AsyncFile::ByteCount>(rest) : pcm_.BlockSize();
            AsyncIo::AsyncFile::ByteCount       read  = 0;
            error_                                    = Waiting(profile, [&] { return input.Read(block, bytes, read, position); });
            if (Error::Failed(error_))
                break;

            pcm_.CommitWriteBlock(read);
            position += read;
            if (read != bytes)
                error_ = Error::EndOfFile; // the file has become shorter than its size said
        }

        input.Close();
        // always, also after an error. The error itself does not travel through the pipe: like LAME, the encoder takes a
        // read error for the end of the input and writes a complete file of what was read (EncodeGraph::Result)
        pcm_.End();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
