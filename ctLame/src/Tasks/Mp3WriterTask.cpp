// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Mp3WriterTask.h"

#include <cstdint>

import ThinkMeta.ConcurrentTasks.Platform.Error;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    using Platform::Error;

    namespace {

        // Creates the file for writing, or empties it; others may read it meanwhile.
        std::uint32_t CreateForWriting(AsyncIo::AsyncFile& file, const Shared::Char* path) noexcept
        {
#if defined(_WIN32)
            return file.Create(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN);
#else
            return file.Create(path, Platform::FileOptions{.access = Platform::FileAccess::Write, .creation = Platform::FileCreation::CreateAlways});
#endif
        }

    } // namespace

    Mp3WriterTask::Mp3WriterTask(Mp3File& mp3, const Settings& settings) noexcept : mp3_(mp3), settings_(settings)
    {}

    void Mp3WriterTask::Run() noexcept
    {
        const ProfiledRun run(settings_.profile);

        AsyncIo::AsyncFile output;
        error_ = CreateForWriting(output, settings_.path);

        // the file, front to back
        std::uint64_t position = 0;

        Write(output, position, mp3_.Id3v2());

        // the MP3 stream; it begins with the placeholder of the LAME tag
        const std::uint64_t lameTagAt = position;
        Mp3File::Bytes      block;
        while (mp3_.AcquireBlock(block)) {
            Write(output, position, block);
            mp3_.ReleaseBlock(); // also after an error (Write then does nothing), so Encode never waits for a full stream
        }

        Write(output, position, mp3_.Id3v1());

        // only now known: the LAME tag, over its placeholder
        position = lameTagAt;
        Write(output, position, mp3_.LameTag());

        output.Close();
    }

    void Mp3WriterTask::Write(AsyncIo::AsyncFile& output, std::uint64_t& position, Mp3File::Bytes bytes) noexcept
    {
        if (bytes.size > 0 && Error::Succeeded(error_)) {
            AsyncIo::AsyncFile::ByteCount written = 0;
            error_                                = Waiting(settings_.profile, [&] { return output.Write(bytes.data, bytes.size, written, position); });
            if (Error::Succeeded(error_) && written != bytes.size)
                error_ = Error::WriteFault;
        }
        position += bytes.size;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
