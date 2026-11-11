// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// The ID3 tags of one MP3 file: a port of id3tag.c (id3tag_spec with its FrameDataNode list) without the ID3v2.4
    /// UTF-8 variant and without --space-id3v1. The ID3v2.3 tag goes in front of the MP3 frames, the ID3v1 tag
    /// (128 bytes) behind them; the frontend writes both (lame_main.c).
    /// <para>
    /// Latin-1 texts are char strings; UTF-16 texts are char16_t strings that start with a byte order mark (0xFEFF),
    /// as LAME expects them. Like LAME, a tag starts with the encoder frame (TSSE), and the ID3v1 setters also set
    /// the matching ID3v2 frame. A version 2 tag is only written when it was asked for (AddV2, V2Only, a UTF-16 text,
    /// an album art, ...) or when a text does not fit into the version 1 tag.
    /// </para>
    /// <para>
    /// The output is bit-identical to LAME 4.0, with the ID3 fixes of LAME 4.1 (trunk) where they apply: descriptions
    /// of TXXX, WXXX and COMM are compared whole (4.0 took one that begins another for the same frame), frame ids
    /// need four characters, short language codes are padded, the padding is capped at MaxPadding, and a tag too
    /// large for its 28-bit size field is left out.
    /// </para>
    /// </summary>
    class Id3Tag
    {
    public:
        /// <summary>LAME_MAXID3V2PADDING (LAME 4.1): the largest padding SetPad accepts.</summary>
        static constexpr std::size_t MaxPadding = 128000;

        /// <summary>id3tag_init: an empty tag with the encoder frame.</summary>
        Id3Tag() noexcept;
        ~Id3Tag();

        Id3Tag(const Id3Tag&)            = delete;
        Id3Tag& operator=(const Id3Tag&) = delete;

        /// <summary>id3tag_add_v2: write a version 2 tag in any case.</summary>
        void AddV2() noexcept;

        /// <summary>id3tag_v1_only: no version 2 tag.</summary>
        void V1Only() noexcept;

        /// <summary>id3tag_v2_only: no version 1 tag.</summary>
        void V2Only() noexcept;

        /// <summary>id3tag_set_pad: write a version 2 tag with n bytes of padding (at most MaxPadding).</summary>
        void SetPad(std::size_t n) noexcept;

        /// <summary>id3tag_set_title: Latin-1, for both tags.</summary>
        void SetTitle(const char* title) noexcept;

        /// <summary>id3tag_set_artist: Latin-1, for both tags.</summary>
        void SetArtist(const char* artist) noexcept;

        /// <summary>id3tag_set_album: Latin-1, for both tags.</summary>
        void SetAlbum(const char* album) noexcept;

        /// <summary>id3tag_set_year: Latin-1, for both tags; version 1 keeps 4 digits.</summary>
        void SetYear(const char* year) noexcept;

        /// <summary>id3tag_set_comment: Latin-1, for both tags.</summary>
        void SetComment(const char* comment) noexcept;

        /// <summary>id3tag_set_track: "n" or "n/total"; -1 if n is out of the ID3v1 range 1..255 (kept for version 2).</summary>
        std::int32_t SetTrack(const char* track) noexcept;

        /// <summary>
        /// id3tag_set_genre: a genre number or name; -1 for an unknown number. An unknown name becomes "Other" in
        /// version 1 and goes as it is into version 2.
        /// </summary>
        std::int32_t SetGenre(const char* genre) noexcept;

        /// <summary>id3tag_set_fieldvalue: "XXXX=value" in Latin-1; -1 if the text has not this form.</summary>
        std::int32_t SetFieldValue(const char* fieldValue) noexcept;

        /// <summary>id3tag_set_fieldvalue_utf16: "XXXX=value" in UTF-16 with byte order mark.</summary>
        std::int32_t SetFieldValueUtf16(const char16_t* fieldValue) noexcept;

        /// <summary>
        /// id3tag_set_textinfo_latin1: one version 2 frame (T..., W..., COMM, TXXX and WXXX as "description=value").
        /// 0 on success, -1 for an invalid frame id, -7 without '=', -255 for frames not supported.
        /// </summary>
        std::int32_t SetTextInfoLatin1(const char* id, const char* text) noexcept;

        /// <summary>id3tag_set_textinfo_utf16: as SetTextInfoLatin1 in UTF-16; -3 without byte order mark.</summary>
        std::int32_t SetTextInfoUtf16(const char* id, const char16_t* text) noexcept;

        /// <summary>id3tag_set_comment_utf16: a COMM frame; lang nullptr means "eng", desc may be nullptr.</summary>
        std::int32_t SetCommentUtf16(const char* lang, const char16_t* desc, const char16_t* text) noexcept;

        /// <summary>id3tag_set_albumart: a JPEG, PNG or GIF image as APIC frame; -1 for other data.</summary>
        std::int32_t SetAlbumArt(const std::byte* image, std::size_t size) noexcept;

        /// <summary>
        /// lame_get_id3v2_tag: the version 2 tag; 0 if there is none or if it would exceed the 28-bit size field. Returns
        /// the size needed if size is too small. With a known number of samples it adds the length frame (TLEN), as LAME
        /// does on every call.
        /// </summary>
        std::size_t GetV2Tag(std::byte* buffer, std::size_t size, std::uint32_t numSamples, std::int32_t samplerateIn) noexcept;

        /// <summary>lame_get_id3v1_tag: the 128-byte version 1 tag; 0 if there is none.</summary>
        std::size_t GetV1Tag(std::byte* buffer, std::size_t size) const noexcept;

        /// <summary>id3tag_genre_list: the genres in alphabetical order, each with its number.</summary>
        static void GenreList(void (*handler)(std::int32_t number, const char* name, void* cookie), void* cookie) noexcept;

    private:
        /// <summary>A text of a frame: Latin-1 (enc 0, char) or UCS-2 (enc 1, char16_t); dim characters without the zero.</summary>
        struct Text {
            void*        ptr = nullptr;
            std::size_t  dim = 0;
            std::int32_t enc = 0;

            const char*     Latin1() const noexcept { return static_cast<const char*>(ptr); }
            const char16_t* Ucs2() const noexcept { return static_cast<const char16_t*>(ptr); }
        };

        /// <summary>FrameDataNode: one version 2 frame.</summary>
        struct Frame {
            std::uint32_t fid    = 0;
            char          lng[4] = {};
            Text          dsc;
            Text          txt;
        };

        std::int32_t FindFrame(std::uint32_t frameId, std::int32_t start) const noexcept;
        std::int32_t NewFrame() noexcept;
        template <typename C>
        std::int32_t Add(std::uint32_t frameId, const char* lng, const C* desc, const C* text) noexcept;
        void         CopyV1ToV2(std::uint32_t frameId, const char* text) noexcept;
        std::int32_t SetUserInfoLatin1(std::uint32_t frameId, const char* fieldValue) noexcept;
        std::int32_t SetUserInfoUcs2(std::uint32_t frameId, const char16_t* fieldValue) noexcept;
        std::int32_t SetGenreUtf16(const char16_t* text) noexcept;

        // id3tag_spec
        std::uint32_t flags_ = 0;
        std::int32_t  year_  = 0;
        Text          title_;
        Text          artist_;
        Text          album_;
        Text          comment_;
        std::int32_t  trackId3v1_       = 0;
        std::int32_t  genreId3v1_       = 0;
        std::byte*    albumArt_         = nullptr;
        std::uint32_t albumArtSize_     = 0;
        std::uint32_t paddingSize_      = 0;
        std::int32_t  albumArtMimeType_ = 0;
        Frame*        frames_           = nullptr; // on the heap, growing like LAME's list
        std::int32_t  frameCount_       = 0;
        std::int32_t  frameCapacity_    = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
