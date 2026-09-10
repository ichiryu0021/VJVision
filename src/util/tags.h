// Minimal embedded-tag reader (no external dependency — TagLib is still
// planned for M5 if broader format support is needed).
//
// Supported:
//   * MP3  : ID3v2.3 / ID3v2.4 tags at file start (APIC cover,
//            TIT2/TPE1/TALB text frames; syncsafe sizes; UTF-16/UTF-8 text)
//   * FLAC : native PICTURE block + VORBIS_COMMENT (TITLE/ARTIST/ALBUM)
//   * MP4  : moov/udta/meta/ilst covr atom + ©nam/©ART/©alb text atoms
//   * AIFF : 'ID3 ' chunk containing an ID3v2 tag
//   * WAV  : 'id3 ' chunk containing an ID3v2 tag
//
// Only the metadata region is read (never the whole audio stream).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vj {

struct AudioTags {
    bool valid = false;
    std::string title;
    std::string artist;
    std::string album;
    std::vector<uint8_t> coverData;
    std::string coverExt;   // "jpg" / "png" / "" when no cover
};

// Returns tags; coverData is empty when the file has no embedded picture.
AudioTags readAudioTags(const std::string& path);

} // namespace vj
