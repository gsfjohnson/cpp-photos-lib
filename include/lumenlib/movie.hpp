// What an MP4 or QuickTime movie (MOV, M4V, 3GP) says about itself, read
// from its `moov` box: the movie header's times and duration, each track's
// kind, codec, size, pixel aspect, display matrix and timing, and the tags —
// QuickTime metadata keys, iTunes-style items and user data — and a trimmed
// copy of one. No sample is decoded.
#pragma once

#include <lumenlib/export.hpp>
#include <lumenlib/io.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lumenlib {

struct LUMENLIB_EXPORT MovieTrack {
  std::uint32_t id = 0;
  bool enabled = true;           // tkhd's flag
  std::string handler;           // the media handler: "vide", "soun", "meta", "text"...
  std::string codec;             // the first sample entry's type: "avc1", "hvc1", "mp4a"...;
                                 // for an encrypted one ("encv", "enca") the original's
  std::uint32_t width = 0;       // a picture's size as stored, before the matrix
  std::uint32_t height = 0;      // and the pixel aspect
  std::uint32_t par_h = 1;       // the pixel aspect (pasp): a pixel is par_h / par_v wide
  std::uint32_t par_v = 1;
  // tkhd's display matrix, in the file's order: a, b, u, c, d, v, x, y, w
  // (16.16 fixed point, and 2.30 for u, v and w).
  std::int32_t matrix[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
  std::uint32_t timescale = 0;   // mdhd's
  std::uint64_t duration_ms = 0; // mdhd's duration: the media's, before any edit list
  std::uint64_t samples = 0;     // how many samples stts counts
  // The shortest sample, in milliseconds (1000 / the frame rate of a video
  // track); a track's last sample is left out when others differ from it,
  // since writers give it whatever is left. 0 when the track has no samples.
  double min_sample_ms = 0;
};

// A tag, by where it was found.
enum class MovieItemKind {
  key,       // a QuickTime metadata key (hdlr "mdta"), at moov/meta or moov/udta/meta
  itunes,    // an iTunes-style item (hdlr "mdir"), named by its box type or a "----" item's name
  userData,  // one of udta's own items: QuickTime text ("©xyz", "©day")
};

struct LUMENLIB_EXPORT MovieItem {
  MovieItemKind kind = MovieItemKind::key;
  std::string name;
  std::string value;
};

struct LUMENLIB_EXPORT MovieInfo {
  std::string brand;                      // ftyp's major brand: "qt  ", "isom", "mp42"...
  std::optional<std::uint64_t> created;   // mvhd, seconds since 1904-01-01T00:00:00Z; absent when 0
  std::optional<std::uint64_t> modified;
  std::uint32_t timescale = 0;            // mvhd's
  // The movie's length: mvhd's, which counts every track's edit list; the
  // longest track's when mvhd says 0.
  std::uint64_t duration_ms = 0;
  bool fragmented = false;                // has an mvex: its samples are in moof boxes
  std::vector<MovieTrack> tracks;
  // The tags with text or a number for a value, in this order: `moov/meta`'s
  // QuickTime keys ("com.apple.quicktime.make"), then `moov/udta/meta`'s
  // (keys, or iTunes items named by their box type, "©too"), then `udta`'s
  // own items ("©xyz", "©day"). A name may repeat; the first is the one a
  // reader should take. A box type's 0xA9 byte is spelt "©" in UTF-8.
  std::vector<MovieItem> items;

  // The first track with this handler, or null.
  const MovieTrack* firstTrack(std::string_view handler) const noexcept;
  // The value of the first item with this name, or null.
  const std::string* item(std::string_view name) const noexcept;
};

// Reads a movie. Throws Error(unsupportedFormat) when the data is not an
// ISO media file with a movie box (a Matroska or AVI file, a photo), and
// Error(corruptData) when its movie box is damaged.
LUMENLIB_EXPORT MovieInfo readMovie(const InputSource& source);
LUMENLIB_EXPORT MovieInfo readMovie(const std::filesystem::path& path);

// Seconds since 1904 (a movie header's) as seconds since 1970.
constexpr std::int64_t movieTimeToUnix(std::uint64_t seconds) noexcept {
  return static_cast<std::int64_t>(seconds) - 2082844800;
}

// Which of a movie's tags a trimmed copy keeps. Each keeps less than the one
// before it. The display matrix is not a tag: it always goes along.
enum class MovieTags {
  all,         // every tag, and the movie header's times
  // Every tag but those that could say where: keys and items whose name
  // speaks of a location, udta's ©xyz and loci, XMP, and any other udta box
  // that is not plain text; timed-metadata tracks unless every key of theirs
  // is known and none names a location, and text and subtitle tracks, whose
  // samples may carry a place (a drone's). The times stay.
  noLocation,
  // No tag at all: no meta or udta box, no timed-metadata, text or subtitle
  // track, and every creation and modification time 0.
  none,
};

// A trimmed copy of the part of a movie from start_ms to end_ms.
struct LUMENLIB_EXPORT MovieTrim {
  std::uint64_t start_ms = 0;
  std::uint64_t end_ms = 0;  // 0: to the end
  MovieTags tags = MovieTags::all;
  // Told the bytes of samples written so far and how many there will be, as
  // the copy goes; returning false stops it with Error(cancelled).
  std::function<bool(std::uint64_t done, std::uint64_t total)> progress;
};

struct LUMENLIB_EXPORT TrimmedMovie {
  std::uint64_t duration_ms = 0;  // the copy's, as its movie header says
};

// Writes the part of an MP4 or QuickTime movie that `trim` names, with no
// sample decoded. The range is widened to whole pictures of the video: it
// starts where the picture showing at start_ms begins and ends where the one
// showing just before end_ms ends, since players disagree over a picture an
// edit list cuts into. Each track's samples, from the sync sample at or
// before the start (and, for sound, one before that, for the decoder's
// overlap) to the last one shown before the end, whatever frame reordering
// needs, are copied into one media data box after the movie box, and each
// track gets an edit list that shows exactly that range, composed with any
// it had. The copy keeps
// the original's brand, its tracks' sample descriptions, its display matrix
// and, unless the tags are none, its times; `trim.tags` says which tags go
// along. A track with nothing to show in the range is left out.
//
// Throws Error(unsupportedFormat) for data that is not an ISO movie,
// Error(unsupportedOperation) for one this cannot copy (fragmented,
// encrypted, an edit that changes speed), Error(invalidArgument) when the
// range holds nothing of the movie, Error(corruptData) for damaged tables
// or samples outside the file, and Error(cancelled) when `progress` says to
// stop; `out` then holds a partial copy, which the caller discards.
LUMENLIB_EXPORT TrimmedMovie trimMovie(const InputSource& in, OutputSink& out, const MovieTrim& trim);
// The same between files: `out` is written beside itself and renamed into
// place only once whole, so a failure leaves any file there as it was. `out`
// may be `in`.
LUMENLIB_EXPORT TrimmedMovie trimMovie(const std::filesystem::path& in, const std::filesystem::path& out,
                                       const MovieTrim& trim);

}  // namespace lumenlib
