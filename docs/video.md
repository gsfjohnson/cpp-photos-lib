# Movies: reading MP4 and MOV, and trimming them (planned)

lumenlib reads photos' metadata today. This page plans two additions for the
video files Lumen keeps beside them, MP4 and QuickTime (MOV, M4V, 3GP):
reading what the container says about a movie, and writing a trimmed copy of
one. Neither decodes a sample, so both stay within lumenlib's rules: no
required dependencies, every read bounds-checked, and no codec in the library,
patented or otherwise.

Nothing here is built yet. The reasons are in Lumen's
`pm/ADD_CODEC_LICENSING.md` (§7–§8 in `../cpp-photos`). In short, Lumen is
moving its video work off FFmpeg: onto AVFoundation on Apple platforms, Media
Foundation on Windows, and whatever FFmpeg the system has on Linux. None of
the three reads a movie's metadata alike, and Media Foundation cannot write a
frame-exact trim at all. One container reader and one trim writer in lumenlib
make every device agree.

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

This takes over the part of `src/bmff.cpp`'s walk that HEIF and CR3 already
use (box bounds, the box budget, 64-bit sizes). It also replaces Lumen's
`video/movie_header.cpp`, which reads `mvhd` and the keys on its own today.

Out of scope at first: fragmented MP4 (`moof`), and Matroska/WebM, whose tags
are EBML and not boxes.

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

Not final; it follows `ImageFile`'s conventions (paths, `InputSource`,
`Error` on failure):

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
  std::vector<std::pair<std::string, std::string>> items;  // keys and © items, by name
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
  an iPhone-style MOV with `mdta` keys and a location track, an Android MP4
  with `©xyz`, an FFmpeg MP4 with `use_metadata_tags`, a turned clip, a
  mirrored clip, an anamorphic clip, one with B-frames (`ctts`), and one with
  an edit list already in it.
- **Round trips.** Trim, read the copy back, and compare the tables, the edit
  list and the tags. Lumen's own suites check the first frame decodes to the
  original's frame at `start`.
- **Robustness.** A `fuzz_movie` target covers both the reader and the trim,
  and the robustness test truncates and mutates every movie fixture under
  AddressSanitizer, as it does the images.
