# lumenlib

A C++17 library for reading and writing photo metadata (Exif, IPTC and XMP),
and for reading what an MP4 or QuickTime movie says about itself and
writing a trimmed copy of one, built for the Lumen photo album app, which ships on Windows, macOS, Linux,
iOS and Android. It is [MIT](LICENSE)-licensed and written from the file
format specifications, so an application can link it without taking on
copyleft terms.

- **No required dependencies.** The sources build anywhere CMake and a C++17
  compiler do. zlib is optional; without it, compressed PNG text chunks and
  ICC profiles cannot be read (they are still written, uncompressed).
- **Keys from the standards.** Exif tags are named by IFD and by the name the
  TIFF, Exif or DNG specification gives them (`exif.DateTimeOriginal`,
  `gps.GPSLatitude`, `ifd0.Make`); IPTC datasets by their IIM names
  (`Keywords`, `CaptionAbstract`, `ByLine`); XMP properties by the XMP
  specification's own paths (`dc:subject`,
  `Iptc4xmpExt:LocationShown[1]/Iptc4xmpExt:City`).
- **Camera raw files, read and written.** TIFF-based raws (DNG, CR2, NEF,
  ARW, ORF, PEF, SRW) are rewritten without touching their image data or
  maker notes; RW2, CR3, RAF, HEIF and AVIF are read.
- **Maker notes decoded.** Canon, Nikon, Sony, Olympus / OM System,
  Panasonic, Pentax, Fujifilm, Samsung and Apple notes are decoded
  read-only, and `lensDescription()` finds the lens even when the standard
  tag is missing. A maker note that moves in an edit has its offsets
  corrected, so it stays readable.
- **Safe on untrusted files.** Every read is bounds-checked. IFD loops and
  nesting depth are limited, and XML entities are never expanded. The test
  suite truncates and mutates every test image and movie under
  AddressSanitizer, and libFuzzer targets cover every reader and writer.
- **Careful writing.** Only the metadata is rewritten; the image data is
  copied byte for byte. Unchanged Exif, or Exif whose edited values still fit
  where the old ones were, is written back in place. Multi-picture (MPF)
  JPEGs keep their extra images reachable. In a TIFF or raw file, removed
  metadata is overwritten, not just unlinked. A file is replaced only after
  its new version has been written in full.

Each release carries the library prebuilt for Windows, macOS and Linux, and
the iOS dependency packages the app links, lumenlib among them
([Releases](#releases), [docs/ios-deps.md](docs/ios-deps.md)).
[docs/migrating.md](docs/migrating.md) maps the app's current exiv2 calls
onto this library.

## Formats

| Format | Read | Write |
| --- | --- | --- |
| JPEG (and MPO) | Exif, IPTC, XMP, comment, ICC | Exif, IPTC, XMP, comment, ICC |
| PNG | Exif, IPTC, XMP, ICC | Exif, IPTC, XMP, ICC |
| WebP | Exif, XMP, ICC | Exif, XMP, ICC |
| TIFF and TIFF-based raw: DNG, CR2, NEF, ARW, ORF, PEF, SRW, ... | Exif, IPTC, XMP, ICC | Exif, IPTC, XMP, ICC |
| Panasonic RW2 | Exif, IPTC, XMP, ICC, maker note | not yet |
| Fujifilm RAF | Exif, IPTC, XMP, ICC | not yet |
| HEIF/HEIC, AVIF | Exif, XMP | not yet |
| Canon CR3 | Exif, XMP, maker note | not yet |
| JPEG XL (container) | Exif, XMP | not yet |
| XMP sidecar (`.xmp`) | XMP | XMP |
| MP4, QuickTime (MOV, M4V, 3GP) | the movie: times, length, tracks, tags ([below](#movies)) | a trimmed copy, its tags kept, without location, or none ([below](#movies)) |

For the read-only formats, write edits to an XMP sidecar (Lightroom and
darktable do the same), or, for a HEIF being encoded, pass
`ExifMetadata::encode()` and `XmpMetadata::serialize()` to the encoder. The
`PhotoInfo` setters handle sidecars automatically: on a file that cannot hold
Exif, they store the value in XMP's Exif namespace instead.

## Using it

```cpp
#include <lumenlib/lumenlib.hpp>

auto file = lumenlib::ImageFile::open("IMG_0001.jpg");  // or open(bytes), open(source)
file->load();

// Tag by tag
if (const auto* model = file->exif().find("ifd0.Model")) std::cout << model->text() << "\n";
if (const auto* t = file->exif().find("exif.ExposureTime")) std::cout << t->describe() << "\n";  // "1/250 s"
file->xmp().setItems("dc:subject", {"harbour", "boats"});
file->xmp().setLangText("dc:title", "x-default", "Harbour at dusk");
file->iptc().set("City", "Copenhagen");
file->exif().removeIf([](const lumenlib::ExifEntry& e) { return e.ifd() == lumenlib::Ifd::gps; });

// Or the album-level view, which knows where each value may live
lumenlib::PhotoInfo info = lumenlib::readPhotoInfo(*file);
if (info.dateTaken) std::cout << info.dateTaken->toIso8601() << "\n";   // 2024-05-17T18:42:07.250+02:00
lumenlib::setRating(*file, 4);             // xmp:Rating (+ Microsoft/Exif ratings present)
lumenlib::setKeywords(*file, {"harbour"});  // dc:subject (+ IPTC Keywords present)
lumenlib::setOrientation(*file, 6);        // ifd0.Orientation (+ tiff:Orientation present)

file->save();                              // to the same file, or buffer
```

`PhotoInfo` reads the date taken (with sub-seconds and UTC offset), the
orientation, the size, camera and lens, the exposure settings, GPS position,
title, description, keywords, creator, copyright and rating. It takes each
value from Exif, XMP or IPTC as the
[Metadata Working Group](https://en.wikipedia.org/wiki/Metadata_Working_Group)
advises, so photos from any camera or application read the same.

The pieces an application may need on their own:

- `ExifMetadata::encode()` / `decode()`: the TIFF block of an Exif segment,
  eXIf chunk or HEIF Exif item.
- `XmpMetadata::parse()` / `serialize(options)`: packets and sidecars, with
  or without the `<?xpacket?>` wrapper, in the compact attribute form if
  wanted. UTF-16 and UTF-32 packets are read too.
- `IptcMetadata::encode()` / `decode()`: IIM data.
- `detectFormat(path)`, `formatCanWrite(format, kind)`: what a file is and
  what can be written into it, without reading it.
- `ImageFile::setIccProfile()`, `clearMetadata()` (which keeps the ICC
  profile: it says how to read the pixels).
- `ExifMetadata::makerNote()`, `lensDescription()`: the maker note's entries
  and the lens.
- `setWarningHandler()`: the problems the library works around rather than
  fails on (a damaged XMP packet left out, a damaged IFD entry skipped).

On Android, a photo from a content URI arrives as a file descriptor, and on
iOS often as `NSData`. Pass the bytes to `ImageFile::open(Bytes)`, or subclass
`lumenlib::InputSource` to read on demand. To save, call
`saveTo(OutputSink&)` with a sink that writes back. Paths are
`std::filesystem::path`, so names outside ASCII work on Windows too.

An `ImageFile` is not thread-safe; use one per thread. Nothing needs setting
up first, and the XMP namespace registry and the warning handler are
thread-safe.

### Movies

`readMovie()` reads what an MP4 or QuickTime movie says about itself, from
its `moov` box alone; no sample is read or decoded, so no codec is involved:

```cpp
lumenlib::MovieInfo movie = lumenlib::readMovie("IMG_0002.MOV");  // or an InputSource
if (movie.created) std::cout << lumenlib::movieTimeToUnix(*movie.created) << "\n";  // mvhd, UTC
if (const auto* video = movie.firstTrack("vide")) {
  // "hvc1", 1920x1080 as stored, pixel aspect par_h:par_v, tkhd's matrix, 1000 / min_sample_ms fps
}
if (const auto* where = movie.item("com.apple.quicktime.location.ISO6709")) std::cout << *where << "\n";
```

The items are the QuickTime keys at `moov/meta` (Apple's) and
`moov/udta/meta` (FFmpeg's `use_metadata_tags`), iTunes-style items, and
`udta`'s own (`©xyz`, `©day`), as text, each with its `MovieItemKind`.

`trimMovie()` writes a part of one, again with no sample decoded: the
samples are copied, the sample tables cut, and an edit list starts the copy
on the very frame.

```cpp
lumenlib::MovieTrim trim;
trim.start_ms = 1200;
trim.end_ms = 5400;                                   // 0: to the end
trim.tags = lumenlib::MovieTags::noLocation;          // all, noLocation or none
lumenlib::trimMovie("IMG_0002.MOV", "IMG_0002 trimmed.MOV", trim);  // or InputSource -> OutputSink
```

[docs/video.md](docs/video.md) has the boxes read and written, and what a
trim keeps.

### Keys

| Family | Key | Examples |
| --- | --- | --- |
| Exif | `ifd.Name`, with the IFD one of `ifd0`, `exif`, `gps`, `interop`, `ifd1`; or the bare name, which finds its IFD | `ifd0.Make`, `exif.DateTimeOriginal`, `exif.PhotographicSensitivity`, `gps.GPSLatitude`, `DateTimeOriginal`, `ifd0.0xabcd` |
| IPTC | the IIM 4.2 dataset name, run together, or `record:dataset` | `Keywords`, `ObjectName`, `CaptionAbstract`, `ByLine`, `CopyrightNotice`, `CodedCharacterSet`, `2:25` |
| XMP | the XMP path: `prefix:name`, then `/prefix:name` for struct fields and `[n]` for array items | `dc:subject`, `xmp:Rating`, `exif:GPSLatitude`, `Iptc4xmpCore:Location`, `mwg-rs:Regions/mwg-rs:RegionList[1]/mwg-rs:Name` |
| Maker note | `group.Name` (read-only) | `canon.LensModel`, `nikon.Lens`, `olympus.equipment.LensModel` |

Exif values have TIFF types (`FieldType::u16` is a SHORT, `urational` a
RATIONAL) and print as the specification writes them; `describe()` gives the
meaning ("Rotated 90° clockwise", "Fired, auto mode", "F2.8").

### Command line

`lumen-meta` prints and edits metadata:

```
lumen-meta photo.jpg                              # every key, type, count and value
lumen-meta -p t photo.jpg                         # Exif, each value described
lumen-meta -p m photo.cr2                         # the maker note, and the lens
lumen-meta -p s photo.jpg                         # the PhotoInfo summary
lumen-meta -M "set xmp xmp:Rating 5" -M "del exif gps.GPSLatitude" photo.jpg
lumen-meta -M "add iptc Keywords harbour" photo.jpg
```

## Building

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

| Option | Default | |
| --- | --- | --- |
| `LUMENLIB_WITH_ZLIB` | `AUTO` | `ON`, `OFF` or `AUTO` (use zlib when found) |
| `LUMENLIB_BUILD_TESTS` | on when top level | the unit tests |
| `LUMENLIB_BUILD_TOOLS` | on when top level | `lumen-meta` |
| `LUMENLIB_BUILD_FUZZERS` | `OFF` | the libFuzzer targets (Clang) |
| `LUMENLIB_INSTALL` | on when top level | the install rules and CMake package |
| `LUMENLIB_WARNINGS_AS_ERRORS` | `OFF` | |
| `BUILD_SHARED_LIBS` | `OFF` | only the public API is exported |

Consume it with `add_subdirectory` or, after `cmake --install`, with
`find_package(lumenlib CONFIG REQUIRED)`. Either way, link
`lumenlib::lumenlib`.

Cross-compiling works the usual way:

```bash
# iOS (device; use iphonesimulator for the Simulator)
cmake -S . -B build-ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DLUMENLIB_BUILD_TESTS=OFF -DLUMENLIB_BUILD_TOOLS=OFF
# Android
cmake -S . -B build-android -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
```

CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs these jobs:

- builds and tests with GCC, Clang, AppleClang and MSVC, and as a shared
  library;
- runs the tests under ASan and UBSan, as both 64-bit and 32-bit builds;
- compares the output with an independent reader's, exiv2's;
- cross-compiles for iOS (device and both Simulator architectures) and for
  Android (arm64-v8a, armeabi-v7a, x86_64);
- fuzzes the image readers and writers for two minutes, and the movie
  reader and trim for two more.

## Releases

`VERSION` is the repository's one version: the library's (`project()` reads
it) and every release package's. Publishing a GitHub release tagged `vX.Y.Z`,
matching it, runs [`.github/workflows/release.yml`](.github/workflows/release.yml),
which attaches these with a `SHA256SUMS` file:

| Package | Holds | Built with |
| --- | --- | --- |
| `lumenlib-X.Y.Z-windows-x86_64-clang64.zip` | lumenlib | MSYS2 CLANG64 (clang, libc++) |
| `lumenlib-X.Y.Z-linux-x86_64.tar.gz` | lumenlib | Clang on Ubuntu 26.04 |
| `lumenlib-X.Y.Z-macos-arm64.tar.gz`, `-macos-x86_64.tar.gz` | lumenlib | Xcode, for macOS 12 and later |
| `lumen-ios-deps-X.Y.Z-{arm64,sim-arm64,sim-x86_64}.tar.gz` | lumenlib, with the other iOS libraries Lumen links | [docs/ios-deps.md](docs/ios-deps.md) |

The lumenlib packages are `cmake --install` trees of the static library,
built with zlib: point `CMAKE_PREFIX_PATH` at one and
`find_package(lumenlib CONFIG REQUIRED)`, which finds zlib too. The Windows
package links only with MSYS2 CLANG64, not MSVC or MINGW64. Each is tested,
installed, built against by `tests/find_package`, and checked for absolute
build paths before it is packaged.

## Tests

- `tests/unit`: the unit tests, over the images in `tests/data` and
  synthetic maker notes. They cover the codecs, every format's reading and
  writing, the maker notes, the `PhotoInfo` layer, and robustness against
  truncated and mutated files. They use a small built-in harness, so they
  build on every platform the library does.
- With `LUMENLIB_SAMPLES` naming a folder of real camera files, the tests
  also read each one, print its camera and lens, and edit and rewrite each
  TIFF-based raw, checking its image data and maker note survive. The files
  `docs/migrating.md` lists (from [raw.pixls.us](https://raw.pixls.us), CC0)
  all pass.
- `tests/compare/compare_exiv2.sh build/tools/lumen-meta tests/data/*.jpg ...`
  compares every Exif and IPTC tag (by number), XMP property, type, count and
  value with exiv2's reading, as a second opinion from an independent reader.
  exiv2 is only run as a program; nothing here links it. The only
  differences it allows are listed in the script, and are by design.
- `tests/data/generate.py` regenerates the test images with Pillow,
  pillow-heif and exiftool; `tests/data/make_movies.py` the test movies, box
  by box, with Python alone.
- `tests/find_package` builds against an installed package.

## Not yet done

What a photo album might still want, roughly in order:

- writing HEIF/HEIC, AVIF, CR3, RAF and RW2 (today: a sidecar, or the
  encoder). An RW2 repeats its metadata in the JPEG it carries, which would
  have to be rewritten too, so nothing removed from it lingers;
- lens names from makers' numeric lens codes (Canon LensType, Sony
  LensType, Pentax LensType, Nikon LensID): today the lens is the maker's
  own text or the focal and aperture range;
- editing maker note entries (they are read-only; the note is kept as a
  whole);
- preview images inside raw files; the Exif thumbnail is available now;
- extended XMP in JPEG, over 64 KB: it is read past and kept, but not merged
  into the XMP data;
- JPEG XL's Brotli-compressed boxes;
- XMP qualifiers other than `xml:lang`;
- fragmented MP4s' samples (`moof`), for reading and trimming; Matroska and
  WebM.
