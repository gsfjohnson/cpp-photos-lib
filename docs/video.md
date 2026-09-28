# Movies: reading MP4 and MOV, and trimming them

lumenlib reads photos' metadata. This page covers two additions for the video
files Lumen keeps beside them, MP4 and QuickTime (MOV, M4V, 3GP): reading what
the container says about a movie (built, 0.2.0: `<lumenlib/movie.hpp>`), and
writing a trimmed copy of one (planned). Neither decodes a sample, so both
stay within lumenlib's rules: no required dependencies, every read
bounds-checked, and no codec in the library, patented or otherwise.

The reasons are in Lumen's `pm/ADD_CODEC_LICENSING.md` (§7–§8 in
`../cpp-photos`). In short, Lumen is moving its video work off FFmpeg: onto
AVFoundation on Apple platforms, Media Foundation on Windows, and whatever
FFmpeg the system has on Linux. None of the three reads a movie's metadata
alike, and Media Foundation cannot write a frame-exact trim at all. One
container reader and one trim writer in lumenlib make every device agree.

## Reading

What Lumen's `VideoProbe` needs, all from the `moov` box:

| Box | Gives |
| --- | --- |
| `mvhd` | creation and modification times (seconds since 1904), timescale, duration |
| `trak/tkhd` | the display matrix (turns and mirrors), the track's size |
| `trak/mdia/hdlr` | the track's kind: `vide`, `soun`, `meta` |
| `trak/mdia/mdhd` | the track's timescale and duration |
| `stsd` sample entry | the codec (`avc1`/`avc3`, `hvc1`/`hev1`, `av01`, `vp09`, `mp4a`, …), the stored width and height, `pasp` (pixel aspect) |
| `stts` | sample durations: the shortest is the frame rate Lumen reports |
| `moov/meta` (`keys` + `ilst`) | Apple's `mdta` keys: `com.apple.quicktime.creationdate` (with its UTC offset), `.location.ISO6709`, `.make`, `.model` |
| `moov/udta/meta` | the same keys where FFmpeg writes them (`-movflags use_metadata_tags`) |
| `moov/udta` | `©xyz` (Android's ISO 6709 place), `©day`, and the other `©` items |

As built (`src/movie.cpp`), the reader shares `src/bmff_boxes.hpp`'s walk with
the HEIF and CR3 reader (box bounds, the box budget, 64-bit sizes), and has
replaced Lumen's `video/movie_header.cpp` reader. What it settled:

- **The top level is lenient, the movie box is not.** A file cut short in its
  `mdat`, or with junk after a whole `moov`, still reads; a `moov` that runs
  past the end, or holds no `mvhd`, is `corruptData`. Data that does not begin
  with an ISO box is `unsupportedFormat`, and so is an ISO file with no movie
  box (a HEIF photo).
- **A damaged tag loses only itself**, with a warning: the tracks and times
  still read. A damaged sample entry's own boxes (`pasp`, `sinf`) are skipped
  the same way.
- **The shortest sample leaves out a lone last one** (an `stts` entry of one
  sample, when there are others): writers give the last sample whatever time
  is left, which would make a 30 fps clip read as 73 fps.
- **An encrypted entry** (`encv`, `enca`) is named by its `sinf/frma`.
- **Items are text**: UTF-8 and UTF-16 values, integers and floats (types
  1, 2, 21, 22, 23, 24); pictures and the implicit type are left out. An
  iTunes `----` item is named by its `name`. QuickTime user data text in a
  Mac language code that is not UTF-8 is read as Latin-1.
- **A fragmented movie** (`mvex`) says so (`MovieInfo::fragmented`); its
  length is `mehd`'s when `mvhd` has none. Its samples, in `moof` boxes, are
  not read, so its tracks have no sample durations.

Out of scope: the samples of a fragmented MP4 (`moof`), and Matroska/WebM,
whose tags are EBML and not boxes.

## Trimming

A copy of `[start, end)` of the movie, with no sample decoded:

- **Samples, per track.** Video starts at the sync sample (`stss`) at or
  before `start`, and ends at the first sample presented at or after `end`,
  plus whatever frame reordering (`ctts`) needs. Audio and timed-metadata
  tracks keep the samples that cover the range.
- **Tables.** `stts`, `ctts`, `stss`, `stsc`, `stsz`, and `stco` (`co64` once
  the output passes 4 GiB) are rewritten, as are `sdtp` and `sgpd`/`sbgp` when
  present. The durations in `mvhd`, `tkhd` and `mdhd` are updated.
- **An exact start.** Each track gets an edit list (`elst`) whose media time
  skips from the sync sample to `start`, so the copy plays from the very
  frame. An edit list already in the input (encoder priming, an earlier
  trim) is composed with the new one, not replaced.
- **Tags.** Keep all, keep all but location, or keep none. "Location" means
  the ISO 6709 keys, `©xyz`, `loci`, and timed-metadata tracks whose keys name
  a place. The display matrix is not a tag and always goes along. The
  original's `mvhd` creation time goes along too, since a passthrough export
  that stamps the copy with the time it was made loses the date (Lumen,
  Phase 50).
- **Output.** The input's own brand and container: a MOV stays a MOV. One
  `mdat`, with samples interleaved by time. The copy is written through an
  `OutputSink`, and a file is replaced only after its new version has been
  written in full, as lumenlib's other writers do.

## Shape

The reader is as below (`include/lumenlib/movie.hpp` has the whole of it:
also a track's id, flag, timescale and sample count, and the movie's brand
and fragmented flag). The trim is not final; it follows `ImageFile`'s
conventions (paths, `InputSource`, `Error` on failure):

```cpp
// <lumenlib/movie.hpp>
namespace lumenlib {

struct MovieTrack {
  std::string handler;           // "vide", "soun", "meta"
  std::string codec;             // the sample entry's type: "hvc1", "mp4a"...
  std::uint32_t width = 0;       // as stored, before the matrix
  std::uint32_t height = 0;
  std::uint32_t par_h = 1, par_v = 1;
  std::int32_t matrix[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
  std::uint64_t duration_ms = 0;
  double min_sample_ms = 0;      // the shortest sample: 1000 / frame rate
};

struct MovieInfo {
  std::optional<std::uint64_t> created;   // mvhd, seconds since 1904
  std::optional<std::uint64_t> modified;
  std::uint64_t duration_ms = 0;
  std::vector<MovieTrack> tracks;
  std::vector<MovieItem> items;  // {kind, name, value}: keys, iTunes items, udta's © items
};

MovieInfo readMovie(const std::filesystem::path& path);
MovieInfo readMovie(InputSource& source);

enum class MovieTags { all, noLocation, none };
void trimMovie(InputSource& in, OutputSink& out, std::int64_t start_ms, std::int64_t end_ms,
               MovieTags tags);

}  // namespace lumenlib
```

## Tests

- **Fixtures**, generated like `tests/data/` (small, synthetic, checked in):
  `make_movies.py` writes, box by box, an iPhone-style MOV with `mdta` keys
  and a timed-metadata track, turned; an Android MP4 with `©xyz`; an FFmpeg
  MP4 with `use_metadata_tags`; a mirrored clip with iTunes items; an
  anamorphic, encrypted clip with version 1 headers; and a fragmented one
  (`tests/unit/test_movie.cpp`). The trim will need one with B-frames
  (`ctts`) and one with an edit list already in it, with real samples.
- **Round trips.** Trim, read the copy back, and compare the tables, the edit
  list and the tags. Lumen's own suites check the first frame decodes to the
  original's frame at `start`.
- **Robustness.** `fuzz_movie` covers the reader (and will cover the trim),
  and the robustness test truncates every movie fixture at every length and
  mutates each 2000 times under AddressSanitizer, as it does the images.
