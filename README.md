# photos

A C++17 library for reading and writing photo metadata (Exif, IPTC and XMP),
built to replace [exiv2](https://exiv2.org) in a photo album app that ships on
Windows, macOS, Linux, iOS and Android. exiv2 is GPL; this library is
[MIT](LICENSE).

- **No required dependencies.** The sources build anywhere CMake and a C++17
  compiler do. zlib is optional; without it, the library skips compressed
  PNG text chunks.
- **exiv2's key names.** `Exif.Photo.DateTimeOriginal`,
  `Iptc.Application2.Keywords`, `Xmp.dc.subject`: code written against exiv2
  mostly ports by changing types, not strings. `tests/compare` checks the
  output against exiv2's.
- **Safe on untrusted files.** Every read is bounds-checked. IFD loops and
  nesting depth are limited, and XML entities are never expanded. The test
  suite truncates and mutates every test image under AddressSanitizer, and a
  libFuzzer target covers every reader.
- **Careful writing.** Only the metadata is rewritten; the image data is
  copied byte for byte. Unchanged Exif, or Exif whose edited values still fit
  where the old ones were, is written back in place, so maker notes keep their
  internal offsets. Multi-picture (MPF) JPEGs keep their extra images reachable. A file
  is replaced only after its new version has been written in full.

This repository also builds the iOS dependency packages the app links today;
see [docs/ios-deps.md](docs/ios-deps.md).

## Formats

| Format | Read | Write |
| --- | --- | --- |
| JPEG (and MPO) | Exif, IPTC, XMP, comment, ICC | Exif, IPTC, XMP, comment |
| PNG | Exif, IPTC, XMP, ICC | Exif, IPTC, XMP |
| WebP | Exif, XMP, ICC | Exif, XMP |
| TIFF and TIFF-based raw: DNG, CR2, NEF, ARW, ORF, RW2, PEF, SRW, ... | Exif, IPTC, XMP | not yet |
| HEIF/HEIC, AVIF | Exif, XMP | not yet |
| Canon CR3 | Exif, XMP | not yet |
| JPEG XL (container) | Exif, XMP | not yet |
| XMP sidecar (`.xmp`) | XMP | XMP |

For the read-only formats, write edits to an XMP sidecar (Lightroom and
darktable do the same). The `PhotoInfo` setters handle this automatically:
on an image that cannot hold Exif, they store the value in XMP's Exif
namespace instead.

## Using it

```cpp
#include <photos/photos.hpp>

auto image = photos::Image::open("IMG_0001.jpg");   // or open(bytes), open(source)
image->readMetadata();

// exiv2-style access
std::string model = image->exifData()["Exif.Image.Model"].toString();
auto* date = image->exifData().find("Exif.Photo.DateTimeOriginal");  // nullptr if absent
image->xmpData()["Xmp.dc.subject"] = std::vector<std::string>{"harbour", "boats"};
image->iptcData()["Iptc.Application2.City"] = "Copenhagen";

// or the album-level view, which knows where each value may live
photos::PhotoInfo info = photos::readPhotoInfo(*image);
if (info.dateTaken) std::cout << info.dateTaken->toIso8601() << "\n";   // 2024-05-17T18:42:07.250+02:00
photos::setRating(*image, 4);            // Xmp.xmp.Rating (+ Microsoft/Exif ratings present)
photos::setKeywords(*image, {"harbour"}); // Xmp.dc.subject (+ IPTC keywords present)
photos::setOrientation(*image, 6);       // Exif (+ Xmp.tiff.Orientation present)

image->writeMetadata();                  // to the same file, or buffer
```

`PhotoInfo` reads the date taken (with sub-seconds and UTC offset), the
orientation, the size, camera and lens, the exposure settings, GPS position,
title, description, keywords, creator, copyright and rating. It takes each
value from Exif, XMP or IPTC as the
[Metadata Working Group](https://en.wikipedia.org/wiki/Metadata_Working_Group)
advises, so photos from any camera or application read the same.

On Android, a photo from a content URI arrives as a file descriptor, and on
iOS often as `NSData`. Pass the bytes to `Image::open(Bytes)`, or subclass
`photos::InputSource` to read on demand. To save, call
`writeMetadata(OutputSink&)` with a sink that writes back.

An `Image` is not thread-safe; use one per thread. The XMP namespace registry
is thread-safe.

### From exiv2

| exiv2 | photos |
| --- | --- |
| `Exiv2::ImageFactory::open(path)` | `photos::Image::open(path)` |
| `Exiv2::XmpParser::initialize()` / `terminate()` | not needed |
| `image->exifData()["Exif.Image.Model"]` | the same |
| `datum.toString()`, `toInt64()`, `toFloat()`, `toRational()` | `toString()`, `toInt64()`, `toDouble()`, `toRational()` |
| `datum.typeId()`, `datum.count()` | the same (`photos::TypeId`) |
| `exifData.findKey(Exiv2::ExifKey(k))` | `exifData.find(k)` (a pointer) or `findKey(photos::ExifKey(k))` |
| `Exiv2::ExifThumb(exif).copy()` | `exifData.thumbnail()` |
| `Exiv2::XmpProperties::registerNs(uri, prefix)` | `photos::registerXmpNamespace(uri, prefix)` |
| `Exiv2::XmpParser::decode` / `encode` | `photos::XmpData::parse` / `serialize` |
| `Exiv2::Error` | `photos::Error` (with `code()`) |
| `image->pixelWidth()` | the same |

A few things work differently:

- IPTC values are UTF-8 strings. The 2-byte binary datasets
  (`RecordVersion` and a few others) read and write as decimal text.
- An XMP struct, or an array of structs, has its own kind (`XmpStruct`)
  where exiv2 reports `XmpText`.
- Values print raw (`1/250`, `6`). exiv2's interpreted output (`1/250 s`,
  `right, top`) and maker-note decoding are not implemented yet.

### Command line

`photos-meta` prints and edits metadata:

```
photos-meta photo.jpg                          # every key, type, count and value
photos-meta -p s photo.jpg                     # the PhotoInfo summary
photos-meta -M "set Xmp.xmp.Rating 5" -M "del Exif.GPSInfo.GPSLatitude" photo.jpg
```

## Building

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

| Option | Default | |
| --- | --- | --- |
| `PHOTOS_WITH_ZLIB` | `AUTO` | `ON`, `OFF` or `AUTO` (use zlib when found) |
| `PHOTOS_BUILD_TESTS` | on when top level | the unit tests |
| `PHOTOS_BUILD_TOOLS` | on when top level | `photos-meta` |
| `PHOTOS_BUILD_FUZZERS` | `OFF` | the libFuzzer target (Clang) |
| `PHOTOS_INSTALL` | on when top level | the install rules and CMake package |
| `PHOTOS_WARNINGS_AS_ERRORS` | `OFF` | |
| `BUILD_SHARED_LIBS` | `OFF` | only the public API is exported |

Consume it with `add_subdirectory` or, after `cmake --install`, with
`find_package(photos CONFIG REQUIRED)`. Either way, link `photos::photos`.

Cross-compiling works the usual way:

```bash
# iOS (device; use iphonesimulator for the Simulator)
cmake -S . -B build-ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DPHOTOS_BUILD_TESTS=OFF -DPHOTOS_BUILD_TOOLS=OFF
# Android
cmake -S . -B build-android -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
```

CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs these jobs:

- builds and tests with GCC, Clang, AppleClang and MSVC, and as a shared
  library;
- runs the tests under ASan and UBSan, as both 64-bit and 32-bit builds;
- compares the output with exiv2's;
- cross-compiles for iOS (device and both Simulator architectures) and for
  Android (arm64-v8a, armeabi-v7a, x86_64);
- fuzzes for two minutes.

## Tests

- `tests/unit`: the unit tests, over the images in `tests/data`. They cover
  the codecs, every format's reading and writing, the `PhotoInfo` layer, and
  robustness against truncated and mutated files. They use a small built-in
  harness, so they build on every platform the library does.
- `tests/compare/compare_exiv2.sh build/tools/photos-meta tests/data/*.jpg ...`
  compares every key, type, count and value with `exiv2 -Pkycv`. The only
  differences it allows are listed in the script, and are by design.
- `tests/data/generate.py` regenerates the test images with Pillow,
  pillow-heif and exiftool.
- `tests/find_package` builds against an installed package.

## Not yet done

This is a first cut of the framework. What exiv2 does that this library does
not do yet, roughly in order of what a photo album needs:

- writing HEIF/HEIC and AVIF (today: use a sidecar);
- maker notes: Canon, Nikon, Sony and other lens names, decoded (they are
  kept as `Exif.Photo.MakerNote` bytes, and survive edits);
- interpreted values (`exiv2 -pt` style);
- preview images inside raw files (exiv2's `PreviewManager`); the Exif
  thumbnail is available now;
- writing TIFF and raw files;
- extended XMP in JPEG, over 64 KB: it is read past and kept, but not merged
  into the XMP data;
- JPEG XL's Brotli-compressed boxes, and XMP in UTF-16;
- XMP qualifiers other than `xml:lang`;
- video formats.
