# Movies: reading MP4 and MOV, and trimming them

lumenlib reads photos' metadata. This page covers two additions for the video
files Lumen keeps beside them, MP4 and QuickTime (MOV, M4V, 3GP): reading what
the container says about a movie (built, 0.2.0: `<lumenlib/movie.hpp>`), and
writing a trimmed copy of one (built, 0.3.0: `trimMovie`). Neither decodes a
sample, so both stay within lumenlib's rules: no required dependencies, every read
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
| `trak/edts/elst` | the edit list (0.4.0): Media Foundation reports media times and ignores it, so Lumen's Windows engine maps a time through it |
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

A copy of `[start, end)` of the movie, with no sample decoded. As built
(`src/movie_trim.cpp`):

- **Whole frames.** The range is widened to whole pictures of the picture
  track (the first enabled video track with samples): it starts where the
  picture showing at `start` begins and ends where the one showing just
  before `end` ends. Players disagree over a picture an edit list cuts into
  (FFmpeg drops it, AVFoundation shows it) and agree over one it holds
  whole. The picture track's edit starts on that picture's own composition
  time, which a movie timescale coarser than the frames would miss.
- **Samples, per track.** Each track's edit list, or the one it implies,
  is cut to the range: that is the new edit list, and the media times it
  shows. The samples those need are one run in decode order, from the sync
  sample (`stss`; every sample without one) at or before the first shown to
  the last one shown, which holds whatever frame reordering (`ctts`) needs.
  Sound keeps one sample more before the first heard, for the decoder's
  overlap (the edit list skips it). A track that shows nothing in the range
  is left out, and so are hint tracks, which point into other tracks'
  samples by offset.
- **Tables.** `stts`, `ctts`, `stss`, `stps`, `stsz` (an `stz2` is written
  as one), `sdtp` and `sbgp` are cut to the run; `stsc` and `stco` (`co64`
  when an offset passes 4 GiB) are written for the new chunks; `cslg` is
  measured again; `stsd` and `sgpd` are copied. `subs`, `saiz`, `saio`,
  `stsh`, `padb` and `stdp` are left out. `mvhd`, `tkhd` and `mdhd` get the
  new durations.
- **An exact start.** Each track gets an edit list whose media time skips
  from its first sample to the start. An edit list already in the input
  (encoder priming, an empty edit, an earlier trim) is cut, not replaced:
  its empty edits stay empty, and its pieces of media stay in their order.
  A picture shown before it is decoded (a negative composition offset)
  moves every offset later rather than ask for an edit before the media.
- **Chunks.** The input's chunks, cut to each run, are copied in the order
  they lie in the file, so the copy is interleaved as the original was, and
  read front to back.
- **Tags.** `all` keeps every tag and box. `noLocation` takes out every key
  and item whose name speaks of a place (`location`, `ISO6709`, `gps`,
  `©xyz`...), XMP (`XMP_`, `uuid`, a meta's `xml `), and from `udta` all but
  QuickTime's `©` text items (not `©xyz`) and 3GPP's text boxes (not
  `loci`); timed-metadata tracks unless every key in their `mebx` sample
  entries is known and none names a place (a GoPro's `gpmd` or a `camm`
  track is dropped: its samples may be coordinates), and text and subtitle
  tracks, whose samples may carry a place (a drone's). `none` keeps no
  `meta` or `udta` box, no timed-metadata, text or subtitle track, and sets
  every creation and modification time to 0. The display matrix is not a
  tag: it always goes along, and so, unless the tags are `none`, does the
  original's `mvhd` creation time (a passthrough export that stamps the copy
  with the time it was made loses the date: Lumen, Phase 50). A track
  reference to a track left out is taken out with it.
- **Output.** The input's `ftyp` (and, with `all`, its top-level `uuid` and
  `meta` boxes), then the movie box, then one `mdat` (a 64-bit one past
  4 GiB). The copy is written through an `OutputSink`; the path overload
  writes beside the output and renames it into place only once whole, so
  `out` may be `in`.
- **Refused.** A fragmented movie (`mvex`, `moof`), an encrypted track
  (`encv`, `enca`...), samples in another file (`dref`), and an edit that
  changes speed (unsupportedOperation); a range that holds none of the movie
  (invalidArgument); tables that disagree with each other, or samples
  outside the file (corruptData). `progress` is told the bytes copied and
  may stop the copy (cancelled).

## Shape

In short (`include/lumenlib/movie.hpp` has the whole of it: also a track's
id, flag, timescale and sample count, and the movie's brand and fragmented
flag):

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
MovieInfo readMovie(const InputSource& source);

enum class MovieTags { all, noLocation, none };
struct MovieTrim {
  std::uint64_t start_ms = 0;
  std::uint64_t end_ms = 0;  // 0: to the end
  MovieTags tags = MovieTags::all;
  std::function<bool(std::uint64_t done, std::uint64_t total)> progress;  // false: stop
};
struct TrimmedMovie { std::uint64_t duration_ms = 0; };

TrimmedMovie trimMovie(const InputSource& in, OutputSink& out, const MovieTrim& trim);
TrimmedMovie trimMovie(const std::filesystem::path& in, const std::filesystem::path& out,
                       const MovieTrim& trim);

}  // namespace lumenlib
```

## Tests

- **Fixtures**, generated like `tests/data/` (small, synthetic, checked in):
  `make_movies.py` writes, box by box, an iPhone-style MOV with `mdta` keys
  and a timed-metadata track, turned; an Android MP4 with `©xyz`; an FFmpeg
  MP4 with `use_metadata_tags`; a mirrored clip with iTunes items; an
  anamorphic, encrypted clip with version 1 headers; and a fragmented one
  (`tests/unit/test_movie.cpp`). For the trim, two with real sample tables,
  each sample marked with its track and number: an FFmpeg-style MP4 with
  B-frames (`ctts`), AAC priming, `sdtp` and a sample grouping, and a MOV
  with an edit list already in it (an empty edit, media out of order), a
  timed-metadata track naming a location, a chapter track (`stz2`) and
  64-bit chunk offsets (`tests/unit/test_movie_trim.cpp`).
- **Round trips.** Trim, read the copy back box by box, and compare which
  samples it holds (byte for byte), the tables, the edit lists and the
  tags; trim a copy again. Lumen's own suites decode the copies on both of
  its engines and check the first frame is the original's frame at `start`.
- **Robustness.** `fuzz_movie` covers the reader and the trim, and the
  robustness test truncates every movie fixture at every length and mutates
  each 2000 times, reading and trimming each under AddressSanitizer, as it
  does the images.
